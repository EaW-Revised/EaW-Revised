#if !defined(EAWR_SFLUA_HARDWARE_TWIN)

#include "sflua_persist_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {
namespace persist_internal {

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

} // namespace persist_internal

using namespace persist_internal;

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

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#endif // !EAWR_SFLUA_HARDWARE_TWIN
