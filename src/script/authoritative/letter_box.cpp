#include "eawr/script/authoritative/letter_box.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace eawr::script::authoritative {
namespace {

core::Result<ValueList> problem(std::string message) {
    core::Diagnostic out;
    out.code = std::string(codes::invalid_request);
    out.message = std::move(message);
    return core::Result<ValueList>::failure(std::move(out));
}

} // namespace

Binding letter_box_binding(std::string name, std::string direction, std::string verb) {
    return [name = std::move(name), direction = std::move(direction), verb = std::move(verb)](
               BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* seconds = arguments.size() == 1 ? std::get_if<numeric::LuaNumber>(&arguments[0].data) : nullptr;
        if (seconds == nullptr) return problem(name + " expects one number of seconds");
        const auto fixed = numeric::to_fixed(*seconds); // fails for a NaN, an infinity or a value out of range
        if (!fixed || fixed.value().raw() < 0) return problem(name + ": the seconds must be a finite number, not negative");
        // Locale-free and integer only (the simulation boundary has no floating point): the fixed-point value rounded to six
        // decimals, trailing zeros dropped ("2", "0.5", "1.25").
        constexpr std::int64_t micro = 1000000;
        const std::int64_t raw = fixed.value().raw();
        std::int64_t whole = raw >> sim::math::Fixed::fractional_bits;
        std::int64_t micros = (((raw & (sim::math::Fixed::scale - 1)) * micro) + (sim::math::Fixed::scale / 2)) >> sim::math::Fixed::fractional_bits;
        if (micros >= micro) { ++whole; micros -= micro; }
        std::string text = std::to_string(whole);
        if (micros != 0) {
            std::string digits = std::to_string(micros);
            digits.insert(0, 6 - digits.size(), '0');
            while (digits.back() == '0') digits.pop_back();
            text += "." + digits;
        }
        context.issue_command(verb, {Value::text(direction), Value::text(std::move(text))});
        return core::Result<ValueList>::success({});
    };
}

} // namespace eawr::script::authoritative
