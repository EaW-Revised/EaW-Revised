#include "eawr/presentation/ui/hud.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eawr::presentation::ui {
namespace {

bool equal_name(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a'-'A')) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](char x, char y) { return lower(x) == lower(y); });
}
core::Diagnostic diagnostic(std::string code, std::string message, std::string path = {}) {
    core::Diagnostic out;
    out.code = std::move(code);
    out.severity = core::Severity::warning;
    out.message = std::move(message);
    out.logical_path = std::move(path);
    return out;
}
void include(std::optional<data::ui::ReferenceRect>& extent, const data::ui::ReferenceRect rect) {
    if (!extent) { extent = rect; return; }
    const float left = std::min(extent->x, rect.x), bottom = std::min(extent->y, rect.y);
    const float right = std::max(extent->right(), rect.right()), top = std::max(extent->top(), rect.top());
    extent = data::ui::ReferenceRect{left, bottom, right-left, top-bottom};
}
bool contains(const data::ui::ReferenceRect& r, const ReferencePoint p) {
    return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.top();
}
struct Point { double x, y, u, v; };
using Polygon = std::vector<Point>;
// Clip in UV space, carrying the corresponding shell position along each edge.
// This includes texel area, not merely texel centres, even on slanted triangles.
Polygon clip(const Polygon& input, bool u_axis, double edge, bool greater) {
    Polygon out;
    if (input.empty()) return out;
    const auto coordinate = [=](const Point& p) { return u_axis ? p.u : p.v; };
    const auto inside = [=](const Point& p) { return greater ? coordinate(p) >= edge : coordinate(p) <= edge; };
    Point previous = input.back();
    for (const auto current : input) {
        if (inside(previous) != inside(current)) {
            const double t = (edge-coordinate(previous))/(coordinate(current)-coordinate(previous));
            out.push_back({previous.x+t*(current.x-previous.x), previous.y+t*(current.y-previous.y),
                previous.u+t*(current.u-previous.u), previous.v+t*(current.v-previous.v)});
        }
        if (inside(current)) out.push_back(current);
        previous = current;
    }
    return out;
}
void opaque_extent(std::optional<data::ui::ReferenceRect>& extent, const HudFaceplate& face) {
    Polygon polygon;
    for (const auto& vertex : face.triangle.vertices)
        polygon.push_back({vertex.position.x, vertex.position.y, vertex.uv.x, vertex.uv.y});
    const auto& mask = *face.mask;
    double u0 = polygon[0].u, u1 = u0, v0 = polygon[0].v, v1 = v0;
    for (const auto& p : polygon) {
        u0 = std::min(u0,p.u); u1 = std::max(u1,p.u);
        v0 = std::min(v0,p.v); v1 = std::max(v1,p.v);
    }
    // Enumerate repeated texture tiles and opaque horizontal runs. Work scales
    // with texture rows/runs rather than every pixel of a large solid faceplate.
    for (double tile_v = std::floor(v0); tile_v <= std::floor(v1); ++tile_v) {
        for (std::uint32_t y = 0; y < mask.height; ++y) {
            const double bottom = tile_v + static_cast<double>(y)/mask.height;
            const double top = tile_v + static_cast<double>(y+1U)/mask.height;
            if (top < v0 || bottom > v1 || (v0 == v1 && top == v0)) continue;
            for (double tile_u = std::floor(u0); tile_u <= std::floor(u1); ++tile_u) {
                for (std::uint32_t x = 0; x < mask.width;) {
                    if (mask.alpha[static_cast<std::size_t>(y)*mask.width+x] == 0) { ++x; continue; }
                    const auto begin = x++;
                    while (x < mask.width && mask.alpha[static_cast<std::size_t>(y)*mask.width+x] != 0) ++x;
                    const double left = tile_u + static_cast<double>(begin)/mask.width;
                    const double right = tile_u + static_cast<double>(x)/mask.width;
                    if (right < u0 || left > u1 || (u0 == u1 && right == u0)) continue;
                    auto cut = clip(clip(clip(clip(polygon,true,left,true),true,right,false),false,bottom,true),false,top,false);
                    if (cut.size() < 3U) continue;
                    // Reject zero-area boundary intersections at repeat seams.
                    double area = 0;
                    for (std::size_t i = 0; i < cut.size(); ++i) {
                        const auto& a = cut[i]; const auto& b = cut[(i+1U)%cut.size()];
                        area += a.x*b.y-b.x*a.y;
                    }
                    if (std::abs(area) < 1e-12) continue;
                    for (const auto& p : cut) include(extent, {static_cast<float>(p.x),static_cast<float>(p.y),0,0});
                }
            }
        }
    }
}
bool face_hit(const HudFaceplate& face, const ReferencePoint p) {
    const auto& a = face.triangle.vertices[0];
    const auto& b = face.triangle.vertices[1];
    const auto& c = face.triangle.vertices[2];
    const double ax = a.position.x, ay = a.position.y;
    const double bx = b.position.x, by = b.position.y, cx = c.position.x, cy = c.position.y;
    const double denominator = (by-cy)*(ax-cx)+(cx-bx)*(ay-cy);
    if (std::abs(denominator) < 1e-12) return false;
    const double wa = ((by-cy)*(p.x-cx)+(cx-bx)*(p.y-cy))/denominator;
    const double wb = ((cy-ay)*(p.x-cx)+(ax-cx)*(p.y-cy))/denominator;
    const double wc = 1-wa-wb;
    if (wa < 0 || wb < 0 || wc < 0) return false;
    return face.mask->opaque(wa*a.uv.x+wb*b.uv.x+wc*c.uv.x, wa*a.uv.y+wb*b.uv.y+wc*c.uv.y);
}
} // namespace

