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

#include "sflua_persist_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {
Sandbox::Impl& PersistAccess::impl(Sandbox& sandbox) noexcept { return *sandbox.impl_; }
::std::unique_ptr<Sandbox> PersistAccess::adopt(::std::unique_ptr<Sandbox::Impl> impl) {
        return ::std::unique_ptr<Sandbox>(new Sandbox(::std::move(impl)));
    }

namespace {

void append_library(::std::vector<CFunctionEntry>& out, ::std::string_view prefix, const luaL_reg* functions) {
    for (const luaL_reg* entry = functions; entry->name != nullptr; ++entry) {
        out.push_back({::std::string(prefix) + entry->name, entry->func, UpvalueShape::none});
    }
}

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
