#include "eawr/script/script_host.hpp"

#include <utility>

namespace eawr::script {
namespace {

[[nodiscard]] core::Diagnostic unsupported_conversion() {
    core::Diagnostic diagnostic;
    diagnostic.code = diagnostic_codes::unsupported_feature;
    diagnostic.message = "non-ASCII narrow/wide conversion is unsupported";
    return diagnostic;
}

[[nodiscard]] core::Result<std::u16string> ascii_units(const std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (const unsigned char byte : text) {
        if (byte > 0x7fU) {
            return core::Result<std::u16string>::failure(unsupported_conversion());
        }
        result.push_back(static_cast<char16_t>(byte));
    }
    return core::Result<std::u16string>::success(std::move(result));
}

} // namespace

WideString WideString::at(const std::size_t zero_based_position) const {
    if (zero_based_position >= units_.size()) {
        return {};
    }
    return WideString(units_.substr(zero_based_position));
}

WideString WideString::substr(
    const std::size_t zero_based_position,
    const std::size_t count
) const {
    if (zero_based_position > units_.size()) {
        return {};
    }
    return WideString(units_.substr(zero_based_position, count));
}

core::Result<WideString> WideString::append_ascii(const std::string_view ascii) {
    auto converted = ascii_units(ascii);
    if (!converted) return core::Result<WideString>::failure(converted.error());
    units_.append(converted.value());
    return core::Result<WideString>::success(WideString(units_));
}

WideString WideString::append(const WideString& other) {
    units_.append(other.units_);
    return WideString(units_);
}

core::Result<WideString> WideString::assign_ascii(const std::string_view ascii) {
    auto converted = ascii_units(ascii);
    if (!converted) return core::Result<WideString>::failure(converted.error());
    units_ = std::move(converted).value();
    return core::Result<WideString>::success(WideString(units_));
}

WideString WideString::erase(const std::size_t zero_based_position, const std::size_t count) {
    if (zero_based_position <= units_.size()) {
        units_.erase(zero_based_position, count);
    }
    return WideString(units_);
}

core::Result<WideString> WideString::insert_ascii(
    const std::size_t zero_based_position,
    const std::string_view ascii
) {
    if (zero_based_position > units_.size()) {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::invalid_value;
        diagnostic.message = "wide-string insert position is out of range";
        return core::Result<WideString>::failure(std::move(diagnostic));
    }
    auto converted = ascii_units(ascii);
    if (!converted) return core::Result<WideString>::failure(converted.error());
    units_.insert(zero_based_position, converted.value());
    return core::Result<WideString>::success(WideString(units_));
}

core::Result<WideString> WideString::replace_ascii(
    const std::size_t zero_based_position,
    const std::size_t count,
    const std::string_view ascii
) {
    if (zero_based_position > units_.size()) {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::invalid_value;
        diagnostic.message = "wide-string replace position is out of range";
        return core::Result<WideString>::failure(std::move(diagnostic));
    }
    auto converted = ascii_units(ascii);
    if (!converted) return core::Result<WideString>::failure(converted.error());
    units_.replace(zero_based_position, count, converted.value());
    return core::Result<WideString>::success(WideString(units_));
}

void WideString::reserve(const std::size_t units) {
    units_.reserve(units);
}

void WideString::resize(const std::size_t units) {
    units_.resize(units);
}

core::Result<float> WideString::compare_ascii(const std::string_view ascii) const {
    auto other = ascii_units(ascii);
    if (!other) return core::Result<float>::failure(other.error());
    const auto comparison = units_.compare(other.value());
    if (comparison < 0) return core::Result<float>::success(-1.0F);
    if (comparison > 0) return core::Result<float>::success(1.0F);
    return core::Result<float>::success(0.0F);
}

core::Result<float> WideString::find_ascii(
    const std::string_view ascii,
    const std::size_t zero_based_position
) const {
    auto converted = ascii_units(ascii);
    if (!converted) return core::Result<float>::failure(converted.error());
    const auto found = units_.find(converted.value(), zero_based_position);
    if (found == std::u16string::npos) {
        core::Diagnostic diagnostic;
        diagnostic.code = diagnostic_codes::unsupported_feature;
        diagnostic.message = "wide-string not-found numeric sentinel is unsupported";
        return core::Result<float>::failure(std::move(diagnostic));
    }
    return core::Result<float>::success(static_cast<float>(found));
}

} // namespace eawr::script
