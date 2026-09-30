// Canonical persistence of an authoritative Lua state (#248); the format and
// its rules are in docs/lua-persistence.md.
//
// Saving is a read-only walk of the reachable graph from the registry and the
// main thread, breadth first in canonical order. Restoring decodes the whole
// graph into a plain intermediate form and validates it before it creates a
// single Lua object; building then only allocates, with collection disabled,
// and any failure discards the new state.
//
// Authoritative profile only: the hardware-double benchmark twin
// (EAWR_SFLUA_HARDWARE_TWIN) compiles this file empty.

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
    static Sandbox::Impl& impl(Sandbox& sandbox) noexcept { return *sandbox.impl_; }
    static ::std::unique_ptr<Sandbox> adopt(::std::unique_ptr<Sandbox::Impl> impl) {
        return ::std::unique_ptr<Sandbox>(new Sandbox(::std::move(impl)));
    }
};

namespace {

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

void append_library(::std::vector<CFunctionEntry>& out, ::std::string_view prefix, const luaL_reg* functions) {
    for (const luaL_reg* entry = functions; entry->name != nullptr; ++entry) {
        out.push_back({::std::string(prefix) + entry->name, entry->func, UpvalueShape::none});
    }
}

bool is_nan(const lua_Number& value) noexcept { return !(value == value); }

// The integer a number holds exactly, if it is one in [low, high].
bool whole_number(::std::uint64_t bits, int low, int high, int& out) noexcept {
    const lua_Number value = lua_Number::from_repr(bits);
    const int whole = static_cast<int>(value);
    if (!(lua_Number(whole) == value) || whole < low || whole > high) return false;
    out = whole;
    return true;
}

// ---- Save ----

class GraphSaver {
public:
    GraphSaver(Sandbox::Impl& sandbox, const CFunctionTable& functions) noexcept : sandbox_(sandbox), functions_(functions) {}

    bool run(persist::ByteWriter& out, ::std::string& error) {
        lua_State* state = sandbox_.state;
        const ::std::uint32_t registry = reference_of(registry(state));
        const ::std::uint32_t main = add(state, kind_thread);
        while (error_.empty() && next_ < objects_.size()) expand(static_cast<::std::uint32_t>(next_++));
        if (error_.empty()) resolve_open_upvalues();
        const ::std::uint32_t handle_metatable = id_of(sandbox_.handle_metatable);
        const ::std::uint32_t anchors = id_of(sandbox_.anchors);
        const ::std::uint32_t handles = id_of(sandbox_.handles);
        const ::std::uint32_t sizes = id_of(sandbox_.sizes);
        if (error_.empty() && (handle_metatable == 0 || anchors == 0 || handles == 0 || sizes == 0)) {
            error_ = "sandbox tables are not reachable from the registry";
        }
        if (!error_.empty()) {
            error = error_;
            return false;
        }
        out.u32(static_cast<::std::uint32_t>(objects_.size()));
        for (const ::std::uint32_t id : {registry, main, handle_metatable, anchors, handles, sizes}) out.u32(id);
        out.u64(sandbox_.next_identity);
        out.u64(sandbox_.memory_measured);
        out.u64(sandbox_.memory_charged);
        write_id_list(out, sandbox_.frozen);
        ::std::vector<const void*> protected_names(sandbox_.protected_names.begin(), sandbox_.protected_names.end());
        write_id_list(out, protected_names);
        for (::std::uint32_t id = 1; id <= objects_.size(); ++id) write_object(out, id);
        if (!error_.empty()) {
            error = error_;
            return false;
        }
        return true;
    }

private:
    struct Entry {
        Kind kind;
        const void* object;
    };

    ::std::uint32_t id_of(const void* object) const {
        const auto found = ids_.find(object);
        return found == ids_.end() ? 0 : found->second;
    }

    ::std::uint32_t add(const void* object, Kind kind) {
        const auto [found, inserted] = ids_.emplace(object, static_cast<::std::uint32_t>(objects_.size() + 1));
        if (inserted) objects_.push_back(Entry{kind, object});
        return found->second;
    }

    // The object id of a value (0 for nil, booleans and numbers).
    ::std::uint32_t reference_of(const TObject* value) {
        switch (ttype(value)) {
        case LUA_TNIL:
        case LUA_TBOOLEAN:
        case LUA_TNUMBER:
            return 0;
        case LUA_TSTRING:
            return add(gcvalue(value), kind_string);
        case LUA_TTABLE:
            return add(gcvalue(value), hvalue(value) == sandbox_.handles ? kind_handle_table : kind_table);
        case LUA_TFUNCTION:
            return add(gcvalue(value), clvalue(value)->c.isC ? kind_c_function : kind_lua_function);
        case LUA_TUSERDATA:
            return add(gcvalue(value), is_handle(sandbox_, value) ? kind_handle : kind_userdata);
        case LUA_TTHREAD:
            return add(gcvalue(value), kind_thread);
        default:
            if (error_.empty()) error_ = "light userdata cannot be saved";
            return 0;
        }
    }

    void fail(::std::string message) {
        if (error_.empty()) error_ = ::std::move(message);
    }

