#pragma once

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sflua_sandbox_internal.hpp"

EAWR_SFLUA_BEGIN
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lopcodes.h"
#include "lstring.h"
EAWR_SFLUA_END

namespace eawr::script::EAWR_SFLUA_NAMESPACE {
// Accessors in upstream/lbaselib.cpp, lstrlib.cpp and ltablib.cpp.
const luaL_reg* sflua_base_functions();
const luaL_reg* sflua_coroutine_functions();
lua_CFunction sflua_coroutine_wrapper();
const luaL_reg* sflua_string_functions();
lua_CFunction sflua_gfind_iterator();
lua_CFunction sflua_metered_find();
lua_CFunction sflua_metered_gfind();
lua_CFunction sflua_metered_gfind_iterator();
lua_CFunction sflua_metered_gsub();
const luaL_reg* sflua_table_functions();

struct PersistAccess {
    static Sandbox::Impl& impl(Sandbox& sandbox) noexcept;
    static ::std::unique_ptr<Sandbox> adopt(::std::unique_ptr<Sandbox::Impl> impl);
};

namespace persist_internal {

enum Kind : ::std::uint8_t {
    kind_string = 1,
    kind_table = 2,
    kind_lua_function = 3,
    kind_c_function = 4,
    kind_userdata = 5,
    kind_handle = 6,
    kind_handle_table = 7,
    kind_thread = 8,
    kind_prototype = 9,
    kind_upvalue = 10,
};

enum ValueTag : ::std::uint8_t {
    tag_nil = 0,
    tag_false = 1,
    tag_true = 2,
    tag_number = 3,
    tag_reference = 4,
};

constexpr ::std::string_view yield_function = "coroutine.yield";
constexpr int lua_frame = CI_SAVEDPC;
constexpr int calling_frame = CI_SAVEDPC | CI_CALLING;
constexpr int yield_frame = CI_C | CI_YIELD;
// Format bounds; the quotas and budget keep real states far below them.
constexpr ::std::uint32_t max_stack_slots = 1u << 20;
constexpr ::std::uint32_t max_frame_slots = 1u << 16;
constexpr ::std::uint64_t max_total_stack_slots = ::std::uint64_t{1} << 22;
constexpr ::std::uint32_t max_table_pairs = 1u << 24;
constexpr ::std::uint32_t max_prototype_depth = 200;

struct IrValue {
    ::std::uint8_t tag{tag_nil};
    ::std::uint64_t bits{};
    ::std::uint32_t reference{};
};

struct IrFrame {
    ::std::uint32_t base{};
    ::std::uint32_t top{};
    ::std::uint8_t state{};
    ::std::uint32_t savedpc{};
    ::std::int32_t tailcalls{};
};

struct IrObject {
    Kind kind{};
    ::std::string text;                        // string bytes, C function name, prototype chunk
    ::std::uint32_t first{};                   // metatable, prototype, globals, upvalue thread
    ::std::uint32_t second{};                  // environment, upvalue slot
    ::std::vector<IrValue> values;             // table pairs, C upvalues, stack, closed upvalue
    ::std::vector<::std::uint32_t> references; // Lua upvalues, handles, prototype path
    ::std::vector<IrFrame> frames;
    ::std::uint32_t handle_kind{};
    ::std::uint64_t handle_id{};
    bool flag{}; // main thread, open upvalue
    ::std::uint8_t hookmask{};
    ::std::uint8_t allowhook{};
    ::std::uint8_t hookinit{};
    ::std::int32_t basehookcount{};
    ::std::int32_t hookcount{};
    // Build results.
    void* built{};
    const Proto* prototype{};
    const CFunctionEntry* function{};
};

struct IrGraph {
    ::std::vector<IrObject> objects;
    ::std::uint32_t registry{};
    ::std::uint32_t main{};
    ::std::uint32_t handle_metatable{};
    ::std::uint32_t anchors{};
    ::std::uint32_t handles{};
    ::std::uint32_t sizes{};
    ::std::uint64_t next_identity{};
    ::std::uint64_t memory_measured{};
    ::std::uint64_t memory_charged{};
    ::std::vector<::std::uint32_t> frozen;
    ::std::vector<::std::uint32_t> protected_names;
    ::std::vector<::std::uint32_t> identities; // identity k at index k - 1
};

class GraphSaver {
public:
    GraphSaver(Sandbox::Impl& sandbox, const CFunctionTable& functions) noexcept;

