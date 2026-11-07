#include "eawr/presentation/shader_techniques.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace eawr::presentation::shader_techniques {
namespace {
namespace codes = diagnostic_codes;
using assets::EffectValueKind;
struct Failure final { std::string_view code; const char* message; };
[[noreturn]] void fail(std::string_view code, const char* message) { throw Failure{code, message}; }

class Reader final {
public:
    Reader(const assets::EffectView& effect, Limits limits) : effect_(effect), limits_(limits) {}
    std::size_t work{};
    // Compiled payload/state offsets follow the container header; a descriptor
    // offset of zero is treated as unavailable here.
    std::optional<std::size_t> technique, pass, offset;
    std::string_view annotation;
    void step(std::size_t count = 1U) {
        if (count > limits_.work - work) fail(codes::limit, "technique work budget exceeded");
        work += count;
    }
    void text(std::string_view contents) {
        if (contents.size() > limits_.string_bytes) fail(codes::limit, "annotation name or text budget exceeded");
        step(contents.size());
    }
    const assets::EffectValue& value(std::size_t index) {
        step();
        if (index >= effect_.values.size()) fail(codes::descriptor, "annotation value index is out of range");
        return effect_.values[index];
    }
    const assets::EffectType& type(const assets::EffectValue& item) {
        step();
        if (item.type >= effect_.types.size()) fail(codes::descriptor, "annotation type index is out of range");
        return effect_.types[item.type];
    }
    std::optional<std::size_t> find(std::span<const std::size_t> annotations, std::string_view name) {
        offset.reset(); annotation = name;
        if (annotations.size() > limits_.annotations) fail(codes::limit, "annotation count budget exceeded");
        std::optional<std::size_t> found;
        for (const auto index : annotations) {
            const auto& shape = type(value(index));
            text(shape.name);
            if (shape.name != name) continue;
            if (found) fail(codes::unsupported, "duplicate recognized annotation is unsupported");
            found = index;
        }
        return found;
    }
    void single(const assets::EffectValue& item, const assets::EffectType& shape) {
        if (item.initializer) fail(codes::unsupported, "dynamic annotation is unsupported");
        if (shape.array_count != 0U || !shape.members.empty() || !item.members.empty()
            || !item.sampler_elements.empty()) fail(codes::unsupported, "annotation must be a scalar value");
    }
    std::uint32_t numeric(std::size_t index, EffectValueKind kind) {
        const auto& item = value(index); const auto& shape = type(item);
        single(item, shape);
        if (shape.kind != kind || shape.value_class > assets::EffectValueCategory::row_matrix
            || shape.rows != 1U || shape.columns != 1U || !item.object_slots.empty())
            fail(codes::unsupported, "recognized annotation has an unsupported numeric type");
        if (item.bytes.size() != 4U) fail(codes::descriptor, "annotation numeric storage must be four bytes");
        step(4U);
        std::uint32_t result{};
        for (unsigned i = 0U; i < 4U; ++i)
            result |= std::to_integer<std::uint32_t>(item.bytes[i]) << (8U * i);
        return result;
    }
    bool boolean(std::span<const std::size_t> annotations, std::string_view name) {
        const auto index = find(annotations, name);
        if (!index) return false;
        const auto bits = numeric(*index, EffectValueKind::boolean);
        if (bits > 1U) fail(codes::unsupported, "annotation boolean must be zero or one");
        return bits != 0U;
    }
    std::string_view string(std::size_t index) {
        const auto& item = value(index); const auto& shape = type(item);
        single(item, shape);
        if (shape.kind != EffectValueKind::text || shape.value_class != assets::EffectValueCategory::object
            || item.object_slots.size() != 1U)
            fail(codes::unsupported, "recognized annotation has an unsupported text type");
        // Both compiled and source-produced descriptors supply this typed
        // identity. Container slot-word decoding belongs to the P1 reader.
        const auto slot = item.object_slots.front();
        if (slot >= effect_.objects.size()) fail(codes::descriptor, "annotation text slot is out of range");
        const auto& object = effect_.objects[slot];
        if (object.byte_offset != 0U) offset = object.byte_offset;
        if (slot == 0U) {
            if (object.has_payload || object.initializer || object.expression || object.program || !object.bytes.empty()
                || (object.resolved_object && *object.resolved_object != 0U))
                fail(codes::descriptor, "annotation null text slot is malformed");
            return {};
        }
        if (object.initializer || object.expression || object.program
            || (object.resolved_object && *object.resolved_object != slot))
            fail(codes::unsupported, "annotation text reference is unsupported");
        if (!object.has_payload || object.kind != EffectValueKind::text)
            fail(codes::descriptor, "annotation text payload is unavailable");
        if (object.bytes.size() > limits_.string_bytes) fail(codes::limit, "annotation text budget exceeded");
        step(object.bytes.size());
        if (object.bytes.empty() || object.bytes.back() != std::byte{}
            || std::find(object.bytes.begin(), object.bytes.end() - 1, std::byte{}) != object.bytes.end() - 1)
            fail(codes::descriptor, "annotation text requires exactly one terminal zero");
        return {reinterpret_cast<const char*>(object.bytes.data()), object.bytes.size() - 1U};
    }
    core::Diagnostic with_context(core::Diagnostic result) const {
        std::string prefix;
        if (technique) prefix += "technique " + std::to_string(*technique);
        if (pass) prefix += " pass " + std::to_string(*pass);
        if (!annotation.empty()) prefix += " lookup " + std::string(annotation);
        if (!prefix.empty()) result.message = prefix + ": " + result.message;
        if (result.message.size() > 256U) result.message.resize(256U);
        return result;
    }
    core::Diagnostic diagnostic(const Failure& failure) const {
        core::Diagnostic result; result.code = failure.code; result.message = failure.message;
        if (!effect_.source.logical_path.empty()) result.logical_path = effect_.source.logical_path.substr(0U, 512U);
        if (!effect_.source.source_id.empty()) result.source_id = effect_.source.source_id.substr(0U, 512U);
        if (offset && *offset < std::numeric_limits<std::uint64_t>::max())
            result.column = static_cast<std::uint64_t>(*offset) + 1U;
        return with_context(std::move(result));
    }
private:
    const assets::EffectView& effect_;
    Limits limits_;
};

std::uint32_t lod_rank(std::string_view name) {
    if (core::iequals(name, "FIXEDFUNCTION")) return 0U;
    if (core::iequals(name, "DX8")) return 1U;
    if (core::iequals(name, "DX8ATI")) return 2U;
    return 3U; // DX9, absent and unknown are the highest rank.
}
constexpr std::array<std::string_view, 10U> vendor_names{
    "VENDOR_FILTER_0", "VENDOR_FILTER_1", "VENDOR_FILTER_2", "VENDOR_FILTER_3", "VENDOR_FILTER_4",
    "VENDOR_FILTER_5", "VENDOR_FILTER_6", "VENDOR_FILTER_7", "VENDOR_FILTER_8", "VENDOR_FILTER_9"};
} // namespace