    // Keys of a table in canonical order. The weak size table keeps a reference
    // key only when the key is reachable without it; those follow the other
    // keys in object id order.
    bool ordered_keys(const Table* table, ::std::vector<TObject>& keys) {
        if (table != sandbox_.sizes) {
            if (!collect_canonical_keys(sandbox_, table)) {
                fail("table key without canonical order");
                return false;
            }
            keys = sandbox_.scratch_keys;
            return true;
        }
        ::std::vector<TObject> plain;
        ::std::vector<::std::pair<::std::uint32_t, TObject>> weak;
        for (int index = 0; index < table->sizearray; ++index) {
            if (ttisnil(&table->array[index])) continue;
            TObject key;
            setnvalue(&key, static_cast<lua_Number>(index + 1));
            plain.push_back(key);
        }
        for (int index = 0; index < sizenode(table); ++index) {
            const Node* node = gnode(table, index);
            if (ttisnil(gval(node))) continue;
            if (!is_reference(gkey(node)) || is_handle(sandbox_, gkey(node))) {
                plain.push_back(*gkey(node));
            } else if (const ::std::uint32_t id = id_of(gcvalue(gkey(node))); id != 0 && !iscollectable(gval(node))) {
                // lauxlib stores counts here; a reference value would need
                // ephemeron tracing, so such an entry is not state it writes.
                weak.emplace_back(id, *gkey(node));
            }
        }
        ::std::sort(plain.begin(), plain.end(), [&](const TObject& left, const TObject& right) {
            return key_less(sandbox_, &left, &right);
        });
        ::std::sort(weak.begin(), weak.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        keys = ::std::move(plain);
        for (const auto& [id, key] : weak) keys.push_back(key);
        return true;
    }

    ::std::vector<const TObject*> handle_values(const Table* table) {
        ::std::vector<const TObject*> values;
        for (int index = 0; index < sizenode(table); ++index) {
            const TObject* value = gval(gnode(table, index));
            if (ttisnil(value)) continue;
            if (!is_handle(sandbox_, value)) {
                fail("handle table holds a value that is not a handle");
                return {};
            }
            values.push_back(value);
        }
        if (table->sizearray != 0) {
            for (int index = 0; index < table->sizearray; ++index) {
                if (!ttisnil(&table->array[index])) fail("handle table has an array part");
            }
        }
        ::std::sort(values.begin(), values.end(), [&](const TObject* left, const TObject* right) {
            return key_less(sandbox_, left, right);
        });
        return values;
    }

    // Checks the thread is at a barrier; returns false with a reason.
    bool check_thread(const lua_State* thread) {
        if (thread->nCcalls != 0 || thread->errfunc != 0 || thread->errorJmp != nullptr) {
            fail("a thread is running (the state is not at a tick barrier)");
            return false;
        }
        if (thread->hookmask != sandbox_hook_mask || thread->hook != sandbox_count_hook() || thread->allowhook != 1 ||
            thread->hookinit != 0 || thread->basehookcount != sandbox_.limits.hook_granularity || thread->hookcount < 1 ||
            thread->hookcount > thread->basehookcount) {
            fail("a thread does not run under the instruction budget hook");
            return false;
        }
        const ::std::ptrdiff_t frames = thread->ci - thread->base_ci + 1;
        if (thread->base_ci->state != CI_C) {
            fail("a thread's base frame is not a C frame");
            return false;
        }
        if (thread == sandbox_.state && (frames != 1 || thread->top != thread->base)) {
            fail("the main thread is not idle (the state is not at a tick barrier): " + ::std::to_string(frames) + " frames, " +
                 ::std::to_string(thread->top - thread->base) + " values");
            return false;
        }
        if (frames == 1) return true;
        const CallInfo* top_frame = thread->ci;
        const TObject* yielder = top_frame->base - 1;
        const CFunctionEntry* entry = ttisfunction(yielder) && clvalue(yielder)->c.isC
            ? functions_.find(clvalue(yielder)->c.f)
            : nullptr;
        if (frames < 3 || top_frame->state != yield_frame || entry == nullptr || entry->name != yield_function) {
            fail("unsupported C continuation: a coroutine is suspended outside coroutine.yield");
            return false;
        }
        for (const CallInfo* frame = thread->base_ci + 1; frame < top_frame; ++frame) {
            const int expected = frame + 1 == top_frame ? lua_frame : calling_frame;
            if (frame->state != expected || !ttisfunction(frame->base - 1) || clvalue(frame->base - 1)->c.isC) {
                fail("unsupported C continuation: a suspended coroutine holds a C or running frame");
                return false;
            }
        }
        return true;
    }

    void expand(::std::uint32_t index) {
        const Entry entry = objects_[index];
        switch (entry.kind) {
        case kind_string:
        case kind_handle:
        case kind_prototype:
            if (entry.kind == kind_prototype &&
                !sandbox_.prototype_paths.contains(static_cast<const Proto*>(entry.object))) {
                fail("a function prototype was not compiled by the sandbox");
            }
            return;
        case kind_table: {
            const auto* table = static_cast<const Table*>(entry.object);
            if (table->metatable != hvalue(defaultmeta(sandbox_.state))) add(table->metatable, kind_table);
            ::std::vector<TObject> keys;
            if (!ordered_keys(table, keys)) return;
            for (const TObject& key : keys) {
                if (table == sandbox_.sizes && is_reference(&key) && !is_handle(sandbox_, &key)) continue;
                reference_of(&key);
                reference_of(luaH_get(const_cast<Table*>(table), &key));
            }
            return;
        }
        case kind_handle_table:
            for (const TObject* value : handle_values(static_cast<const Table*>(entry.object))) reference_of(value);
            return;
        case kind_lua_function: {
            const LClosure& closure = static_cast<const Closure*>(entry.object)->l;
            add(closure.p, kind_prototype);
            if (!ttistable(&closure.g)) fail("a function environment is not a table");
            reference_of(&closure.g);
            for (int index = 0; index < closure.nupvalues; ++index) add(closure.upvals[index], kind_upvalue);
            return;
        }
        case kind_c_function: {
            const CClosure& closure = static_cast<const Closure*>(entry.object)->c;
            if (functions_.find(closure.f) == nullptr) fail("a C function has no stable name");
            for (int index = 0; index < closure.nupvalues; ++index) reference_of(&closure.upvalue[index]);
            return;
        }
        case kind_userdata: {
            const Udata* userdata = static_cast<const Udata*>(entry.object);
            if (userdata->uv.len != 0) fail("userdata with a payload other than a host handle cannot be saved");
            if (userdata->uv.metatable != hvalue(defaultmeta(sandbox_.state))) add(userdata->uv.metatable, kind_table);
            return;
        }
        case kind_thread: {
            const auto* thread = static_cast<const lua_State*>(entry.object);
            if (!check_thread(thread)) return;
            if (!ttistable(gt(thread))) fail("a thread's globals are not a table");
            reference_of(gt(thread));
            for (const TObject* slot = thread->stack; slot < thread->top; ++slot) reference_of(slot);
            return;
        }
        case kind_upvalue:
            // An open upvalue's value lives on its thread's stack; it is also
            // what a closed copy holds when that thread is unreachable.
            reference_of(static_cast<const UpVal*>(entry.object)->v);
            return;
        }
    }

    // Open upvalues of reachable threads stay open; one whose thread is
    // unreachable can never change again and is saved closed.
    void resolve_open_upvalues() {
        for (const Entry& entry : objects_) {
            if (entry.kind != kind_thread) continue;
            const auto* thread = static_cast<const lua_State*>(entry.object);
            for (const GCObject* link = thread->openupval; link != nullptr; link = link->gch.next) {
                const UpVal* upvalue = gcotouv(const_cast<GCObject*>(link));
                open_[upvalue] = {id_of(thread), static_cast<::std::uint32_t>(upvalue->v - thread->stack)};
            }
        }
    }

    void write_value(persist::ByteWriter& out, const TObject* value) {
        switch (ttype(value)) {
        case LUA_TNIL:
            out.u8(tag_nil);
            return;
        case LUA_TBOOLEAN:
            out.u8(bvalue(value) != 0 ? tag_true : tag_false);
            return;
        case LUA_TNUMBER:
            out.u8(tag_number);
            out.u64(nvalue(value).repr);
            return;
        default:
            out.u8(tag_reference);
            out.u32(ttislightuserdata(value) ? 0 : id_of(gcvalue(value)));
            return;
        }
    }

    template <typename Pointer>
    void write_id_list(persist::ByteWriter& out, const ::std::vector<Pointer>& objects) {
        ::std::vector<::std::uint32_t> ids;
        for (const Pointer object : objects) {
            if (const ::std::uint32_t id = id_of(object); id != 0) ids.push_back(id);
        }
        ::std::sort(ids.begin(), ids.end());
        ids.erase(::std::unique(ids.begin(), ids.end()), ids.end());
        out.u32(static_cast<::std::uint32_t>(ids.size()));
        for (const ::std::uint32_t id : ids) out.u32(id);
    }

    void write_object(persist::ByteWriter& out, ::std::uint32_t id) {
        const Entry entry = objects_[id - 1];
        out.u8(entry.kind);
        switch (entry.kind) {
        case kind_string: {
            const auto* string = static_cast<const TString*>(entry.object);
            out.text(::std::string_view(getstr(string), string->tsv.len));
            return;
        }
        case kind_table: {
            const auto* table = static_cast<const Table*>(entry.object);
            out.u32(id_of(table->metatable)); // the default metatable has no id
            ::std::vector<TObject> keys;
            if (!ordered_keys(table, keys)) return;
            out.u32(static_cast<::std::uint32_t>(keys.size()));
            for (const TObject& key : keys) {
                write_value(out, &key);
                write_value(out, luaH_get(const_cast<Table*>(table), &key));
            }
            return;
        }
        case kind_handle_table: {
            const ::std::vector<const TObject*> values = handle_values(static_cast<const Table*>(entry.object));
            out.u32(static_cast<::std::uint32_t>(values.size()));
            for (const TObject* value : values) out.u32(id_of(gcvalue(value)));
            return;
        }
        case kind_lua_function: {
            const LClosure& closure = static_cast<const Closure*>(entry.object)->l;
            out.u32(id_of(closure.p));
            out.u32(id_of(gcvalue(&closure.g)));
            out.u8(closure.nupvalues);
            for (int index = 0; index < closure.nupvalues; ++index) out.u32(id_of(closure.upvals[index]));
            return;
        }
        case kind_c_function: {
            const CClosure& closure = static_cast<const Closure*>(entry.object)->c;
            out.text(functions_.find(closure.f)->name);
            out.u8(closure.nupvalues);
            for (int index = 0; index < closure.nupvalues; ++index) write_value(out, &closure.upvalue[index]);
            return;
        }
        case kind_userdata:
            out.u32(id_of(static_cast<const Udata*>(entry.object)->uv.metatable));
            return;
        case kind_handle: {
            const auto* userdata = static_cast<const Udata*>(entry.object);
            const auto& payload = *reinterpret_cast<const HandlePayload*>(userdata + 1);
            out.u32(payload.kind);
            out.u64(payload.id);
            return;
        }
        case kind_thread: {
            const auto* thread = static_cast<const lua_State*>(entry.object);
            out.u8(thread == sandbox_.state ? 1 : 0);
            out.u32(id_of(gcvalue(gt(thread))));
            out.u8(thread->hookmask);
            out.u8(thread->allowhook);
            out.u8(thread->hookinit);
            out.i32(thread->basehookcount);
            out.i32(thread->hookcount);
            out.u32(static_cast<::std::uint32_t>(thread->top - thread->stack));
            for (const TObject* slot = thread->stack; slot < thread->top; ++slot) write_value(out, slot);
            out.u32(static_cast<::std::uint32_t>(thread->ci - thread->base_ci + 1));
            for (const CallInfo* frame = thread->base_ci; frame <= thread->ci; ++frame) {
                out.u32(static_cast<::std::uint32_t>(frame->base - thread->stack));
                out.u32(static_cast<::std::uint32_t>(frame->top - thread->stack));
                out.u8(static_cast<::std::uint8_t>(frame->state));
                if ((frame->state & CI_C) == 0) {
                    const Proto* prototype = clvalue(frame->base - 1)->l.p;
                    out.u32(static_cast<::std::uint32_t>(frame->u.l.savedpc - prototype->code));
                    out.i32(frame->u.l.tailcalls);
                }
            }
            return;
        }
        case kind_prototype: {
            const auto* prototype = static_cast<const Proto*>(entry.object);
            out.text(::std::string_view(getstr(prototype->source), prototype->source->tsv.len));
            const ::std::vector<::std::uint32_t>& path = sandbox_.prototype_paths.at(prototype);
            out.u32(static_cast<::std::uint32_t>(path.size()));
            for (const ::std::uint32_t index : path) out.u32(index);
            return;
        }
        case kind_upvalue: {
            const auto* upvalue = static_cast<const UpVal*>(entry.object);
            const auto found = open_.find(upvalue);
            if (upvalue->v != &upvalue->value && found != open_.end() && found->second.first != 0) {
                out.u8(1);
                out.u32(found->second.first);
                out.u32(found->second.second);
            } else {
                out.u8(0);
                write_value(out, upvalue->v);
            }
            return;
        }
        }
    }

    Sandbox::Impl& sandbox_;
    const CFunctionTable& functions_;
    ::std::vector<Entry> objects_;
    ::std::unordered_map<const void*, ::std::uint32_t> ids_;
    ::std::unordered_map<const UpVal*, ::std::pair<::std::uint32_t, ::std::uint32_t>> open_;
    ::std::size_t next_{};
    ::std::string error_;
};

// ---- Logical size ----

// The logical memory measurement (docs/lua-sandbox.md): the objects GraphSaver
// reaches, over the same edges plus a prototype's nested prototypes and
// strings, by the fixed sizes of logical_size. A state and its restored copy
// hold the same reachable graph, so they measure the same; collection timing,
// capacities and dead keys do not count. The size table's reference keys are
// weak and it counts as an empty table.
class LogicalSizer {
public:
    explicit LogicalSizer(const Sandbox::Impl& sandbox) noexcept : sandbox_(sandbox) {}