    bool run(persist::ByteWriter& out, ::std::string& error);

private:
    struct Entry {
        Kind kind;
        const void* object;
    };

    ::std::uint32_t id_of(const void* object) const;

    ::std::uint32_t add(const void* object, Kind kind);

    // The object id of a value (0 for nil, booleans and numbers).
    ::std::uint32_t reference_of(const TObject* value);

    void fail(::std::string message);

    // Keys of a table in canonical order. The weak size table keeps a reference
    // key only when the key is reachable without it; those follow the other
    // keys in object id order.
    bool ordered_keys(const Table* table, ::std::vector<TObject>& keys);

    ::std::vector<const TObject*> handle_values(const Table* table);

    // Checks the thread is at a barrier; returns false with a reason.
    bool check_thread(const lua_State* thread);

    void expand(::std::uint32_t index);

    // Open upvalues of reachable threads stay open; one whose thread is
    // unreachable can never change again and is saved closed.
    void resolve_open_upvalues();

    void write_value(persist::ByteWriter& out, const TObject* value);

    template <typename Pointer>
    void write_id_list(persist::ByteWriter& out, const ::std::vector<Pointer>& objects);

    void write_object(persist::ByteWriter& out, ::std::uint32_t id);

    Sandbox::Impl& sandbox_;
    const CFunctionTable& functions_;
    ::std::vector<Entry> objects_;
    ::std::unordered_map<const void*, ::std::uint32_t> ids_;
    ::std::unordered_map<const UpVal*, ::std::pair<::std::uint32_t, ::std::uint32_t>> open_;
    ::std::size_t next_{};
    ::std::string error_;
};

class GraphValidator {
public:
    GraphValidator(IrGraph& graph, const RestoreContext& context) noexcept;

    bool run(::std::string& error);

private:
    [[nodiscard]] bool ok() const noexcept;

    void require(bool condition, ::std::string_view message);

    [[nodiscard]] const IrObject* object(::std::uint32_t id) const noexcept;

    [[nodiscard]] bool is(::std::uint32_t id, Kind kind) const noexcept;

    [[nodiscard]] bool is_table(::std::uint32_t id) const noexcept;

    [[nodiscard]] bool is_value(const IrValue& value) const noexcept;

    [[nodiscard]] bool reference_to(const IrValue& value, Kind kind) const noexcept;

    [[nodiscard]] bool anchorable(const IrValue& value) const noexcept;

    [[nodiscard]] bool string_is(const IrValue& value, ::std::string_view text) const noexcept;

    // The value of a key in a decoded table, or nullptr.
    [[nodiscard]] const IrValue* lookup(::std::uint32_t table, const IrValue& key) const;

    [[nodiscard]] const IrValue* registry_field(::std::string_view name) const;

    void check_specials();

    void check_table(::std::uint32_t id, const IrObject& table);

    void check_c_function(IrObject& function);

    void check_thread(const IrObject& thread);

    void check_object(::std::uint32_t id);

    void check_identities();

    void check_handles();

public:
    // Anchored objects, filled before the object checks.
    void collect_anchored();

private:
    IrGraph& graph_;
    const RestoreContext& context_;
    ::std::string error_;
    ::std::set<::std::uint32_t> anchored_;
    ::std::set<::std::pair<::std::uint32_t, ::std::uint32_t>> open_slots_;
    ::std::uint64_t stack_slots_{};
};

} // namespace persist_internal

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#endif // !EAWR_SFLUA_HARDWARE_TWIN
