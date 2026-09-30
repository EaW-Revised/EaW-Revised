#pragma once

#include "eawr/core/result.hpp"
#include "eawr/script/pglua.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::vfs {
class Vfs;
}

namespace eawr::script {

using InstanceId = std::uint64_t;

struct FunctionReference {
    InstanceId instance{0};
    std::uint64_t token{0};
    friend bool operator==(const FunctionReference&, const FunctionReference&) = default;
};

struct HostReference {
    InstanceId instance{0};
    std::uint64_t token{0};
    friend bool operator==(const HostReference&, const HostReference&) = default;
};

struct ThreadReference {
    InstanceId instance{0};
    std::uint32_t slot{0};
    friend bool operator==(const ThreadReference&, const ThreadReference&) = default;
};

class ScriptValue final {
public:
    struct Nil {
        friend bool operator==(Nil, Nil) = default;
    };
    using Sequence = std::vector<ScriptValue>;
    using SequenceStorage = std::shared_ptr<const Sequence>;
    using Storage = std::variant<
        Nil,
        bool,
        float,
        std::string,
        SequenceStorage,
        FunctionReference,
        HostReference,
        ThreadReference>;

    ScriptValue() = default;
    explicit ScriptValue(bool value) : storage_(value) {}
    explicit ScriptValue(float value) : storage_(value) {}
    explicit ScriptValue(std::string value) : storage_(std::move(value)) {}
    explicit ScriptValue(const char* value) : storage_(std::string(value)) {}
    explicit ScriptValue(Sequence value)
        : storage_(std::make_shared<const Sequence>(std::move(value))) {}
    explicit ScriptValue(FunctionReference value) : storage_(value) {}
    explicit ScriptValue(HostReference value) : storage_(value) {}
    explicit ScriptValue(ThreadReference value) : storage_(value) {}

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

private:
    Storage storage_{};
};

using ValueList = std::vector<ScriptValue>;

struct ScriptDiagnostic {
    core::Diagnostic diagnostic;
    std::optional<InstanceId> instance;
    std::optional<std::uint32_t> coroutine;
    std::string operation;
    std::optional<std::string> api_name;
    std::string traceback;
    std::optional<std::string> source_kind;
    std::optional<std::int32_t> prototype_id;
    std::optional<std::uint32_t> pc;
};

struct CallOutcome {
    std::optional<ScriptValue> result;
    std::optional<ScriptDiagnostic> error;
    [[nodiscard]] bool succeeded() const noexcept { return !error.has_value(); }
};

enum class ResumeState : std::uint8_t {
    live,
    ended,
};

struct ResumeOutcome {
    ResumeState state{ResumeState::ended};
    std::optional<ScriptDiagnostic> error;
    [[nodiscard]] bool succeeded() const noexcept { return !error.has_value(); }
};

struct DispatchOutcome {
    std::size_t invoked{0};
    std::optional<ScriptDiagnostic> error;
    [[nodiscard]] bool succeeded() const noexcept { return !error.has_value(); }
};

struct ApiCallContext {
    InstanceId instance{0};
    std::optional<std::uint32_t> coroutine;
    std::string_view api_name;
};

using ApiCallback = std::function<core::Result<ValueList>(
    const ApiCallContext&,
    const ValueList&) >;

struct ApiInvocation {
    InstanceId instance{0};
    std::optional<std::uint32_t> coroutine;
    std::string name;
    ValueList arguments;
};

struct ModuleRequest {
    std::string name;
    std::optional<std::string> resolved_path;
    bool cache_hit{false};
};

struct ScriptCapabilities {
    bool fork_security{false};
    bool state_pooling{false};
    bool whole_state_persistence{false};
    bool multi_value_coroutine_yield{false};
    bool non_ascii_wide_conversion{false};
};

class WideString final {
public:
    WideString() = default;
    explicit WideString(std::u16string units) : units_(std::move(units)) {}

    [[nodiscard]] std::size_t size() const noexcept { return units_.size(); }
    [[nodiscard]] const std::u16string& units() const noexcept { return units_; }
    [[nodiscard]] WideString at(std::size_t zero_based_position) const;
    [[nodiscard]] WideString substr(std::size_t zero_based_position, std::size_t count) const;
    [[nodiscard]] core::Result<WideString> append_ascii(std::string_view ascii);
    [[nodiscard]] WideString append(const WideString& other);
    [[nodiscard]] core::Result<WideString> assign_ascii(std::string_view ascii);
    [[nodiscard]] WideString erase(std::size_t zero_based_position, std::size_t count);
    [[nodiscard]] core::Result<WideString> insert_ascii(
        std::size_t zero_based_position,
        std::string_view ascii
    );
    [[nodiscard]] core::Result<WideString> replace_ascii(
        std::size_t zero_based_position,
        std::size_t count,
        std::string_view ascii
    );
    void reserve(std::size_t units);
    void resize(std::size_t units);
    [[nodiscard]] core::Result<float> compare_ascii(std::string_view ascii) const;
    [[nodiscard]] core::Result<float> find_ascii(
        std::string_view ascii,
        std::size_t zero_based_position = 0
    ) const;

private:
    std::u16string units_;
};

class ScriptHost final {
public:
    explicit ScriptHost(const vfs::Vfs& files);
    ~ScriptHost();
    ScriptHost(ScriptHost&&) noexcept;
    ScriptHost& operator=(ScriptHost&&) noexcept;
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    [[nodiscard]] core::Result<InstanceId> load(
        std::string script_path,
        std::vector<std::string> script_directories = {},
        std::optional<RetailIdentity> retail_identity = std::nullopt
    );
    [[nodiscard]] CallOutcome start(
        InstanceId instance,
        std::string_view function,
        const ValueList& arguments = {}
    );
    [[nodiscard]] ResumeOutcome resume(ThreadReference thread);
    [[nodiscard]] DispatchOutcome dispatch(
        InstanceId instance,
        std::string_view event,
        const ValueList& arguments = {}
    );
    [[nodiscard]] core::Result<void> destroy(InstanceId instance);

    // An empty callback declares a known-but-unimplemented engine binding.  It
    // is callable only to produce the distinct missing-engine-API diagnostic.
    [[nodiscard]] core::Result<void> register_api(
        std::string name,
        std::string signature,
        ApiCallback callback = {}
    );

    [[nodiscard]] const std::vector<ApiInvocation>& invocations() const noexcept;
    [[nodiscard]] core::Result<std::vector<ModuleRequest>> module_trace(InstanceId instance) const;
    [[nodiscard]] static constexpr ScriptCapabilities capabilities() noexcept { return {}; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::script
