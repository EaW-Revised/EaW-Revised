#include "eawr/script/authoritative/story_presentation.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"

#include <system_error>
#include <utility>

namespace eawr::script::authoritative {
namespace {

using numeric::LuaNumber;

core::Result<ValueList> problem(std::string message) {
    core::Diagnostic out;
    out.code = std::string(codes::invalid_request);
    out.message = std::move(message);
    return core::Result<ValueList>::failure(std::move(out));
}

core::Result<ValueList> done() { return core::Result<ValueList>::success({}); }

// Locale-free and integer only (the script boundary has no floating point): the fixed-point value rounded to six decimals,
// trailing zeros dropped ("2", "-0.5", "1.25"). The same writing the letterbox calls use.
std::string fixed_text(const sim::math::Fixed value) {
    constexpr std::int64_t micro = 1000000;
    const std::int64_t raw = value.raw();
    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(raw) + 1U) : static_cast<std::uint64_t>(raw);
    std::uint64_t whole = magnitude >> sim::math::Fixed::fractional_bits;
    std::uint64_t micros = (((magnitude & (sim::math::Fixed::scale - 1)) * micro) + (sim::math::Fixed::scale / 2)) >> sim::math::Fixed::fractional_bits;
    if (micros >= static_cast<std::uint64_t>(micro)) { ++whole; micros -= static_cast<std::uint64_t>(micro); }
    std::string text = negative && (whole != 0 || micros != 0) ? "-" : "";
    text += std::to_string(whole);
    if (micros != 0) {
        std::string digits = std::to_string(micros);
        digits.insert(0, 6 - digits.size(), '0');
        while (digits.back() == '0') digits.pop_back();
        text += "." + digits;
    }
    return text;
}

// A script number the call reads as a float; nullopt for anything else (a non-number, or one the boundary refuses).
std::optional<sim::math::Fixed> read_number(const Value& value) {
    const auto* number = std::get_if<LuaNumber>(&value.data);
    if (number == nullptr) return std::nullopt;
    const auto fixed = numeric::to_fixed(*number);
    if (!fixed) return std::nullopt;
    return fixed.value();
}

// The original reads a Lua boolean for a flag and a Lua number for the other flags (greater than zero is true).
bool positive(const sim::math::Fixed value) { return value.raw() > 0; }

} // namespace

Binding story_camera_key_binding(std::string name, const bool target, const bool transition, std::shared_ptr<const StoryPositionReader> host) {
    return [name = std::move(name), target, transition, host = std::move(host)](
               BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const std::size_t expected = transition ? 9U : 8U;
        if (arguments.size() < expected) return problem(name + " -- Expected " + std::to_string(expected) + " parameters");
        const auto position = host ? host->read(arguments[0]) : std::nullopt;
        if (!position) {
            context.report(codes::script_warning, name + " -- Invalid parameter 1, expected a position.");
            return done();
        }
        // The numbers: [time], distance, pitch, yaw, euler, [object], use-object-rotation, centre. Each is a float.
        std::size_t at = 1;
        std::optional<sim::math::Fixed> seconds;
        if (transition) {
            seconds = read_number(arguments[at]);
            if (!seconds) return problem(name + " -- Invalid parameter 2, expected a float");
            ++at;
        }
        std::array<sim::math::Fixed, 4> offset{}; // distance, pitch, yaw, euler flag
        for (std::size_t i = 0; i < offset.size(); ++i, ++at) {
            const auto value = read_number(arguments[at]);
            if (!value) return problem(name + " -- Invalid parameter " + std::to_string(at + 1) + ", expected a float");
            offset[i] = *value;
        }
        std::string object = "-"; // an object argument that is not a game object is ignored, as in the original
        if (const auto* handle = std::get_if<Handle>(&arguments[at].data); handle != nullptr && host && handle->kind == host->object_kind())
            object = std::to_string(handle->id);
        ++at;
        std::array<sim::math::Fixed, 2> flags{}; // use-object-rotation, centre
        for (std::size_t i = 0; i < flags.size(); ++i, ++at) {
            const auto value = read_number(arguments[at]);
            if (!value) return problem(name + " -- Invalid parameter " + std::to_string(at + 1) + ", expected a float");
            flags[i] = *value;
        }
        context.issue_command(std::string(verb_story_camera), {
            Value::text(target ? "target" : "camera"), Value::text(transition ? "transition" : "set"),
            Value::text(fixed_text((*position)[0])), Value::text(fixed_text((*position)[1])), Value::text(fixed_text((*position)[2])),
            Value::text(seconds ? fixed_text(*seconds) : "0"),
            Value::text(fixed_text(offset[0])), Value::text(fixed_text(offset[1])), Value::text(fixed_text(offset[2])),
            Value::text(positive(offset[3]) ? "1" : "0"), Value::text(std::move(object)),
            Value::text(positive(flags[0]) ? "1" : "0"), Value::text(positive(flags[1]) ? "1" : "0")});
        return done();
    };
}

