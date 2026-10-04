#if !defined(EAWR_SFLUA_HARDWARE_TWIN)

#include "sflua_persist_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {
namespace persist_internal {

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

::std::uint32_t GraphSaver::id_of(const void* object) const {
        const auto found = ids_.find(object);
        return found == ids_.end() ? 0 : found->second;
    }

::std::uint32_t GraphSaver::add(const void* object, Kind kind) {
        const auto [found, inserted] = ids_.emplace(object, static_cast<::std::uint32_t>(objects_.size() + 1));
        if (inserted) objects_.push_back(Entry{kind, object});
        return found->second;
    }

::std::uint32_t GraphSaver::reference_of(const TObject* value) {
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

void GraphSaver::fail(::std::string message) {
        if (error_.empty()) error_ = ::std::move(message);
    }

bool GraphSaver::ordered_keys(const Table* table, ::std::vector<TObject>& keys) {
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

::std::vector<const TObject*> GraphSaver::handle_values(const Table* table) {
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

bool GraphSaver::check_thread(const lua_State* thread) {
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

void GraphSaver::expand(::std::uint32_t index) {
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

void GraphSaver::resolve_open_upvalues() {
        for (const Entry& entry : objects_) {
            if (entry.kind != kind_thread) continue;
            const auto* thread = static_cast<const lua_State*>(entry.object);
            for (const GCObject* link = thread->openupval; link != nullptr; link = link->gch.next) {
                const UpVal* upvalue = gcotouv(const_cast<GCObject*>(link));
                open_[upvalue] = {id_of(thread), static_cast<::std::uint32_t>(upvalue->v - thread->stack)};
            }
        }
    }

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

GraphValidator::GraphValidator(IrGraph& graph, const RestoreContext& context) noexcept : graph_(graph), context_(context) {}

bool GraphValidator::run(::std::string& error) {
        check_specials();
        for (::std::uint32_t id = 1; ok() && id <= graph_.objects.size(); ++id) check_object(id);
        if (ok()) check_identities();
        if (ok()) check_handles();
        for (const ::std::uint32_t id : graph_.frozen) require(is(id, kind_table), "frozen entry is not a table");
        for (const ::std::uint32_t id : graph_.protected_names) require(is(id, kind_string), "protected name is not a string");
        if (!ok()) error = error_;
        return ok();
    }

[[nodiscard]] bool GraphValidator::ok() const noexcept { return error_.empty(); }

void GraphValidator::require(bool condition, ::std::string_view message) {
        if (!condition && error_.empty()) error_ = ::std::string(message);
    }

[[nodiscard]] const IrObject* GraphValidator::object(::std::uint32_t id) const noexcept {
        return id >= 1 && id <= graph_.objects.size() ? &graph_.objects[id - 1] : nullptr;
    }

[[nodiscard]] bool GraphValidator::is(::std::uint32_t id, Kind kind) const noexcept {
        const IrObject* found = object(id);
        return found != nullptr && found->kind == kind;
    }

[[nodiscard]] bool GraphValidator::is_table(::std::uint32_t id) const noexcept { return is(id, kind_table) || is(id, kind_handle_table); }

[[nodiscard]] bool GraphValidator::is_value(const IrValue& value) const noexcept {
        if (value.tag != tag_reference) return true;
        const IrObject* found = object(value.reference);
        return found != nullptr && found->kind != kind_prototype && found->kind != kind_upvalue;
    }

[[nodiscard]] bool GraphValidator::reference_to(const IrValue& value, Kind kind) const noexcept {
        return value.tag == tag_reference && is(value.reference, kind);
    }

[[nodiscard]] bool GraphValidator::anchorable(const IrValue& value) const noexcept {
        if (value.tag != tag_reference) return false;
        const IrObject* found = object(value.reference);
        return found != nullptr && found->kind != kind_string && found->kind != kind_handle &&
            found->kind != kind_prototype && found->kind != kind_upvalue;
    }

[[nodiscard]] bool GraphValidator::string_is(const IrValue& value, ::std::string_view text) const noexcept {
        return reference_to(value, kind_string) && object(value.reference)->text == text;
    }

[[nodiscard]] const IrValue* GraphValidator::lookup(::std::uint32_t table, const IrValue& key) const {
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

[[nodiscard]] const IrValue* GraphValidator::registry_field(::std::string_view name) const {
        const IrObject& registry = graph_.objects[graph_.registry - 1];
        for (::std::size_t index = 0; index < registry.values.size(); index += 2) {
            if (string_is(registry.values[index], name)) return &registry.values[index + 1];
        }
        return nullptr;
    }

void GraphValidator::check_specials() {
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

void GraphValidator::check_table(::std::uint32_t id, const IrObject& table) {
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

void GraphValidator::check_c_function(IrObject& function) {
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

void GraphValidator::check_thread(const IrObject& thread) {
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

void GraphValidator::check_object(::std::uint32_t id) {
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

void GraphValidator::check_identities() {
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

void GraphValidator::check_handles() {
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

void GraphValidator::collect_anchored() {
        if (!is(graph_.anchors, kind_table)) return;
        const IrObject& anchors = graph_.objects[graph_.anchors - 1];
        for (::std::size_t index = 1; index < anchors.values.size(); index += 2) {
            if (anchors.values[index].tag == tag_reference) anchored_.insert(anchors.values[index].reference);
        }
    }

} // namespace persist_internal

using namespace persist_internal;

::std::uint64_t measure_logical_bytes(const Sandbox::Impl& sandbox) { return LogicalSizer(sandbox).run(); }

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE

#endif // !EAWR_SFLUA_HARDWARE_TWIN