bool ShellAlphaMask::valid() const noexcept {
    return width != 0 && height != 0 && static_cast<std::uint64_t>(width)*height == alpha.size();
}
bool ShellAlphaMask::opaque(double u, double v) const noexcept {
    if (!valid() || !std::isfinite(u) || !std::isfinite(v)) return false;
    u -= std::floor(u); v -= std::floor(v);
    const auto x = std::min(static_cast<std::uint32_t>(u*width), width-1U);
    const auto y = std::min(static_cast<std::uint32_t>(v*height), height-1U);
    return alpha[static_cast<std::size_t>(y)*width+x] != 0;
}

std::size_t HudViewModel::family_size(const std::string_view stem) const {
    for (std::size_t i = 0; i < 100; ++i) {
        const std::string name = std::string(stem)+(i < 10 ? "0" : "")+std::to_string(i);
        if (std::none_of(components.begin(), components.end(), [&](const auto& c) {
            return equal_name(data::ui::split_alt(c.name).base, name);
        })) return i;
    }
    return 100;
}
ShellPlacement HudViewModel::placement(const ReferenceSpace& space) const noexcept {
    // D4 measures right extent from the authored origin; preserve FoC's safe
    // area placement, including its one-unit negative faceplate border.
    return place_shell(space, visible_extent ? std::max(0.0F,visible_extent->right()) : 0.0);
}
bool HudViewModel::hit_test(const ReferencePoint point) const noexcept {
    if (!visible_extent || !contains(*visible_extent,point)) return false;
    for (const auto& component : components)
        if (component.visible && contains(component.rect,point)) return true;
    for (const auto& face : faceplates) if (face_hit(face,point)) return true;
    return false;
}
bool HudViewModel::hit_test_screen(const ReferencePoint point, const ReferenceSpace& space) const noexcept {
    if (point.x < 0 || point.y < 0 || point.x >= space.viewport.width || point.y >= space.viewport.height) return false;
    const auto placed = placement(space);
    if (placed.scale <= 0) return false;
    return hit_test({(point.x-placed.left)/placed.scale,(placed.bottom-point.y)/placed.scale});
}
HudViewModel hud_view_model(const data::ui::ShellAnchors& shell, const data::ui::CommandBarCatalog& catalog,
                           const std::uint32_t variant, const ShellMaskLookup& masks) {
    HudViewModel out;
    for (const auto* anchor : shell.for_variant(variant)) {
        if (catalog.find(anchor->name)) {
            // Shell placeholder visibility and XML Hidden are initialization
            // data, not runtime control state. Bound slots define HUD capacity
            // and extent; the presenter supplies their runtime visibility.
            out.components.push_back({anchor->name,anchor->rect,true});
            include(out.visible_extent,anchor->rect);
            continue;
        }
        const auto& name = anchor->alt.base;
        // Decorative help/hero art is not an input-blocking faceplate.
        bool faceplate = false;
        for (std::size_t i = 0; i+9U <= name.size(); ++i)
            faceplate = faceplate || equal_name(std::string_view(name).substr(i,9),"faceplate");
        if (!anchor->visible || !faceplate) continue;
        for (const auto& triangle : anchor->triangles) {
            auto mask = masks ? masks(triangle.base_texture) : nullptr;
            if (!mask || !mask->valid()) {
                if (std::none_of(out.diagnostics.begin(),out.diagnostics.end(),[&](const auto& d) {
                    return d.logical_path == triangle.base_texture;
                })) out.diagnostics.push_back(diagnostic("EAWR-UI-0314","faceplate alpha mask is unavailable",triangle.base_texture));
                continue;
            }
            out.faceplates.push_back({triangle,std::move(mask)});
            opaque_extent(out.visible_extent,out.faceplates.back());
        }
    }
    return out;
}
core::Result<data::ui::ShellAnchorLoad> animated_shell_anchors(
    const assets::Model& model, const animation::Player& player, const animation::SampleRequest& request) {
    if (player.bone_count() != model.bones.size())
        return core::Result<data::ui::ShellAnchorLoad>::failure(diagnostic("EAWR-UI-0315","shell player bone count differs"));
    auto pose = player.sample(request);
    if (!pose) return core::Result<data::ui::ShellAnchorLoad>::failure(pose.error());
    std::vector<data::ui::ShellTransform> transforms;
    for (const auto& bone : pose.value().bones) transforms.push_back(bone.model_asset);
    auto out = data::ui::shell_anchors(model,transforms);
    // Keep all anchors for family capacity; hide pose-invisible geometry.
    data::ui::ShellAnchors visible;
    visible.set_model_path(out.shell.model_path());
    for (auto anchor : out.shell.anchors()) {
        const auto mesh = std::find_if(model.meshes.begin(),model.meshes.end(),
            [&](const auto& m) { return m.name == anchor.name; });
        if (mesh != model.meshes.end() && mesh->bone >= 0)
            anchor.visible = anchor.visible && pose.value().bones[static_cast<std::size_t>(mesh->bone)].visible;
        visible.add(std::move(anchor));
    }
    out.shell = std::move(visible);
    return core::Result<data::ui::ShellAnchorLoad>::success(std::move(out));
}
} // namespace eawr::presentation::ui