Binding story_camera_start_binding(std::string name) {
    return [name = std::move(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        bool music = true; // the default of the original
        if (!arguments.empty()) {
            const auto* flag = std::get_if<bool>(&arguments[0].data);
            if (flag == nullptr) return problem(name + " -- invalid type for parameter 1.  Expected bool.");
            music = *flag;
        }
        context.issue_command(std::string(verb_story_camera), {Value::text("start"), Value::text(music ? "1" : "0")});
        return done();
    };
}

Binding story_camera_end_binding(std::string) {
    return [](BindingContext& context, const ValueList&) -> core::Result<ValueList> {
        context.issue_command(std::string(verb_story_camera), {Value::text("end")});
        return done();
    };
}

Binding story_to_tactical_binding(std::string name) {
    return [name = std::move(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty()) return problem(name + " -- Expected float parameter 1");
        const auto seconds = read_number(arguments[0]);
        if (!seconds) return problem(name + " -- Invalid parameter 1, expected a float");
        context.issue_command(std::string(verb_story_camera), {Value::text("tactical"), Value::text(fixed_text(*seconds))});
        return done();
    };
}

Binding story_fade_binding(std::string name, const StoryRequestKind kind) {
    const bool timed = kind == StoryRequestKind::fade_screen_in || kind == StoryRequestKind::fade_screen_out;
    const char* op = kind == StoryRequestKind::fade_screen_in ? "in" : kind == StoryRequestKind::fade_screen_out ? "out"
        : kind == StoryRequestKind::fade_on ? "on" : "off";
    return [name = std::move(name), timed, op](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        std::string seconds = "0";
        if (timed) {
            if (arguments.empty()) return problem(name + " -- Expected float parameter 1");
            const auto value = read_number(arguments[0]);
            if (!value) return problem(name + " -- Invalid parameter 1, expected a float");
            seconds = fixed_text(*value);
        }
        context.issue_command(std::string(verb_story_fade), {Value::text(op), Value::text(std::move(seconds))});
        return done();
    };
}

Binding story_environment_binding(std::string name, const StoryRequestKind kind) {
    return [name = std::move(name), kind](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (kind == StoryRequestKind::environment_cinematic) {
            if (arguments.empty()) return problem(name + " -- Expected bool parameter 1");
            const auto* flag = std::get_if<bool>(&arguments[0].data);
            if (flag == nullptr) return problem(name + " -- invalid type for parameter 1.  Expected bool.");
            context.issue_command(std::string(verb_story_environment), {Value::text("cinematic"), Value::text(*flag ? "1" : "0")});
            return done();
        }
        if (arguments.empty()) return problem(name + " -- Expected int parameter 1");
        const auto value = read_number(arguments[0]);
        if (!value) return problem(name + " -- invalid type for parameter 1.  Expected int.");
        // The original truncates the number to an integer; the toward-zero cut of the fixed-point raw value is the same.
        const std::int64_t whole = value->raw() / (std::int64_t{1} << sim::math::Fixed::fractional_bits);
        context.issue_command(std::string(verb_story_environment), {Value::text("index"), Value::text(std::to_string(whole))});
        return done();
    };
}

bool is_story_verb(const std::string_view verb) noexcept {
    return verb == verb_story_camera || verb == verb_story_fade || verb == verb_story_environment;
}

} // namespace eawr::script::authoritative
