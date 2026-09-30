#pragma once
#include "eawr/script/script_host.hpp"

#include "eawr/vfs/vfs.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Private declarations shared by the script host implementation files.
namespace eawr::script {

constexpr std::size_t max_coroutines = 4'096;
constexpr std::size_t max_queue_entries = 1'000'000;
constexpr std::uint64_t max_instructions = 10'000'000;
constexpr int hook_granularity = 1;
constexpr std::string_view smoke_path =
    "data/scripts/story/story_empire_acti_m02_fondor_land.lua";

[[nodiscard]] core::Diagnostic make_core_diagnostic(
    const std::string_view code,
    std::string message,
    const std::optional<std::string_view> logical_path = std::nullopt
);
[[nodiscard]] std::string lua_message(lua_State* state);
[[nodiscard]] std::optional<std::uint64_t> lua_error_line(const std::string_view message);

struct ScriptHost::Impl final {
    struct Binding {
        std::string name;
        std::string signature;
        ApiCallback callback;
    };

    struct ApiUserdata {
        Impl* host{nullptr};
        InstanceId instance{0};
        std::size_t binding{0};
    };

    struct ThreadSlot {
        int registry_reference{LUA_NOREF};
        int function_reference{LUA_NOREF};
        bool live{false};
        bool first_resume{true};
        bool restart_on_next_resume{false};
        std::optional<ScriptValue> initial;
        std::vector<int> callbacks;
        std::vector<int> parameters;
    };

    struct EventHandler {
        std::uint64_t id{0};
        int function_reference{LUA_NOREF};
    };

    struct LoadedSource {
        std::string canonical_path;
        std::string source_id;
    };

    struct Instance {
        Impl* owner{nullptr};
        InstanceId id{0};
        lua_State* state{nullptr};
        std::string logical_path;
        std::optional<RetailIdentity> retail_identity;
        bool closing{false};
        bool destroy_requested{false};
        std::uint32_t active_operations{0};
        std::optional<std::uint32_t> current_coroutine;
        std::uint64_t instruction_count{0};
        std::vector<ThreadSlot> threads;
        std::unordered_map<std::string, std::vector<EventHandler>> handlers;
        std::uint64_t next_handler_id{1};
        std::uint64_t mutation_generation{0};
        std::unordered_map<std::uint64_t, int> function_references;
        std::uint64_t next_function_token{1};
        std::vector<ModuleRequest> module_trace;
        std::unordered_map<std::string, LoadedSource> loaded_sources;
        std::optional<ScriptDiagnostic> pending_diagnostic;
        std::string current_operation;
    };

    explicit Impl(const vfs::Vfs& source) : files(source) {}

    ~Impl() {
        for (auto& [id, instance] : instances) {
            (void)id;
            close_instance(*instance);
        }
    }

    static inline char context_key{};
    static constexpr const char* api_metatable = "EAWR.ScriptApi";

    const vfs::Vfs& files;
    InstanceId next_instance{1};
    std::vector<Binding> bindings;
    std::unordered_map<std::string, std::size_t> binding_by_name;
    std::unordered_map<InstanceId, std::unique_ptr<Instance>> instances;
    std::vector<ApiInvocation> invocation_log;

    [[nodiscard]] static Instance* context(lua_State* state);
    static void set_context(lua_State* state, Instance* instance);
    static void remember_source(
        Instance& instance,
        const std::string_view requested_path,
        const vfs::AssetRecord& record
    );
    [[nodiscard]] static const LoadedSource* loaded_source(
        const Instance& instance,
        const char* frame_source
    );

    struct StackContext {
        std::string traceback;
        const LoadedSource* source{nullptr};
        std::optional<std::uint64_t> line;
    };

    [[nodiscard]] static StackContext stack_context(Instance& instance, lua_State* state);
    [[nodiscard]] static ScriptDiagnostic make_script_diagnostic(
        Instance& instance,
        const std::string_view code,
        const std::string_view operation,
        std::string message,
        lua_State* state,
        std::optional<std::string> api_name = std::nullopt,
        const core::Diagnostic* cause = nullptr
    );
    static int capture_runtime_error(lua_State* state);
    static int protected_call(
        Instance& instance,
        lua_State* state,
        const int arguments,
        const int results,
        const std::string_view operation
    );
    static int raise_pending(lua_State* state, Instance& instance, ScriptDiagnostic diagnostic);
    static void instruction_hook(lua_State* state, lua_Debug*);
    static void begin_operation(Instance& instance, lua_State* state);
    static void end_operation(lua_State* state);
    [[nodiscard]] static bool is_api_value(lua_State* state, const int index);
    [[nodiscard]] static ApiUserdata* api_value(lua_State* state, const int index);
    [[nodiscard]] core::Result<ScriptValue> read_value(
        Instance& instance,
        lua_State* state,
        const int index,
        std::unordered_set<const void*>& table_stack
    );
    [[nodiscard]] core::Result<ScriptValue> read_value(
        Instance& instance,
        lua_State* state,
        const int index
    );
    [[nodiscard]] core::Result<void> push_value(
        Instance& instance,
        lua_State* state,
        const ScriptValue& value
    );
    static int api_call(lua_State* state, const int userdata_index, const int first_argument);
    static int api_trampoline(lua_State* state);
    static int function_call(lua_State* state);
    void push_api_value(Instance& instance, const std::size_t binding);
    void install_api(Instance& instance, const std::size_t binding);
    [[nodiscard]] core::Result<void> push_chunk(
        Instance& instance,
        lua_State* state,
        const std::string& logical_path
    );
    static int vfs_loadfile(lua_State* state);
    static int vfs_dofile(lua_State* state);
    static std::string substitute_module(std::string pattern, const std::string_view name);
    static int vfs_require(lua_State* state);
    [[nodiscard]] core::Result<std::uint32_t> create_thread(
        Instance& instance,
        const std::string_view function,
        std::optional<ScriptValue> initial
    );
    static int create_thread_lua(lua_State* state);
    static int kill_thread(lua_State* state);
    static int kill_all_threads(lua_State* state);
    static int register_event(lua_State* state);
    static int cancel_event(lua_State* state);
    static int get_event(lua_State* state);
    static int get_event_params(lua_State* state);
    static int pop_event_queue(lua_State* state, const bool callback);
    static int reset_events(lua_State* state);
    static int queue_event(lua_State* state);
    void install_infrastructure(Instance& instance);
    void clear_queue(Instance& instance, std::vector<int>& queue);
    void release_thread(Instance& instance, ThreadSlot& slot);
    void close_instance(Instance& instance);
    [[nodiscard]] ScriptDiagnostic operation_error(
        Instance& instance,
        lua_State* state,
        const std::string_view operation
    );
};

} // namespace eawr::script