    ::std::uint64_t run() {
        lua_State* state = sandbox_.state;
        visit(registry(state));
        add(state, LUA_TTHREAD);
        while (!pending_.empty()) {
            const auto [object, type] = pending_.back();
            pending_.pop_back();
            expand(object, type);
        }
        return bytes_;
    }

private:
    void add(const void* object, int type) {
        if (seen_.insert(object).second) pending_.emplace_back(object, type);
    }

    void visit(const TObject* value) {
        if (iscollectable(value)) add(gcvalue(value), ttype(value));
    }

    void add_string(const TString* string) {
        if (string != nullptr) add(string, LUA_TSTRING);
    }

    void expand(const void* object, int type) {
        switch (type) {
        case LUA_TSTRING:
            bytes_ += logical_size::string + static_cast<const TString*>(object)->tsv.len;
            return;
        case LUA_TTABLE: {
            const auto* table = static_cast<const Table*>(object);
            bytes_ += logical_size::table;
            if (table->metatable != hvalue(defaultmeta(sandbox_.state))) add(table->metatable, LUA_TTABLE);
            if (table == sandbox_.sizes) return;
            const bool handles = table == sandbox_.handles;
            for (int index = 0; index < table->sizearray; ++index) {
                if (ttisnil(&table->array[index])) continue;
                bytes_ += logical_size::table_entry;
                visit(&table->array[index]);
            }
            for (int index = 0; index < sizenode(table); ++index) {
                const Node* node = gnode(table, index);
                if (ttisnil(gval(node))) continue;
                bytes_ += logical_size::table_entry;
                if (!handles) visit(gkey(node));
                visit(gval(node));
            }
            return;
        }
        case LUA_TFUNCTION: {
            const Closure* closure = static_cast<const Closure*>(object);
            bytes_ += logical_size::closure;
            if (closure->c.isC) {
                bytes_ += closure->c.nupvalues * logical_size::c_upvalue;
                for (int index = 0; index < closure->c.nupvalues; ++index) visit(&closure->c.upvalue[index]);
                return;
            }
            bytes_ += closure->l.nupvalues * logical_size::upvalue_slot;
            add(closure->l.p, prototype_type);
            visit(&closure->l.g);
            for (int index = 0; index < closure->l.nupvalues; ++index) add(closure->l.upvals[index], upvalue_type);
            return;
        }
        case LUA_TUSERDATA: {
            const Udata* userdata = static_cast<const Udata*>(object);
            bytes_ += logical_size::userdata + userdata->uv.len;
            if (userdata->uv.metatable != hvalue(defaultmeta(sandbox_.state))) add(userdata->uv.metatable, LUA_TTABLE);
            return;
        }
        case LUA_TTHREAD: {
            const auto* thread = static_cast<const lua_State*>(object);
            const ::std::ptrdiff_t used = thread->top - thread->stack;
            bytes_ += logical_size::thread;
            if (used > logical_size::stack_allowance) {
                bytes_ += static_cast<::std::uint64_t>(used - logical_size::stack_allowance) * logical_size::stack_slot;
            }
            visit(gt(thread));
            for (const TObject* slot = thread->stack; slot < thread->top; ++slot) visit(slot);
            return;
        }
        case upvalue_type:
            bytes_ += logical_size::upvalue;
            visit(static_cast<const UpVal*>(object)->v);
            return;
        case prototype_type: {
            // The prototype's arrays, its nested prototypes and the strings
            // it holds; a restore compiles the same chunk, so its copy holds
            // the same.
            const auto* prototype = static_cast<const Proto*>(object);
            bytes_ += logical_size::prototype +
                logical_size::prototype_instruction * static_cast<::std::uint64_t>(prototype->sizecode) +
                logical_size::prototype_constant * static_cast<::std::uint64_t>(prototype->sizek) +
                logical_size::prototype_child * static_cast<::std::uint64_t>(prototype->sizep) +
                logical_size::prototype_local * static_cast<::std::uint64_t>(prototype->sizelocvars) +
                logical_size::prototype_upvalue_name * static_cast<::std::uint64_t>(prototype->sizeupvalues);
            add_string(prototype->source);
            for (int index = 0; index < prototype->sizek; ++index) visit(&prototype->k[index]);
            for (int index = 0; index < prototype->sizep; ++index) add(prototype->p[index], prototype_type);
            for (int index = 0; index < prototype->sizelocvars; ++index) add_string(prototype->locvars[index].varname);
            for (int index = 0; index < prototype->sizeupvalues; ++index) add_string(prototype->upvalues[index]);
            return;
        }
        default:
            return;
        }
    }

