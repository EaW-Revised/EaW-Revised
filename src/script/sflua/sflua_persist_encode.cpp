#if !defined(EAWR_SFLUA_HARDWARE_TWIN)

#include "sflua_persist_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {
namespace persist_internal {

GraphSaver::GraphSaver(Sandbox::Impl& sandbox, const CFunctionTable& functions) noexcept : sandbox_(sandbox), functions_(functions) {}

bool GraphSaver::run(persist::ByteWriter& out, ::std::string& error) {
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

void GraphSaver::write_value(persist::ByteWriter& out, const TObject* value) {
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
void GraphSaver::write_id_list(persist::ByteWriter& out, const ::std::vector<Pointer>& objects) {
        ::std::vector<::std::uint32_t> ids;
        for (const Pointer object : objects) {
            if (const ::std::uint32_t id = id_of(object); id != 0) ids.push_back(id);
        }
        ::std::sort(ids.begin(), ids.end());
        ids.erase(::std::unique(ids.begin(), ids.end()), ids.end());
        out.u32(static_cast<::std::uint32_t>(ids.size()));
        for (const ::std::uint32_t id : ids) out.u32(id);
    }

void GraphSaver::write_object(persist::ByteWriter& out, ::std::uint32_t id) {
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

} // namespace persist_internal

using namespace persist_internal;

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

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#endif // !EAWR_SFLUA_HARDWARE_TWIN