core::Result<Selection> select_technique(const assets::EffectView& effect, Settings settings,
    std::span<const CandidateValidation> validation, Limits limits) {
    Reader reader(effect, limits);
    try {
        if (effect.techniques.size() > limits.techniques) fail(codes::limit, "technique count budget exceeded");
        if (validation.size() != effect.techniques.size()) fail(codes::descriptor, "technique validation table size mismatch");
        for (std::size_t i = 0U; i < validation.size(); ++i) {
            reader.technique = i; reader.step(2U);
            if (validation[i].regular < Validation::compatible || validation[i].regular > Validation::device)
                fail(codes::descriptor, "unknown regular technique validation outcome");
            if (validation[i].software < Validation::compatible || validation[i].software > Validation::device)
                fail(codes::descriptor, "unknown software technique validation outcome");
        }
        Selection result; result.software_processing = settings.software_processing;
        for (std::size_t i = 0U; i < effect.techniques.size(); ++i) {
            reader.technique = i; reader.offset.reset(); reader.annotation = {}; reader.step();
            const auto& technique = effect.techniques[i]; reader.text(technique.name);
            const auto before = result.software_processing;
            const auto reject = [&](Rejection reason, std::optional<Validation> cause = {}) {
                result.rejected.push_back({i, reason, cause, before, result.software_processing});
            };
            if (technique.name == "max_viewport") { reject(Rejection::editor); continue; }
            const auto lod = reader.find(technique.annotations, "LOD");
            if ((lod ? lod_rank(reader.string(*lod)) : 3U) > settings.shader_detail) {
                reject(Rejection::shader_detail); continue;
            }
            const auto water = reader.find(technique.annotations, "WaterLOD");
            if (water && static_cast<std::int64_t>(std::bit_cast<std::int32_t>(reader.numeric(*water, EffectValueKind::integer)))
                > static_cast<std::int64_t>(settings.water_detail)) { reject(Rejection::water_detail); continue; }
            bool excluded{};
            for (const auto name : vendor_names) {
                const auto vendor = reader.find(technique.annotations, name);
                if (!vendor) break;
                if (reader.numeric(*vendor, EffectValueKind::integer) == settings.vendor) { excluded = true; break; }
            }
            if (excluded) { reject(Rejection::vendor); continue; }
            const bool swvp = reader.boolean(technique.annotations, "SWVP");
            if (swvp) result.software_processing = true;
            const auto outcome = result.software_processing ? validation[i].software : validation[i].regular;
            if (outcome != Validation::compatible) { reject(Rejection::validation, outcome); continue; }
            result.validated_software = result.software_processing;
            if (swvp) result.software_processing = false;
            result.cpu_skin = reader.boolean(technique.annotations, "CPUSKIN");
            result.technique = i; break;
        }
        result.work = reader.work;
        return core::Result<Selection>::success(std::move(result));
    } catch (const Failure& failure) { return core::Result<Selection>::failure(reader.diagnostic(failure)); }
}