    static constexpr int upvalue_type = -1;
    static constexpr int prototype_type = -2;

    const Sandbox::Impl& sandbox_;
    ::std::unordered_set<const void*> seen_;
    ::std::vector<::std::pair<const void*, int>> pending_;
    ::std::uint64_t bytes_{};
};

// ---- Restore: decoded form ----

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

IrValue read_value(persist::ByteReader& in) {
    IrValue value;
    value.tag = in.u8();
    if (value.tag == tag_number) value.bits = in.u64();
    if (value.tag == tag_reference) value.reference = in.u32();
    if (value.tag > tag_reference) in.fail();
    return value;
}

// Reads `count` values; false once the reader fails.
bool read_values(persist::ByteReader& in, ::std::vector<IrValue>& values, ::std::size_t count) {
    for (::std::size_t index = 0; index < count; ++index) {
        IrValue* value = in.append(values);
        if (value == nullptr) return false;
        *value = read_value(in);
    }
    return in.ok();
}

bool read_ids(persist::ByteReader& in, ::std::vector<::std::uint32_t>& ids, ::std::size_t count) {
    for (::std::size_t index = 0; index < count; ++index) {
        ::std::uint32_t* id = in.append(ids);
        if (id == nullptr) return false;
        *id = in.u32();
    }
    return in.ok();
}

// A counted ID list of at most `limit` entries.
bool read_id_list(persist::ByteReader& in, ::std::vector<::std::uint32_t>& ids, ::std::uint32_t limit = UINT32_MAX) {
    return read_ids(in, ids, in.count(4, limit));
}

// Smallest object encoding: a closed upvalue holding nil (kind, flag, tag).
constexpr ::std::size_t min_object_bytes = 3;

bool read_graph(persist::ByteReader& in, IrGraph& graph) {
    const ::std::uint32_t object_count = in.count(min_object_bytes);
    graph.registry = in.u32();
    graph.main = in.u32();
    graph.handle_metatable = in.u32();
    graph.anchors = in.u32();
    graph.handles = in.u32();
    graph.sizes = in.u32();
    graph.next_identity = in.u64();
    graph.memory_measured = in.u64();
    graph.memory_charged = in.u64();
    if (!read_id_list(in, graph.frozen, object_count) || !read_id_list(in, graph.protected_names, object_count)) return false;
    for (::std::uint32_t index = 0; index < object_count; ++index) {
        IrObject* appended = in.append(graph.objects);
        if (appended == nullptr) return false;
        IrObject& object = *appended;
        object.kind = static_cast<Kind>(in.u8());
        switch (object.kind) {
        case kind_string:
            object.text = in.text();
            break;
        case kind_table:
            object.first = in.u32();
            if (!read_values(in, object.values, static_cast<::std::size_t>(in.count(2, max_table_pairs)) * 2)) return false;
            break;
        case kind_handle_table:
            if (!read_id_list(in, object.references)) return false;
            break;
        case kind_lua_function:
            object.first = in.u32();
            object.second = in.u32();
            if (!read_ids(in, object.references, in.u8())) return false;
            break;
        case kind_c_function:
            object.text = in.text(256);
            if (!read_values(in, object.values, in.u8())) return false;
            break;
        case kind_userdata:
            object.first = in.u32();
            break;
        case kind_handle:
            object.handle_kind = in.u32();
            object.handle_id = in.u64();
            break;
        case kind_thread: {
            object.flag = in.u8() != 0;
            object.first = in.u32();
            object.hookmask = in.u8();
            object.allowhook = in.u8();
            object.hookinit = in.u8();
            object.basehookcount = in.i32();
            object.hookcount = in.i32();
            if (!read_values(in, object.values, in.count(1, max_stack_slots))) return false;
            const ::std::uint32_t frames = in.count(9, static_cast<::std::uint32_t>(LUA_MAXCALLS - 1));
            if (frames == 0) return false;
            for (::std::uint32_t frame_index = 0; frame_index < frames; ++frame_index) {
                IrFrame* frame = in.append(object.frames);
                if (frame == nullptr) return false;
                frame->base = in.u32();
                frame->top = in.u32();
                frame->state = in.u8();
                if ((frame->state & CI_C) == 0) {
                    frame->savedpc = in.u32();
                    frame->tailcalls = in.i32();
                }
            }
            break;
        }
        case kind_prototype:
            object.text = in.text(4096);
            if (!read_id_list(in, object.references, max_prototype_depth)) return false;
            break;
        case kind_upvalue:
            object.flag = in.u8() != 0;
            if (object.flag) {
                object.first = in.u32();
                object.second = in.u32();
            } else if (!read_values(in, object.values, 1)) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return in.ok();
}

class GraphValidator {
public:
    GraphValidator(IrGraph& graph, const RestoreContext& context) noexcept : graph_(graph), context_(context) {}

    bool run(::std::string& error) {
        check_specials();
        for (::std::uint32_t id = 1; ok() && id <= graph_.objects.size(); ++id) check_object(id);
        if (ok()) check_identities();
        if (ok()) check_handles();
        for (const ::std::uint32_t id : graph_.frozen) require(is(id, kind_table), "frozen entry is not a table");
        for (const ::std::uint32_t id : graph_.protected_names) require(is(id, kind_string), "protected name is not a string");
        if (!ok()) error = error_;
        return ok();
    }

private:
    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }

    void require(bool condition, ::std::string_view message) {
        if (!condition && error_.empty()) error_ = ::std::string(message);
    }

    [[nodiscard]] const IrObject* object(::std::uint32_t id) const noexcept {
        return id >= 1 && id <= graph_.objects.size() ? &graph_.objects[id - 1] : nullptr;
    }

    [[nodiscard]] bool is(::std::uint32_t id, Kind kind) const noexcept {
        const IrObject* found = object(id);
        return found != nullptr && found->kind == kind;
    }

    [[nodiscard]] bool is_table(::std::uint32_t id) const noexcept { return is(id, kind_table) || is(id, kind_handle_table); }

    [[nodiscard]] bool is_value(const IrValue& value) const noexcept {
        if (value.tag != tag_reference) return true;
        const IrObject* found = object(value.reference);
        return found != nullptr && found->kind != kind_prototype && found->kind != kind_upvalue;
    }

    [[nodiscard]] bool reference_to(const IrValue& value, Kind kind) const noexcept {
        return value.tag == tag_reference && is(value.reference, kind);
    }

    [[nodiscard]] bool anchorable(const IrValue& value) const noexcept {
        if (value.tag != tag_reference) return false;
        const IrObject* found = object(value.reference);
        return found != nullptr && found->kind != kind_string && found->kind != kind_handle &&
            found->kind != kind_prototype && found->kind != kind_upvalue;
    }

    [[nodiscard]] bool string_is(const IrValue& value, ::std::string_view text) const noexcept {
        return reference_to(value, kind_string) && object(value.reference)->text == text;
    }

    // The value of a key in a decoded table, or nullptr.
    [[nodiscard]] const IrValue* lookup(::std::uint32_t table, const IrValue& key) const {
        const IrObject& found = graph_.objects[table - 1];
        for (::std::size_t index = 0; index < found.values.size(); index += 2) {
            const IrValue& candidate = found.values[index];
            if (candidate.tag != key.tag) continue;
            if (key.tag == tag_number && candidate.bits == key.bits) return &found.values[index + 1];
            if (key.tag == tag_reference && candidate.reference == key.reference) return &found.values[index + 1];
            if (key.tag == tag_reference && is(key.reference, kind_string) && is(candidate.reference, kind_string) &&
                object(key.reference)->text == object(candidate.reference)->text) {
                return &found.values[index + 1];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const IrValue* registry_field(::std::string_view name) const {
        const IrObject& registry = graph_.objects[graph_.registry - 1];
        for (::std::size_t index = 0; index < registry.values.size(); index += 2) {
            if (string_is(registry.values[index], name)) return &registry.values[index + 1];
        }
        return nullptr;
    }

    void check_specials() {
        require(is(graph_.registry, kind_table), "registry is not a table");
        require(is(graph_.main, kind_thread) && object(graph_.main)->flag, "main thread missing");
        require(is(graph_.handle_metatable, kind_table), "handle metatable is not a table");
        require(is(graph_.anchors, kind_table), "identity anchors are not a table");
        require(is(graph_.handles, kind_handle_table), "handle index missing");
        require(is(graph_.sizes, kind_table), "size table missing");
        if (!ok()) return;
        const auto field_is = [&](::std::string_view name, ::std::uint32_t id) {
            const IrValue* value = registry_field(name);
            return value != nullptr && value->tag == tag_reference && value->reference == id;
        };
        require(field_is("eawr.identity_anchors", graph_.anchors), "registry does not hold the identity anchors");
        require(field_is("eawr.handles", graph_.handles), "registry does not hold the handle index");
        require(field_is("eawr.handle_metatable", graph_.handle_metatable), "registry does not hold the handle metatable");
        IrValue sizes_key;
        sizes_key.tag = tag_number;
        sizes_key.bits = lua_Number(lauxlib_sizes_reference).repr;
        const IrValue* sizes = lookup(graph_.registry, sizes_key);
        require(sizes != nullptr && sizes->tag == tag_reference && sizes->reference == graph_.sizes,
                "registry does not hold the size table");
    }

    void check_table(::std::uint32_t id, const IrObject& table) {
        require(table.first == 0 || is(table.first, kind_table), "metatable is not a table");
        require(table.values.size() / 2 <= max_table_pairs, "table too large");
        const bool weak = id == graph_.sizes;
        for (::std::size_t index = 0; ok() && index < table.values.size(); index += 2) {
            const IrValue& key = table.values[index];
            const IrValue& value = table.values[index + 1];
            require(key.tag != tag_nil && value.tag != tag_nil, "table holds a nil key or value");
            require(is_value(key) && is_value(value), "table refers to a missing object");
            require(key.tag != tag_number || !is_nan(lua_Number::from_repr(key.bits)), "table key is NaN");
            if (!ok()) return;
            if (string_is(key, "__gc") || (string_is(key, "__mode") && !weak)) {
                require(false, "weak table or finalizer key");
                return;
            }
            if (weak && key.tag == tag_reference && !is(key.reference, kind_string)) {
                require(value.tag != tag_reference, "size table entry is not a count");
            }
            // Reference keys of script tables have identities (canonical order).
            if (key.tag == tag_reference && !weak && id != graph_.anchors && !is(key.reference, kind_string) &&
                !is(key.reference, kind_handle)) {
                require(anchored_.count(key.reference) != 0, "table key without identity");
            }
        }
    }

    void check_c_function(IrObject& function) {
        function.function = context_.functions->find(function.text);
        if (function.function == nullptr) {
            require(false, "unknown C function: " + function.text);
            return;
        }
        const auto& up = function.values;
        for (const IrValue& value : up) require(is_value(value), "C function upvalue refers to a missing object");
        if (!ok()) return;
        int position = 0;
        int count = 0;
        switch (function.function->shape) {
        case UpvalueShape::none:
            require(up.empty(), "C function has unexpected upvalues");
            return;
        case UpvalueShape::name:
            require(up.size() == 1 && reference_to(up[0], kind_string), "C function name upvalue");
            return;
        case UpvalueShape::function:
            require(up.size() == 1 && (reference_to(up[0], kind_c_function) || reference_to(up[0], kind_lua_function)),
                    "C function upvalue is not a function");
            return;
        case UpvalueShape::pairs_cursor:
            require(up.size() == 4 && reference_to(up[0], kind_table) && reference_to(up[1], kind_table) &&
                        up[2].tag == tag_number && up[3].tag == tag_number,
                    "pairs cursor shape");
            if (!ok()) return;
            require(whole_number(up[3].bits, 0, 2147483647, count) && whole_number(up[2].bits, 0, count, position) &&
                        static_cast<::std::size_t>(count) <= object(up[1].reference)->values.size() / 2,
                    "pairs cursor out of range");
            return;
        case UpvalueShape::gfind_cursor:
            require(up.size() == 3 && reference_to(up[0], kind_string) && reference_to(up[1], kind_string) &&
                        up[2].tag == tag_number,
                    "gfind cursor shape");
            if (!ok()) return;
            require(object(up[0].reference)->text.size() < 2147483647u &&
                        whole_number(up[2].bits, 0, static_cast<int>(object(up[0].reference)->text.size()) + 1, position),
                    "gfind cursor out of range");
            return;
        case UpvalueShape::coroutine:
            require(up.size() == 1 && reference_to(up[0], kind_thread), "coroutine.wrap upvalue is not a thread");
            return;
        case UpvalueShape::binding_index:
            require(up.size() == 1 && up[0].tag == tag_number && context_.binding_count != 0 &&
                        whole_number(up[0].bits, 0, static_cast<int>(context_.binding_count) - 1, position),
                    "binding index out of range");
            return;
        case UpvalueShape::bound_method:
            require(up.size() == 2 && up[0].tag == tag_number && context_.binding_count != 0 &&
                        whole_number(up[0].bits, 0, static_cast<int>(context_.binding_count) - 1, position) &&
                        reference_to(up[1], kind_handle),
                    "bound method shape");
            return;
        }
    }

    void check_thread(const IrObject& thread) {
        require(is(thread.first, kind_table), "thread globals are not a table");
        for (const IrValue& value : thread.values) require(is_value(value), "stack refers to a missing object");
        // Every thread runs under the instruction budget hook.
        require(thread.hookmask == sandbox_hook_mask && thread.basehookcount == context_.limits.hook_granularity &&
                    thread.hookcount >= 1 && thread.hookcount <= thread.basehookcount && thread.allowhook == 1 &&
                    thread.hookinit == 0,
                "thread hook");
        const ::std::vector<IrFrame>& frames = thread.frames;
        require(frames.size() != 2 && frames[0].state == CI_C && frames[0].base == 1, "thread base frame");
        require(!thread.values.empty() && thread.values[0].tag == tag_nil, "thread base slot");
        ::std::uint64_t highest = thread.values.size();
        for (const IrFrame& frame : frames) {
            require(frame.top >= frame.base && frame.top - frame.base <= max_frame_slots && frame.top < max_stack_slots,
                    "frame bounds");
            highest = ::std::max<::std::uint64_t>(highest, frame.top);
        }
        stack_slots_ += highest;
        require(stack_slots_ <= max_total_stack_slots, "thread stacks too large");
        if (thread.flag) require(frames.size() == 1 && thread.values.size() == 1, "main thread is not idle");
        if (!ok() || frames.size() == 1) return;
        // Frames 1..n-2 are Lua frames; frame n-1 is coroutine.yield. The
        // detailed checks need prototypes (check_frames after compilation).
        const IrFrame& last = frames.back();
        require(last.state == yield_frame && last.base >= 2 && last.base <= thread.values.size() &&
                    thread.values.size() <= last.top,
                "suspended thread frame");
        if (!ok()) return;
        for (::std::size_t index = 1; index + 1 < frames.size(); ++index) {
            const int expected = index + 2 == frames.size() ? lua_frame : calling_frame;
            require(frames[index].state == expected && frames[index].base >= 2 && frames[index].base <= thread.values.size() &&
                        frames[index].base > frames[index - 1].base,
                    "suspended Lua frame");
            if (!ok()) return;
            require(reference_to(thread.values[frames[index].base - 1], kind_lua_function), "Lua frame function");
        }
        if (!ok()) return;
        const IrValue& yielder = thread.values[last.base - 1];
        require(reference_to(yielder, kind_c_function) && object(yielder.reference)->text == yield_function,
                "suspended thread is not in coroutine.yield");
    }

    void check_object(::std::uint32_t id) {
        IrObject& current = graph_.objects[id - 1];
        switch (current.kind) {
        case kind_string:
        case kind_handle:
            return;
        case kind_table:
            check_table(id, current);
            return;
        case kind_handle_table:
            for (const ::std::uint32_t handle : current.references) require(is(handle, kind_handle), "handle index entry");
            return;
        case kind_lua_function:
            require(is(current.first, kind_prototype) && is(current.second, kind_table), "Lua function prototype or environment");
            for (const ::std::uint32_t upvalue : current.references) require(is(upvalue, kind_upvalue), "Lua function upvalue");
            return;
        case kind_c_function:
            check_c_function(current);
            return;
        case kind_userdata:
            require(current.first == 0 || (is(current.first, kind_table) && current.first != graph_.handle_metatable),
                    "userdata metatable");
            return;
        case kind_thread:
            check_thread(current);
            require(current.flag == (id == graph_.main), "only the main thread is marked main");
            return;
        case kind_prototype:
            require(current.text.size() > 1 && current.text.front() == '@', "prototype chunk name");
            return;
        case kind_upvalue:
            if (current.flag) {
                require(is(current.first, kind_thread) && current.second >= 1 &&
                            current.second < object(current.first)->values.size(),
                        "open upvalue slot");
                require(open_slots_.emplace(current.first, current.second).second, "two open upvalues share a slot");
            } else {
                require(is_value(current.values[0]), "upvalue refers to a missing object");
            }
            return;
        }
    }

    void check_identities() {
        const IrObject& anchors = graph_.objects[graph_.anchors - 1];
        const ::std::uint64_t count = anchors.values.size() / 2;
        require(anchors.first == 0, "identity anchors have a metatable");
        require(graph_.next_identity == count + 1 && count <= context_.limits.identities, "identity count or quota");
        require(graph_.memory_measured <= context_.limits.memory_bytes &&
                    graph_.memory_charged <= context_.limits.memory_bytes - graph_.memory_measured,
                "memory quota");
        if (!ok()) return;
        graph_.identities.assign(static_cast<::std::size_t>(count), 0);
        for (::std::size_t index = 0; index < anchors.values.size(); index += 2) {
            int identity = 0;
            const IrValue& key = anchors.values[index];
            require(key.tag == tag_number && whole_number(key.bits, 1, static_cast<int>(count), identity) &&
                        graph_.identities[static_cast<::std::size_t>(identity - 1)] == 0 &&
                        anchorable(anchors.values[index + 1]),
                    "identity anchor");
            if (!ok()) return;
            graph_.identities[static_cast<::std::size_t>(identity - 1)] = anchors.values[index + 1].reference;
        }
        ::std::vector<::std::uint32_t> sorted = graph_.identities;
        ::std::sort(sorted.begin(), sorted.end());
        require(::std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "an object has two identities");
    }

    void check_handles() {
        const IrObject& index = graph_.objects[graph_.handles - 1];
        ::std::set<::std::pair<::std::uint32_t, ::std::uint64_t>> seen;
        ::std::size_t handles = 0;
        for (const IrObject& current : graph_.objects) handles += current.kind == kind_handle ? 1 : 0;
        for (const ::std::uint32_t id : index.references) {
            const IrObject& handle = graph_.objects[id - 1];
            require(seen.emplace(handle.handle_kind, handle.handle_id).second, "handle index entry");
        }
        require(seen.size() == handles, "a handle is missing from the handle index");
    }

public:
    // Anchored objects, filled before the object checks.
    void collect_anchored() {
        if (!is(graph_.anchors, kind_table)) return;
        const IrObject& anchors = graph_.objects[graph_.anchors - 1];
        for (::std::size_t index = 1; index < anchors.values.size(); index += 2) {
            if (anchors.values[index].tag == tag_reference) anchored_.insert(anchors.values[index].reference);
        }
    }

private:
    IrGraph& graph_;
    const RestoreContext& context_;
    ::std::string error_;
    ::std::set<::std::uint32_t> anchored_;
    ::std::set<::std::pair<::std::uint32_t, ::std::uint32_t>> open_slots_;
    ::std::uint64_t stack_slots_{};
};

// Frame checks that need the compiled prototypes.
bool check_frames(const IrGraph& graph, const IrObject& thread, ::std::string& error) {
    const ::std::vector<IrFrame>& frames = thread.frames;
    for (::std::size_t index = 1; index + 1 < frames.size(); ++index) {
        const IrFrame& frame = frames[index];
        const IrObject& function = graph.objects[thread.values[frame.base - 1].reference - 1];
        const Proto* prototype = graph.objects[function.first - 1].prototype;
        if (frame.savedpc < 1 || frame.savedpc > static_cast<::std::uint32_t>(prototype->sizecode) || frame.tailcalls < 0 ||
            frame.top < frame.base + prototype->maxstacksize) {
            error = "suspended Lua frame does not match its prototype";
            return false;
        }
        const Instruction call = prototype->code[frame.savedpc - 1];
        const OpCode opcode = GET_OPCODE(call);
        const bool last = index + 2 == frames.size();
        const ::std::uint32_t callee = frame.base + static_cast<::std::uint32_t>(GETARG_A(call));
        if (!(opcode == OP_CALL || (last && opcode == OP_TAILCALL)) || callee + 1 != frames[index + 1].base ||
            GETARG_A(call) >= prototype->maxstacksize) {
            error = "suspended Lua frame is not at a call";
            return false;
        }
    }
    return true;
}

[[noreturn]] int restore_panic(lua_State*) { throw ::std::bad_alloc(); }

class GraphBuilder {
public:
    GraphBuilder(IrGraph& graph, Sandbox::Impl& impl) noexcept : graph_(graph), impl_(impl), state_(impl.state) {}

    void build() {
        allocate();
        fill();
        sethvalue(registry(state_), table(graph_.registry));
        impl_.globals = hvalue(gt(state_));
        impl_.anchors = table(graph_.anchors);
        impl_.handles = table(graph_.handles);
        impl_.handle_metatable = table(graph_.handle_metatable);
        impl_.sizes = table(graph_.sizes);
        impl_.next_identity = graph_.next_identity;
        impl_.memory_measured = graph_.memory_measured;
        impl_.memory_charged = graph_.memory_charged;
        for (::std::size_t index = 0; index < graph_.identities.size(); ++index) {
            impl_.identities.emplace(graph_.objects[graph_.identities[index] - 1].built, index + 1);
        }
        for (const ::std::uint32_t id : graph_.frozen) impl_.frozen.push_back(table(id));
        for (const ::std::uint32_t id : graph_.protected_names) impl_.protected_names.insert(graph_.objects[id - 1].built);
    }

private:
    Table* table(::std::uint32_t id) const { return static_cast<Table*>(graph_.objects[id - 1].built); }

    void set(TObject* target, const IrValue& value) const {
        switch (value.tag) {
        case tag_nil:
            setnilvalue(target);
            return;
        case tag_false:
        case tag_true:
            setbvalue(target, value.tag == tag_true ? 1 : 0);
            return;
        case tag_number:
            setnvalue(target, lua_Number::from_repr(value.bits));
            return;
        default:
            break;
        }
        const IrObject& object = graph_.objects[value.reference - 1];
        switch (object.kind) {
        case kind_string:
            setsvalue(target, static_cast<TString*>(object.built));
            return;
        case kind_table:
        case kind_handle_table:
            sethvalue(target, static_cast<Table*>(object.built));
            return;
        case kind_lua_function:
        case kind_c_function:
            setclvalue(target, static_cast<Closure*>(object.built));
            return;
        case kind_userdata:
        case kind_handle:
            setuvalue(target, static_cast<Udata*>(object.built));
            return;
        default:
            setthvalue(target, static_cast<lua_State*>(object.built));
            return;
        }
    }

    void allocate_thread(IrObject& object) {
        lua_State* thread = object.flag ? state_ : luaE_newthread(state_);
        object.built = thread;
        ::std::uint32_t highest = static_cast<::std::uint32_t>(object.values.size());
        for (const IrFrame& frame : object.frames) highest = ::std::max(highest, frame.top);
        luaD_reallocstack(thread, static_cast<int>(::std::max<::std::uint32_t>(BASIC_STACK_SIZE, highest + LUA_MINSTACK) + EXTRA_STACK));
        for (TObject* slot = thread->stack; slot < thread->stack + thread->stacksize; ++slot) setnilvalue(slot);
        int frames = BASIC_CI_SIZE;
        while (static_cast<::std::size_t>(frames) <= object.frames.size()) frames *= 2;
        luaD_reallocCI(thread, frames);
    }

    void allocate() {
        // Threads before upvalues: an open upvalue points into its thread's final stack.
        for (IrObject& object : graph_.objects) {
            switch (object.kind) {
            case kind_string:
                object.built = luaS_newlstr(state_, object.text.data(), object.text.size());
                break;
            case kind_table:
            case kind_handle_table:
                object.built = luaH_new(state_, 0, 0);
                break;
            case kind_lua_function:
                object.built = luaF_newLclosure(state_, static_cast<int>(object.references.size()), gt(state_));
                break;
            case kind_c_function: {
                Closure* closure = luaF_newCclosure(state_, static_cast<int>(object.values.size()));
                closure->c.f = object.function->function;
                for (int index = 0; index < closure->c.nupvalues; ++index) setnilvalue(&closure->c.upvalue[index]);
                object.built = closure;
                break;
            }
            case kind_userdata:
                object.built = luaS_newudata(state_, 0);
                break;
            case kind_handle: {
                Udata* userdata = luaS_newudata(state_, sizeof(HandlePayload));
                *reinterpret_cast<HandlePayload*>(userdata + 1) = HandlePayload{object.handle_kind, 0, object.handle_id};
                object.built = userdata;
                break;
            }
            case kind_thread:
                allocate_thread(object);
                break;
            case kind_prototype:
                object.built = const_cast<Proto*>(object.prototype);
                break;
            case kind_upvalue:
                break;
            }
        }
        for (IrObject& object : graph_.objects) {
            if (object.kind != kind_upvalue) continue;
            UpVal* upvalue = luaM_new(state_, UpVal);
            upvalue->tt = LUA_TUPVAL;
            setnilvalue(&upvalue->value);
            object.built = upvalue;
            if (!object.flag) {
                upvalue->v = &upvalue->value;
                luaC_link(state_, valtogco(upvalue), LUA_TUPVAL);
                continue;
            }
            // Open: chained into its thread's list, highest slot first.
            auto* thread = static_cast<lua_State*>(graph_.objects[object.first - 1].built);
            upvalue->marked = 1;
            upvalue->v = thread->stack + object.second;
            GCObject** link = &thread->openupval;
            while (*link != nullptr && gcotouv(*link)->v > upvalue->v) link = &(*link)->gch.next;
            upvalue->next = *link;
            *link = valtogco(upvalue);
        }
    }

    void fill_thread(const IrObject& object) {
        auto* thread = static_cast<lua_State*>(object.built);
        sethvalue(gt(thread), table(object.first));
        for (::std::size_t index = 0; index < object.values.size(); ++index) set(thread->stack + index, object.values[index]);
        for (::std::size_t index = 0; index < object.frames.size(); ++index) {
            const IrFrame& frame = object.frames[index];
            CallInfo* call = thread->base_ci + index;
            call->base = thread->stack + frame.base;
            call->top = thread->stack + frame.top;
            call->state = frame.state;
            if ((frame.state & CI_C) == 0) {
                const Proto* prototype = clvalue(call->base - 1)->l.p;
                call->u.l.savedpc = prototype->code + frame.savedpc;
                call->u.l.pc = nullptr;
                call->u.l.tailcalls = frame.tailcalls;
            }
        }
        thread->ci = thread->base_ci + (object.frames.size() - 1);
        thread->base = thread->ci->base;
        thread->top = thread->stack + object.values.size();
        thread->hook = sandbox_count_hook();
        thread->hookmask = object.hookmask;
        thread->basehookcount = object.basehookcount;
        thread->hookcount = object.hookcount;
        thread->allowhook = object.allowhook;
        thread->hookinit = object.hookinit;
    }

    void fill() {
        // Closures first: frames read their functions' prototypes.
        for (IrObject& object : graph_.objects) {
            if (object.kind == kind_lua_function) {
                Closure* closure = static_cast<Closure*>(object.built);
                closure->l.p = const_cast<Proto*>(graph_.objects[object.first - 1].prototype);
                sethvalue(&closure->l.g, table(object.second));
                for (::std::size_t index = 0; index < object.references.size(); ++index) {
                    closure->l.upvals[index] = static_cast<UpVal*>(graph_.objects[object.references[index] - 1].built);
                }
            } else if (object.kind == kind_c_function) {
                Closure* closure = static_cast<Closure*>(object.built);
                for (::std::size_t index = 0; index < object.values.size(); ++index) set(&closure->c.upvalue[index], object.values[index]);
            }
        }
        for (IrObject& object : graph_.objects) {
            switch (object.kind) {
            case kind_table: {
                Table* target = static_cast<Table*>(object.built);
                if (object.first != 0) target->metatable = table(object.first);
                for (::std::size_t index = 0; index < object.values.size(); index += 2) {
                    TObject key;
                    set(&key, object.values[index]);
                    if (!ttisnil(luaH_get(target, &key))) throw ::std::runtime_error("duplicate table key");
                    TObject value;
                    set(&value, object.values[index + 1]);
                    setobj2t(luaH_set_upstream(state_, target, &key), &value);
                }
                break;
            }
            case kind_handle_table: {
                Table* target = static_cast<Table*>(object.built);
                for (const ::std::uint32_t id : object.references) {
                    Udata* handle = static_cast<Udata*>(graph_.objects[id - 1].built);
                    TObject key;
                    setsvalue(&key, luaS_newlstr(state_, reinterpret_cast<const char*>(handle + 1), sizeof(HandlePayload)));
                    if (!ttisnil(luaH_get(target, &key))) throw ::std::runtime_error("duplicate handle");
                    setuvalue(luaH_set_upstream(state_, target, &key), handle);
                }
                break;
            }
            case kind_userdata:
                if (object.first != 0) static_cast<Udata*>(object.built)->uv.metatable = table(object.first);
                break;
            case kind_handle:
                static_cast<Udata*>(object.built)->uv.metatable = table(graph_.handle_metatable);
                break;
            case kind_thread:
                fill_thread(object);
                break;
            case kind_upvalue:
                if (!object.flag) set(&static_cast<UpVal*>(object.built)->value, object.values[0]);
                break;
            default:
                break;
            }
        }
    }

    IrGraph& graph_;
    Sandbox::Impl& impl_;
    lua_State* state_;
};

} // namespace

// ---- CFunctionTable ----

bool CFunctionTable::create(::std::vector<CFunctionEntry> host, CFunctionTable& out, ::std::string& error) {
    ::std::vector<CFunctionEntry> entries;
    append_library(entries, "base.", sflua_base_functions());
    append_library(entries, "coroutine.", sflua_coroutine_functions());
    entries.push_back({"coroutine.wrapper", sflua_coroutine_wrapper(), UpvalueShape::coroutine});
    append_library(entries, "string.", sflua_string_functions());
    entries.push_back({"string.gfind_iterator", sflua_gfind_iterator(), UpvalueShape::gfind_cursor});
    entries.push_back({"sandbox.find", sflua_metered_find(), UpvalueShape::none});
    entries.push_back({"sandbox.gfind", sflua_metered_gfind(), UpvalueShape::none});
    entries.push_back({"sandbox.gfind_iterator", sflua_metered_gfind_iterator(), UpvalueShape::gfind_cursor});
    entries.push_back({"sandbox.gsub", sflua_metered_gsub(), UpvalueShape::none});
    append_library(entries, "table.", sflua_table_functions());
    append_sandbox_functions(entries);
    for (CFunctionEntry& entry : host) entries.push_back(::std::move(entry));
    ::std::sort(entries.begin(), entries.end(), [](const CFunctionEntry& left, const CFunctionEntry& right) {
        return left.name < right.name;
    });
    ::std::map<lua_CFunction, const ::std::string*> functions;
    for (::std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0 && entries[index].name == entries[index - 1].name) {
            error = "C function name registered twice: " + entries[index].name;
            return false;
        }
        const auto [found, inserted] = functions.emplace(entries[index].function, &entries[index].name);
        if (!inserted) {
            error = "C functions " + *found->second + " and " + entries[index].name + " share one address";
            return false;
        }
    }
    out.entries_ = ::std::move(entries);
    return true;
}

const CFunctionEntry* CFunctionTable::find(lua_CFunction function) const noexcept {
    for (const CFunctionEntry& entry : entries_) {
        if (entry.function == function) return &entry;
    }
    return nullptr;
}

const CFunctionEntry* CFunctionTable::find(::std::string_view name) const noexcept {
    const auto found = ::std::lower_bound(entries_.begin(), entries_.end(), name, [](const CFunctionEntry& entry, ::std::string_view key) {
        return entry.name < key;
    });
    return found != entries_.end() && found->name == name ? &*found : nullptr;
}

::std::uint64_t measure_logical_bytes(const Sandbox::Impl& sandbox) { return LogicalSizer(sandbox).run(); }

// ---- Save and restore ----

bool save_graph(Sandbox& sandbox, const CFunctionTable& functions, persist::ByteWriter& out, ::std::string& error) {
    Sandbox::Impl& impl = PersistAccess::impl(sandbox);
    if (impl.fault != SandboxFault::none || !impl.sort_scratch.empty() || impl.identity_suspended != 0 ||
        !impl.deep_stacks.empty() || impl.host_calls != 0 || impl.memory_suspended != 0) {
        error = "the state is faulted or inside a call";
        return false;
    }
    GraphSaver saver(impl, functions);
    return saver.run(out, error);
}

::std::unique_ptr<Sandbox> restore_graph(persist::ByteReader& in, const RestoreContext& context, ::std::string& error) {
    IrGraph graph;
    if (!read_graph(in, graph)) {
        error = "truncated or malformed Lua graph";
        return nullptr;
    }
    GraphValidator validator(graph, context);
    validator.collect_anchored();
    if (!validator.run(error)) return nullptr;

    lua_State* state = lua_open();
    if (state == nullptr) {
        error = "cannot allocate a Lua state";
        return nullptr;
    }
    auto impl = ::std::make_unique<Sandbox::Impl>();
    impl->state = state;
    impl->limits = context.limits;
    Sandbox::Impl& restored = *impl;
    ::std::unique_ptr<Sandbox> sandbox = PersistAccess::adopt(::std::move(impl));
    lua_atpanic(state, restore_panic);
    // No collection until the graph is whole; the build only allocates.
    G(state)->GCthreshold = MAX_LUMEM;
    try {
        // Prototypes: compile each referenced chunk once (no chunk runs).
        ::std::map<::std::string, const Proto*, ::std::less<>> chunks;
        for (IrObject& object : graph.objects) {
            if (object.kind != kind_prototype) continue;
            auto found = chunks.find(object.text);
            if (found == chunks.end()) {
                const ::std::string* bytes = context.module_source(object.text);
                if (bytes == nullptr) {
                    error = "module is not in the session manifest: " + object.text.substr(1);
                    return nullptr;
                }
                if (sandbox->load_source(*bytes, object.text) != 0) {
                    error = "module does not compile: " + object.text.substr(1);
                    return nullptr;
                }
                found = chunks.emplace(object.text, clvalue(state->top - 1)->l.p).first;
                lua_pop(state, 1);
            }
            const Proto* prototype = found->second;
            for (const ::std::uint32_t index : object.references) {
                if (index >= static_cast<::std::uint32_t>(prototype->sizep)) {
                    error = "prototype path does not exist in " + object.text.substr(1);
                    return nullptr;
                }
                prototype = prototype->p[index];
            }
            object.prototype = prototype;
        }
        for (const IrObject& object : graph.objects) {
            if (object.kind == kind_lua_function &&
                object.references.size() != graph.objects[object.first - 1].prototype->nups) {
                error = "function upvalue count does not match its prototype";
                return nullptr;
            }
            if (object.kind == kind_thread && !check_frames(graph, object, error)) return nullptr;
        }
        GraphBuilder builder(graph, restored);
        builder.build();
        luaC_collectgarbage(state);
    } catch (const ::std::exception& failure) {
        error = ::std::string("cannot build the Lua graph: ") + failure.what();
        return nullptr;
    }
    return sandbox;
}

bool registry_holds(Sandbox& sandbox, int reference, ReferenceKind kind) {
    if (reference <= 0) return false;
    lua_State* state = PersistAccess::impl(sandbox).state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    const int type = lua_type(state, -1);
    lua_pop(state, 1);
    switch (kind) {
    case ReferenceKind::thread:
        return type == LUA_TTHREAD;
    case ReferenceKind::function:
        return type == LUA_TFUNCTION;
    case ReferenceKind::table:
        return type == LUA_TTABLE;
    default:
        return type != LUA_TNIL;
    }
}

lua_State* registry_thread(Sandbox& sandbox, int reference) {
    lua_State* state = PersistAccess::impl(sandbox).state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    lua_State* thread = lua_tothread(state, -1);
    lua_pop(state, 1);
    return thread;
}

bool registry_references_consistent(Sandbox& sandbox, const ::std::vector<int>& held) {
    // luaL_ref: the free list starts at registry[1] and chains through the
    // free entries; without one, the next reference is max(getn, 2) + 1.
    constexpr int free_list = 1;
    constexpr int reserved = 2;
    lua_State* state = PersistAccess::impl(sandbox).state;
    const int limit = ::std::max(luaL_getn(state, LUA_REGISTRYINDEX), reserved);
    ::std::set<int> free_references;
    lua_rawgeti(state, LUA_REGISTRYINDEX, free_list);
    int reference = static_cast<int>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    while (reference != 0) {
        if (reference <= reserved || reference > limit || !free_references.insert(reference).second) return false;
        // As luaL_ref reads it: the last free entry holds nil (0).
        lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
        reference = static_cast<int>(lua_tonumber(state, -1));
        lua_pop(state, 1);
    }
    ::std::set<int> seen;
    for (const int value : held) {
        if (value <= reserved || value > limit || free_references.contains(value) || !seen.insert(value).second) return false;
    }
    return true;
}

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#else

#include "sflua_sandbox_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

// The benchmark twin has no persistence and measures nothing.
::std::uint64_t measure_logical_bytes(const Sandbox::Impl&) { return 0; }

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#endif // !EAWR_SFLUA_HARDWARE_TWIN
