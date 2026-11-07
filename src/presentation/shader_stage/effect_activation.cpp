#include "eawr/presentation/shader_effect_source.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>

namespace eawr::presentation::shader_source {
namespace {
using Kind = assets::EffectValueKind;
using Category = assets::EffectValueCategory;
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    return first == std::string::npos ? std::string{} : value.substr(first, value.find_last_not_of(" \t\n\r") - first + 1);
}
struct Refusal { Diagnostic diagnostic; };
[[noreturn]] void fail(Failure reason, const Location& location) { throw Refusal{{reason, location, {}}}; }
std::string hash(std::span<const std::byte> bytes) {
    return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
std::string hex(const shader_runtime::Digest& bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) { const auto n = std::to_integer<unsigned>(byte); out += alphabet[n >> 4]; out += alphabet[n & 15U]; }
    return out;
}
// SFX-03: spelling -> existing measured P1 identifier, never device execution.
const std::map<std::string, std::uint32_t> state_ids{
    {"zenable",0}, {"zwriteenable",3}, {"alphatestenable",4}, {"srcblend",6}, {"destblend",7},
    {"cullmode",8}, {"zfunc",9}, {"alpharef",10}, {"alphafunc",11}, {"alphablendenable",13},
    {"stencilenable",22}, {"colorwriteenable",73}, {"blendop",75}, {"slopescaledepthbias",79},
    {"srgbwriteenable",97}, {"depthbias",98}, {"separatealphablendenable",99}, {"srcblendalpha",100},
    {"destblendalpha",101}, {"blendopalpha",102}, {"vertexshader",146}, {"pixelshader",147},
    {"texture",164}, {"addressu",165}, {"addressv",166}, {"addressw",167}, {"bordercolor",168},
    {"magfilter",169}, {"minfilter",170}, {"mipfilter",171}, {"mipmaplodbias",172},
    {"maxmiplevel",173}, {"maxanisotropy",174}, {"srgbtexture",175}
};
std::optional<std::uint32_t> enumeration(std::uint32_t id, const std::string& text) {
    std::vector<std::string_view> names;
    if (id == 8) names = {"none","cw","ccw"};
    else if (id == 9 || id == 11) names = {"never","less","equal","lessequal","greater","notequal","greaterequal","always"};
    else if (id == 6 || id == 7 || id == 100 || id == 101) names = {"zero","one","srccolor","invsrccolor","srcalpha","invsrcalpha","destalpha","invdestalpha","destcolor","invdestcolor","srcalphasat","bothsrcalpha","bothinvsrcalpha","blendfactor","invblendfactor","srccolor2","invsrccolor2"};
    else if (id == 75 || id == 102) names = {"add","subtract","revsubtract","min","max"};
    else if (id >= 165 && id <= 167) names = {"wrap","mirror","clamp","border","mirroronce"};
    else if (id >= 169 && id <= 171) {
        if (text == "none") return 0;
        names = {"point","linear","anisotropic"};
    }
    const auto found = std::find(names.begin(), names.end(), text);
    return found == names.end() ? std::nullopt : std::optional(static_cast<std::uint32_t>(found - names.begin()) + 1U);
}
} // namespace
struct Effect::Storage {
    Graph graph;
    assets::EffectView view;
    std::deque<std::string> strings;
    std::deque<std::vector<std::byte>> bytes;
    std::vector<shader_runtime::SourceStage> stages;
};
Effect::Effect(std::unique_ptr<Storage> storage) : storage_(std::move(storage)) {}
Effect::~Effect() = default;
Effect::Effect(Effect&&) noexcept = default;
Effect& Effect::operator=(Effect&&) noexcept = default;
const assets::EffectView& Effect::view() const { return storage_->view; }
struct Builder {
    std::unique_ptr<Effect::Storage> storage{std::make_unique<Effect::Storage>()};
    Limits limits;
    std::map<std::string, std::size_t> parameters;
    Location location;
    std::size_t retained{};
    explicit Builder(Limits bounded) : limits(bounded) {}
    assets::EffectView& view() { return storage->view; }
    void budget() {
        if (view().types.size() + view().values.size() + view().states.size() + view().objects.size() + view().initializers.size() > limits.nodes)
            fail(Failure::budget, location);
    }
    void retain(std::size_t amount) { if (amount > limits.bytes - retained) fail(Failure::budget, location); retained += amount; }
    std::string_view text(std::string label) { retain(label.size()); storage->strings.push_back(std::move(label)); return storage->strings.back(); }
    std::span<const std::byte> bytes(std::vector<std::byte> data) { retain(data.size()); storage->bytes.push_back(std::move(data)); return storage->bytes.back(); }
    std::size_t type(std::string name, std::string label = {}, std::string semantic = {}, std::string array = {}, bool row = false) {
        assets::EffectType item; name = lower(name); item.name = text(std::move(label)); item.semantic = text(std::move(semantic));
        const std::map<std::string, Kind> objects{{"string",Kind::text},{"texture",Kind::texture},{"texture1d",Kind::texture_1d},{"texture2d",Kind::texture_2d},{"texture3d",Kind::texture_3d},{"texturecube",Kind::texture_cube},{"sampler",Kind::sampler},{"sampler1d",Kind::sampler_1d},{"sampler2d",Kind::sampler_2d},{"sampler3d",Kind::sampler_3d},{"samplercube",Kind::sampler_cube},{"vertexshader",Kind::vertex_program},{"pixelshader",Kind::pixel_program}};
        if (objects.contains(name)) { item.kind = objects.at(name); item.value_class = Category::object; }
        else {
            const auto prefix = name.starts_with("float") ? "float" : name.starts_with("bool") ? "bool" : name.starts_with("int") ? "int" : "";
            if (std::string_view(prefix).empty()) fail(Failure::unsupported, location);
            item.kind = name.starts_with("float") ? Kind::floating : name.starts_with("bool") ? Kind::boolean : Kind::integer;
            const auto shape = name.substr(std::string_view(prefix).size()); item.rows = item.columns = 1;
            if (!shape.empty()) {
                if (shape[0] < '1' || shape[0] > '4') fail(Failure::unsupported, location);
                item.columns = static_cast<std::uint32_t>(shape[0] - '0'); item.value_class = Category::vector;
                if (shape.size() == 3 && shape[1] == 'x' && shape[2] >= '1' && shape[2] <= '4') {
                    item.rows = item.columns; item.columns = static_cast<std::uint32_t>(shape[2] - '0');
                    item.value_class = row ? Category::row_matrix : Category::column_matrix;
                } else if (shape.size() != 1) fail(Failure::unsupported, location);
            }
        }
        if (!array.empty()) {
            const auto result = std::from_chars(array.data(), array.data() + array.size(), item.array_count);
            if (result.ec != std::errc{} || result.ptr != array.data() + array.size() || item.array_count == 0) fail(Failure::unsupported, location);
        }
        view().types.push_back(std::move(item)); budget(); return view().types.size() - 1;
    }
    std::size_t value(std::size_t type_index, std::string expression, std::optional<std::uint32_t> literal = {}) {
        const auto shape = view().types[type_index];
        assets::EffectValue item; item.type = type_index; expression = trim(std::move(expression));
        if (shape.value_class == Category::object) {
            if (shape.array_count) fail(Failure::unsupported, location);
            if (shape.kind >= Kind::sampler && shape.kind <= Kind::sampler_cube) item.sampler_elements.resize(1);
            else {
                assets::EffectObject object; object.kind = shape.kind; object.has_payload = true;
                if (!expression.empty() && expression != "NULL" && expression != "null" && expression != "0") {
                    if (expression.front() != '"' || expression.back() != '"' || expression.find('\\') != std::string::npos) fail(Failure::unsupported, location);
                    std::vector<std::byte> data;
                    for (std::size_t i = 1; i + 1 < expression.size(); ++i) data.push_back(static_cast<std::byte>(expression[i]));
                    data.push_back(std::byte{}); object.bytes = bytes(std::move(data));
                }
                item.object_slots.push_back(static_cast<std::uint32_t>(view().objects.size())); view().objects.push_back(std::move(object));
            }
        } else {
            if (shape.value_class == Category::column_matrix && !expression.empty()) fail(Failure::unsupported, location);
            const auto count = std::uint64_t{shape.rows} * shape.columns * std::max(1U, shape.array_count);
            if (count > limits.bytes / 4U) fail(Failure::budget, location);
            std::vector<std::uint32_t> words;
            if (literal) words.push_back(*literal);
            else if (!expression.empty()) {
                const auto open = expression.find('(');
                if (open != std::string::npos) {
                    const auto constructor = lower(trim(expression.substr(0, open)));
                    const auto expected = shape.kind == Kind::floating ? "float" : shape.kind == Kind::boolean ? "bool" : "int";
                    const auto suffix = shape.value_class == Category::scalar ? "" : std::to_string(shape.columns);
                    if (constructor != expected + suffix || expression.back() != ')' || expression.find('(', open + 1) != std::string::npos)
                        fail(Failure::unsupported, location);
                    expression = expression.substr(open + 1, expression.size() - open - 2);
                }
                // Only literal lists. Constructors/arithmetic are retained in the
                // graph but refused here, rather than fabricated as EXPR bytecode.
                for (auto& c : expression) if (c == '{' || c == '}' || c == ',') c = ' ';
                std::string_view rest(expression);
                while (!rest.empty()) {
                    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\n' || rest.front() == '\r' || rest.front() == '\t')) rest.remove_prefix(1);
                    if (rest.empty()) break;
                    const auto end = rest.find_first_of(" \t\n\r"); const auto token = rest.substr(0, end);
                    std::uint32_t word{};
                    if (shape.kind == Kind::floating) {
                        float number{}; auto number_text = token;
                        if (number_text.ends_with('f') || number_text.ends_with('F')) number_text.remove_suffix(1);
                        const auto result = std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
                        if (result.ec != std::errc{} || result.ptr != number_text.data() + number_text.size() || !std::isfinite(number)) fail(Failure::unsupported, location);
                        word = std::bit_cast<std::uint32_t>(number);
                    } else if (lower(std::string(token)) == "true") word = 1;
                    else if (lower(std::string(token)) == "false") word = 0;
                    else {
                        std::int32_t number{}; const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
                        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) fail(Failure::unsupported, location);
                        word = std::bit_cast<std::uint32_t>(number);
                    }
                    if (shape.kind == Kind::boolean && word > 1U) fail(Failure::unsupported, location);
                    words.push_back(word); if (words.size() > count) fail(Failure::unsupported, location);
                    rest.remove_prefix(token.size());
                }
            }
            if (words.empty() && expression.empty()) words.resize(static_cast<std::size_t>(count));
            if (words.size() != count) fail(Failure::unsupported, location);
            std::vector<std::byte> data;
            for (const auto word : words) for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<std::byte>(word >> (i * 8U)));
            item.bytes = bytes(std::move(data));
        }
        view().values.push_back(std::move(item)); budget(); return view().values.size() - 1;
    }
    std::vector<std::size_t> annotations(const std::vector<Assignment>& source) {
        std::vector<std::size_t> result;
        for (const auto& item : source) { location = item.location; result.push_back(value(type(item.type, item.name), item.expression)); }
        return result;
    }
    std::size_t state(const Assignment& item, bool sampler) {
        location = item.location; const auto name = lower(item.name);
        if (!state_ids.contains(name)) fail(Failure::unsupported, location);
        assets::EffectState out; out.identifier = state_ids.at(name); out.selector = sampler ? 256U : 0U;
        if ((sampler && out.identifier < 164U) || (!sampler && out.identifier >= 164U)) fail(Failure::unsupported, location);
        const auto expression = trim(item.expression);
        if (out.identifier == 146U || out.identifier == 147U) {
            const auto stage = out.identifier == 146U ? assets::EffectProgramStage::vertex : assets::EffectProgramStage::pixel;
            assets::EffectShaderAssignment shader; shader.stage = stage;
            bool null = lower(expression) == "null" || expression == "0";
            if (parameters.contains(expression)) {
                shader.parameter = parameters.at(expression);
                null = !storage->graph.declarations[*shader.parameter].program;
            }
            if (!null) {
                shader.descriptor = item.program;
                if (!shader.descriptor || *shader.descriptor >= view().programs.size() || view().programs[*shader.descriptor].stage != stage) fail(Failure::descriptor, location);
                shader.kind = assets::EffectShaderAssignmentKind::program;
            }
            out.value = value(type(stage == assets::EffectProgramStage::vertex ? "vertexshader" : "pixelshader"), {}); out.shader = shader;
        } else {
            auto reference = expression;
            if (reference.size() >= 2 && reference.front() == '<' && reference.back() == '>') reference = trim(reference.substr(1, reference.size() - 2));
            if (parameters.contains(reference)) {
                const auto parameter = parameters.at(reference); const auto target = view().parameters[parameter].value;
                out.value = view().values.size(); auto copy = view().values[target];
                copy.initializer = view().initializers.size(); view().values.push_back(std::move(copy));
                assets::EffectInitializer initializer; initializer.kind = assets::EffectInitializerKind::parameter_reference;
                initializer.value = out.value; initializer.parameter = parameter; view().initializers.push_back(initializer);
            } else if (out.identifier == 164U) out.value = value(type("texture"), expression);
            else if (const auto number = enumeration(out.identifier, lower(expression))) out.value = value(type("int"), {}, number);
            else out.value = value(type(out.identifier == 98U || out.identifier == 79U || out.identifier == 172U ? "float" : "int"), expression);
        }
        view().states.push_back(std::move(out)); budget(); return view().states.size() - 1;
    }
    Produced run(Graph graph, const Compile& compiler) {
        storage->graph = std::move(graph); view().source = storage->graph.source; view().objects.emplace_back();
        for (const auto& declaration : storage->graph.declarations) {
            location = declaration.location;
            for (const auto& qualifier : declaration.qualifiers) if (qualifier == "shared" || qualifier == "volatile" || (qualifier == "static" && !declaration.constant)) fail(Failure::unsupported, location);
            if (parameters.contains(declaration.name)) fail(Failure::unsupported, location);
            const auto name = lower(declaration.type);
            const bool shader = name == "vertexshader" || name == "pixelshader";
            const auto index = value(type(name, declaration.name, declaration.semantic, declaration.array, declaration.row_major), shader ? "" : declaration.expression);
            parameters.emplace(declaration.name, view().parameters.size());
            assets::EffectParameter parameter; parameter.value = index; parameter.annotations = annotations(declaration.annotations);
            view().parameters.push_back(std::move(parameter));
        }
        for (std::size_t i = 0; i < storage->graph.declarations.size(); ++i) {
            const auto& declaration = storage->graph.declarations[i];
            for (const auto& assignment : declaration.sampler_states) {
                const auto index = state(assignment, true);
                view().values[view().parameters[i].value].sampler_elements[0].states.push_back(index);
            }
        }
        for (const auto& stage : storage->graph.stages) {
            location = stage.location;
            if (!compiler) fail(Failure::stage_compile, location);
            auto result = compiler(storage->graph.stage_text, stage.entry, stage.profile);
            if (!result.stage) throw Refusal{{Failure::stage_compile, location, result.diagnostic}};
            if (result.stage->entry != stage.entry || result.stage->profile != stage.profile || result.stage->bytecode.size() > limits.bytes - retained)
                fail(Failure::descriptor, location);
            retain(result.stage->bytecode.size());
            storage->stages.push_back(std::move(*result.stage));
        }
        for (const auto& stage : storage->stages) {
            auto program = shader_runtime::view_source_program(stage);
            if (!program) fail(Failure::descriptor, location);
            if (stage.profile.size() != 6 || program.value().major != static_cast<unsigned>(stage.profile[3] - '0') || program.value().minor != static_cast<unsigned>(stage.profile[5] - '0') || program.value().stage != (stage.profile.starts_with("vs_") ? assets::EffectProgramStage::vertex : assets::EffectProgramStage::pixel)) fail(Failure::descriptor, location);
            for (auto& constant : program.value().constants) if (parameters.contains(std::string(constant.name))) constant.parameter = parameters.at(std::string(constant.name));
            view().programs.push_back(std::move(program).value());
        }
        for (std::size_t i = 0; i < storage->graph.declarations.size(); ++i) {
            const auto& declaration = storage->graph.declarations[i];
            if (declaration.program) {
                const auto slot = view().values[view().parameters[i].value].object_slots[0];
                view().objects[slot].program = declaration.program;
            }
        }
        for (const auto& technique : storage->graph.techniques) {
            assets::EffectTechnique target; target.name = text(technique.name); target.annotations = annotations(technique.annotations);
            for (const auto& pass : technique.passes) {
                assets::EffectPass output; output.name = text(pass.name); output.annotations = annotations(pass.annotations);
                for (const auto& assignment : pass.states) output.states.push_back(state(assignment, false));
                target.passes.push_back(std::move(output));
            }
            view().techniques.push_back(std::move(target));
        }
        return {Effect(std::move(storage)), {}};
    }
};
Produced produce(Graph graph, const Compile& compiler, Limits limits) {
    try { return Builder(limits).run(std::move(graph), compiler); }
    catch (const Refusal& error) { return {{}, error.diagnostic}; }
    catch (const std::bad_alloc&) { return {{}, Diagnostic{Failure::budget, {}, {}}}; }
}
Produced compile(shader_runtime::CompilerSession& session, const shader_runtime::PreprocessedSource& source,
    Limits limits, shader_runtime::SourceLimits compiler_limits) {
    if (source.dependencies.empty()) return {{}, Diagnostic{Failure::selection, {}, {}}};
    std::vector<assets::Source> inputs;
    for (const auto& dependency : source.dependencies) inputs.push_back(dependency.source);
    auto parsed = parse(source.expanded, inputs.front(), inputs, limits);
    if (!parsed.graph) return {{}, parsed.diagnostic};
    return produce(std::move(*parsed.graph), [&](auto stage_text, auto entry, auto profile) {
        return shader_runtime::compile_source_stage(session, source, stage_text, entry, profile, compiler_limits);
    }, limits);
}
core::Result<Selection> select(const vfs::Vfs& mounted, std::string_view logical_stem, const Allowed& allowed, const Allowed& mod_source) {
    for (const auto form : {Form::compiled, Form::source}) {
        auto record = mounted.stat(std::string(logical_stem) + (form == Form::compiled ? ".fxo" : ".fx"));
        if (record) {
            if (form == Form::source && (!mod_source || !mod_source(record.value()))) return core::Result<Selection>::success({});
            Selection out; out.form = form; out.allowed = allowed && allowed(record.value()); out.record = std::move(record).value();
            return core::Result<Selection>::success(std::move(out));
        }
        if (record.error().code != vfs::diagnostic_codes::not_found) return core::Result<Selection>::failure(record.error());
    }
    return core::Result<Selection>::success({});
}
bool source_identity(shader_cache::Identity& identity, const Selection& selection,
    const shader_runtime::PreprocessedSource& source, const Graph& graph) {
    if (!selection.allowed || selection.form != Form::source || !selection.record || source.dependencies.empty()) return false;
    const auto& root = source.dependencies.front(); const auto& selected = *selection.record;
    if (root.source.logical_path != selected.canonical_path || root.source.source_id != selected.source_id || root.source.layer_id != selected.layer_id || root.source.origin != selected.origin) return false;
    identity.form = shader_cache::Form::source; identity.logical = root.source.logical_path; identity.root_sha256 = hex(root.sha256);
    identity.root_source = root.source.source_id; identity.root_layer = root.source.layer_id;
    identity.root_origin = std::string(vfs::to_string(root.source.origin));
    identity.parser_schema = schema_version; identity.selection_policy = "fxo-first/1"; identity.resolver_policy = "mounted-source/1";
    identity.includes.clear(); identity.macros.clear(); identity.stages.clear();
    // SOURCE-A retains an ordered open ledger, not include edges. P7's parent
    // field here is the activation root; expanded-input binds resolved context.
    for (const auto& dependency : source.dependencies) identity.includes.push_back({root.source.logical_path, dependency.source.logical_path, hex(dependency.sha256), dependency.source.source_id, dependency.source.layer_id, std::string(vfs::to_string(dependency.source.origin))});
    for (const auto& macro : source.macros) identity.macros.push_back({macro.name, macro.value});
    for (const auto& stage : graph.stages) identity.stages.push_back({stage.profile.starts_with("vs_") ? "vertex" : "pixel", stage.entry, stage.profile, {}, hash(graph.stage_text)});
    identity.package_sha256 = hex(source.compiler.package_lock_sha256);
    identity.compiler_abi = hex(source.compiler.public_c_abi.manifest_sha256);
    identity.capability_manifest = hex(source.compiler.capability_manifest_sha256);
    std::erase_if(identity.compiler_options, [](const auto& token) { return token.name == "source-contract" || token.name == "preprocess-manifest" || token.name == "expanded-input"; });
    identity.compiler_options.push_back({"source-contract", std::string(shader_runtime::source_compiler_contract())});
    identity.compiler_options.push_back({"preprocess-manifest", hex(source.manifest_sha256)});
    identity.compiler_options.push_back({"expanded-input", hex(source.expanded_sha256)});
    return true;
}
Fallback fallback(const Selection& selected, bool hand_adapter) noexcept {
    if (selected.form == Form::absent) return Fallback::missing_resource;
    return hand_adapter ? Fallback::hand_adapter_approximation : Fallback::omit;
}
} // namespace eawr::presentation::shader_source