core::Result<PassFlow> make_pass_flow(const assets::EffectView& effect, std::size_t technique_index,
    InitialState initial, Limits limits) {
    Reader reader(effect, limits);
    reader.technique = technique_index;
    try {
        if (technique_index >= effect.techniques.size()) fail(codes::descriptor, "selected technique index is out of range");
        const auto& technique = effect.techniques[technique_index];
        if (technique.passes.size() > limits.passes) fail(codes::limit, "pass count budget exceeded");
        if (technique.passes.empty()) fail(codes::unsupported, "selected technique has no draw pass");
        std::vector<bool> cleanup;
        for (std::size_t i = 0U; i < technique.passes.size(); ++i) {
            reader.pass = i; reader.step();
            const bool marked = reader.boolean(technique.passes[i].annotations, "AlamoCleanup");
            if (marked && i != technique.passes.size() - 1U) fail(codes::unsupported, "cleanup must be the trailing pass");
            cleanup.push_back(marked);
        }
        const auto draws = technique.passes.size() - (cleanup.back() ? 1U : 0U);
        // Only a trailing marker is allowed, so draw indices are 0..draws-1;
        // pass 1 is the first extra draw when more than one is proposed.
        reader.annotation = {}; reader.offset.reset(); reader.pass = draws > 1U ? 1U : 0U;
        if (draws != 1U) fail(codes::unsupported, "exactly one real draw pass is supported");
        reader.pass.reset();
        PassFlow result; result.technique = technique_index;
        result.requires_cleanup_qualification = cleanup.back();
        std::vector<EffectiveState> current;
        for (const auto& value : shader_states::render_defaults(initial.depth_attachment)) {
            reader.step(); current.push_back({value.field, value.literal, {}});
        }
        std::optional<assets::EffectShaderAssignment> vertex, pixel;
        std::size_t state_count{};
        for (std::size_t i = 0U; i < technique.passes.size(); ++i) {
            reader.pass = i; reader.offset.reset(); reader.annotation = {};
            const auto& pass = technique.passes[i];
            if (pass.states.size() > limits.states - state_count) fail(codes::limit, "pass state count budget exceeded");
            state_count += pass.states.size();
            auto normalized = shader_states::normalize_pass(effect, pass, {pass.states.size(), limits.work - reader.work});
            if (!normalized) {
                auto error = std::move(normalized.error());
                // P4b's zero/default offset is unavailable in a source graph.
                if (error.column == 1U) error.column.reset();
                return core::Result<PassFlow>::failure(reader.with_context(std::move(error)));
            }
            reader.step(normalized.value().work);
            result.requires_evaluation = result.requires_evaluation || normalized.value().requires_evaluation;
            std::size_t numeric{};
            bool assigned_vertex{}, assigned_pixel{};
            for (const auto state_index : normalized.value().state_order) {
                reader.step(); const auto& state = effect.states[state_index];
                reader.offset = state.byte_offset != 0U ? std::optional<std::size_t>{state.byte_offset} : std::nullopt;
                if (state.shader) {
                    auto& assigned = state.shader->stage == assets::EffectProgramStage::vertex ? assigned_vertex : assigned_pixel;
                    if (assigned) fail(codes::unsupported, "duplicate shader-stage assignment is unsupported");
                    assigned = true;
                    auto& target = state.shader->stage == assets::EffectProgramStage::vertex ? vertex : pixel;
                    target = state.shader;
                    result.requires_parameter_resolution = result.requires_parameter_resolution || state.shader->parameter.has_value();
                } else {
                    if (numeric >= normalized.value().assignments.size())
                        fail(codes::descriptor, "normalized assignment order is inconsistent");
                    const auto& assignment = normalized.value().assignments[numeric++];
                    reader.step(current.size());
                    const auto found = std::find_if(current.begin(), current.end(), [&](const EffectiveState& value) {
                        return value.field == assignment.field;
                    });
                    if (found == current.end()) fail(codes::descriptor, "normalized field is not a render state");
                    found->initial.reset(); found->assigned = assignment;
                    result.requires_parameter_resolution = result.requires_parameter_resolution || assignment.parameter.has_value();
                }
            }
            reader.offset.reset();
            if (!cleanup[i]) {
                if (!vertex || !pixel) fail(codes::unsupported, "draw requires explicit vertex and pixel shader assignments");
                if (vertex->kind == assets::EffectShaderAssignmentKind::null_shader
                    || pixel->kind == assets::EffectShaderAssignmentKind::null_shader)
                    fail(codes::unsupported, "fixed-function draw is unsupported");
            }
            reader.step(current.size());
            result.passes.push_back({i, cleanup[i], std::move(normalized).value(), current, vertex, pixel});
        }
        result.work = reader.work;
        return core::Result<PassFlow>::success(std::move(result));
    } catch (const Failure& failure) { return core::Result<PassFlow>::failure(reader.diagnostic(failure)); }
}
} // namespace eawr::presentation::shader_techniques
