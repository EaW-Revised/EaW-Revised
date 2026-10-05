#include "host_support.hpp"

namespace host_test_support {

void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("failed to write synthetic binary fixture");
}

const float* as_number(const eawr::script::ScriptValue& value) {
    return std::get_if<float>(&value.storage());
}

const eawr::script::ScriptValue::Sequence* as_sequence(const eawr::script::ScriptValue& value) {
    const auto* storage = std::get_if<eawr::script::ScriptValue::SequenceStorage>(&value.storage());
    return storage != nullptr && *storage ? storage->get() : nullptr;
}

void test_host() {
    TempTree tree;
    write_text(tree.root / "Scripts" / "main.lua", R"lua(
counter = 0
a_count, b_count, c_count, d_count = 0, 0, 0, 0
mutated = false
event_sum = 0
function Isolation() require("Exact"); require("Exact"); require("exact"); return counter end
function FalseTwice() require("FalseModule"); require("FalseModule"); return false_counter end
function MultiCall() local a,b,c=Function_Call(Multi, 11, "x", true); return {a,b,c} end
function Worker(v) coroutine.yield(true); coroutine.yield(false) end
function Returner() return true end
function Failer() error("thread boom") end
function EmptyYield() coroutine.yield() end
function MultiYield() coroutine.yield(true, false) end
function MultiYieldLastTrue() coroutine.yield(false, true); return false end
function NonBooleanYield() coroutine.yield(1) end
return_count = 0
function CountingReturner() return_count=return_count+1; return true end
function ReturnCount() return return_count end
function MakeThread(name, value) return Create_Thread(name, value) end
function A() a_count=a_count+1 end
function B() b_count=b_count+1; if not mutated then mutated=true; Cancel_Event("scan", C) end end
function C() c_count=c_count+1 end
function D() d_count=d_count+1 end
function SetupEvents() Register_Event("scan", A); Register_Event("scan", B); Register_Event("scan", C); Register_Event("scan", D) end
function Counts() return {a_count,b_count,c_count,d_count} end
function Red() end
function Blue() end
function EventWorker() local r=GetEvent(); local b=GetEvent(); local p1=GetEvent.Params(); local p2=GetEvent.Params(); event_sum=p1+p2 end
function MakeEventThread() return Create_Thread("EventWorker") end
function Queue(slot) Queue_Event(slot, Red, 10); Queue_Event(slot, Blue, 20) end
function EventSum() return event_sum end
function LibraryMask() return {_G~=nil,_VERSION=="Lua 5.0.2",_LOADED~=nil,coroutine~=nil,require~=nil,loadfile~=nil,dofile~=nil,loadstring~=nil,string~=nil,table~=nil,package==nil,math==nil,io==nil,os==nil,debug==nil} end
function UseDofile() dofile("data/scripts/lib/setter.lua"); return loaded_v end
function MissingApi() Lock_Controls(1) end
function DestroyDuringCall() DestroyNow(); post_destroy_callback=true end
not_function = 3
function FailFunction() error("synthetic function failure") end
    )lua");
    write_text(tree.root / "Scripts" / "lib" / "Exact.lua", "counter=counter+1\n");
    // The VFS deliberately rejects two loose files whose ASCII-folded logical paths
    // collide. Both require spellings below therefore resolve the same physical file,
    // while the script host still proves that its module-cache keys retain spelling.
    write_text(tree.root / "Scripts" / "lib" / "FalseModule.lua", "false_counter=(false_counter or 0)+1; return false\n");
    write_text(tree.root / "Scripts" / "lib" / "setter.lua", "loaded_v=37\n");
    const auto fixtures = std::filesystem::path(EAWR_SCRIPT_FIXTURES);
    write_bytes(tree.root / "Scripts" / "minimal.lua", read_hex(fixtures / "minimal-valid.pglua.hex"));
    write_bytes(tree.root / "Scripts" / "nested.lua", read_hex(fixtures / "nested-debug.pglua.hex"));

    const std::array mounts{eawr::vfs::MountSpec{"synthetic", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic script VFS mounts");
    if (!mounted) return;
    eawr::script::ScriptHost host(mounted.value());
    auto registered = host.register_api(
        "Multi", "(number,string,boolean)->(number,string,boolean)",
        [](const eawr::script::ApiCallContext&, const eawr::script::ValueList&) {
            return eawr::core::Result<eawr::script::ValueList>::success({
                eawr::script::ScriptValue(3.5F), eawr::script::ScriptValue("done"), eawr::script::ScriptValue(false),
            });
        }
    );
    expect(static_cast<bool>(registered), "host API registers");
    expect(static_cast<bool>(host.register_api("Lock_Controls", "(number)->()")),
        "known missing API declaration registers without an implementation");
    expect(static_cast<bool>(host.register_api(
        "DestroyNow", "()->()",
        [&host](const eawr::script::ApiCallContext& context, const eawr::script::ValueList&) {
            auto destroyed = host.destroy(context.instance);
            if (!destroyed) return eawr::core::Result<eawr::script::ValueList>::failure(destroyed.error());
            return eawr::core::Result<eawr::script::ValueList>::success({});
        }
    )), "destroy-during-callback probe registers");
    auto loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    if (!loaded) std::cerr << "host load error: " << loaded.error().code << ' ' << loaded.error().message << '\n';
    expect(static_cast<bool>(loaded), "synthetic Lua instance loads");
    if (!loaded) return;
    const auto id = loaded.value();
    auto second = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    expect(static_cast<bool>(second), "second isolated Lua instance loads");
    auto minimal_loaded = host.load("data/scripts/minimal.lua");
    if (!minimal_loaded) std::cerr << "minimal VM load error: " << minimal_loaded.error().code
                                  << ' ' << minimal_loaded.error().message << '\n';
    expect(static_cast<bool>(minimal_loaded),
        "converted minimal PGLua executes in the native-width VM");
    auto nested_loaded = host.load("data/scripts/nested.lua");
    if (!nested_loaded) std::cerr << "nested VM load error: " << nested_loaded.error().code
                                 << ' ' << nested_loaded.error().message << '\n';
    expect(static_cast<bool>(nested_loaded),
        "converted nested/debug PGLua executes in the native-width VM");

    auto isolation = host.start(id, "Isolation");
    expect(isolation.succeeded() && isolation.result && as_number(*isolation.result) &&
        *as_number(*isolation.result) == 2.0F,
        "exact-key require caches by instance and spelling");
    auto second_isolation = second ? host.start(second.value(), "Isolation") : eawr::script::CallOutcome{};
    expect(second_isolation.succeeded() && second_isolation.result &&
        as_number(*second_isolation.result) && *as_number(*second_isolation.result) == 2.0F,
        "globals and module caches are isolated between instances");
    auto false_twice = host.start(id, "FalseTwice");
    expect(false_twice.succeeded() && false_twice.result && as_number(*false_twice.result) &&
        *as_number(*false_twice.result) == 2.0F,
        "false module results are not truthy cache hits");

    auto multi = host.start(id, "MultiCall");
    const auto* multi_values = multi.result ? as_sequence(*multi.result) : nullptr;
    expect(multi.succeeded() && multi_values != nullptr && multi_values->size() == 3 &&
        as_number((*multi_values)[0]) && *as_number((*multi_values)[0]) == 3.5F,
        "Function_Call preserves multi-return order");
    expect(host.invocations().size() == 1 && host.invocations()[0].arguments.size() == 3,
        "host invocation log preserves arguments");

    auto make_a = host.start(id, "MakeThread", {eawr::script::ScriptValue("Worker"), eawr::script::ScriptValue(7.0F)});
    auto make_b = host.start(id, "MakeThread", {eawr::script::ScriptValue("Returner")});
    auto make_c = host.start(id, "MakeThread", {eawr::script::ScriptValue("Failer")});
    const auto slot_a = static_cast<std::uint32_t>(*as_number(*make_a.result));
    const auto slot_b = static_cast<std::uint32_t>(*as_number(*make_b.result));
    const auto slot_c = static_cast<std::uint32_t>(*as_number(*make_c.result));
    expect(host.resume({id, slot_a}).state == eawr::script::ResumeState::live,
        "single boolean true yield keeps coroutine live");
    expect(host.resume({id, slot_b}).state == eawr::script::ResumeState::live,
        "normal return true keeps coroutine live");
    expect(host.resume({id, slot_b}).state == eawr::script::ResumeState::live,
        "a true-returning coroutine restarts from its function");
    auto counting_thread = host.start(id, "MakeThread", {eawr::script::ScriptValue("CountingReturner")});
    const auto counting_slot = static_cast<std::uint32_t>(*as_number(*counting_thread.result));
    const bool restarted_twice = host.resume({id, counting_slot}).state == eawr::script::ResumeState::live &&
        host.resume({id, counting_slot}).state == eawr::script::ResumeState::live;
    auto return_count = host.start(id, "ReturnCount");
    expect(restarted_twice && return_count.result && as_number(*return_count.result) &&
           *as_number(*return_count.result) == 2.0F,
           "a true return executes the function again on the next resume");
    auto errored = host.resume({id, slot_c});
    expect(!errored.succeeded() && errored.state == eawr::script::ResumeState::ended,
        "coroutine error ends its stable slot");
    auto make_d = host.start(id, "MakeThread", {eawr::script::ScriptValue("Returner")});
    expect(make_d.result && *as_number(*make_d.result) > static_cast<float>(slot_c),
        "ended coroutine slots are not reused");
    expect(host.resume({id, slot_a}).state == eawr::script::ResumeState::ended,
        "boolean false yield ends coroutine");
    auto empty_thread = host.start(id, "MakeThread", {eawr::script::ScriptValue("EmptyYield")});
    const auto empty_slot = static_cast<std::uint32_t>(*as_number(*empty_thread.result));
    auto empty_resume = host.resume({id, empty_slot});
    expect(empty_resume.succeeded() && empty_resume.state == eawr::script::ResumeState::ended,
        "empty yield ends coroutine without guessing a value");
    auto multi_thread = host.start(id, "MakeThread", {eawr::script::ScriptValue("MultiYield")});
    const auto multi_slot = static_cast<std::uint32_t>(*as_number(*multi_thread.result));
    auto multi_resume = host.resume({id, multi_slot});
    expect(multi_resume.succeeded() && multi_resume.state == eawr::script::ResumeState::ended,
        "a multi-value yield ending in false ends its slot");
    auto last_true_thread = host.start(id, "MakeThread", {eawr::script::ScriptValue("MultiYieldLastTrue")});
    const auto last_true_slot = static_cast<std::uint32_t>(*as_number(*last_true_thread.result));
    expect(host.resume({id, last_true_slot}).state == eawr::script::ResumeState::live &&
           host.resume({id, last_true_slot}).state == eawr::script::ResumeState::ended,
        "a multi-value yield ending in true keeps the slot until its next result");
    auto non_boolean_thread = host.start(id, "MakeThread", {eawr::script::ScriptValue("NonBooleanYield")});
    const auto non_boolean_slot = static_cast<std::uint32_t>(*as_number(*non_boolean_thread.result));
    expect(host.resume({id, non_boolean_slot}).state == eawr::script::ResumeState::ended,
        "a non-boolean topmost yield ends its slot");

    expect(host.start(id, "SetupEvents").succeeded(), "event registrations install");
    auto dispatched = host.dispatch(id, "scan");
    expect(dispatched.succeeded() && dispatched.invoked == 4,
        "event mutation restarts before the mutating callback is completed");
    auto counts = host.start(id, "Counts");
    const auto* count_values = counts.result ? as_sequence(*counts.result) : nullptr;
    expect(count_values && count_values->size() == 4 && *as_number((*count_values)[0]) == 1.0F &&
        *as_number((*count_values)[1]) == 2.0F && *as_number((*count_values)[2]) == 0.0F &&
        *as_number((*count_values)[3]) == 1.0F,
        "event mutation ordering is A once, B twice, C never, D once");

    auto event_thread = host.start(id, "MakeEventThread");
    const auto event_slot = static_cast<std::uint32_t>(*as_number(*event_thread.result));
    expect(host.start(id, "Queue", {eawr::script::ScriptValue(static_cast<float>(event_slot))}).succeeded(),
        "parallel event queues accept callback and parameter entries");
    expect(host.resume({id, event_slot}).succeeded(), "event consumer coroutine runs");
    auto event_sum = host.start(id, "EventSum");
    expect(event_sum.result && *as_number(*event_sum.result) == 30.0F,
        "event parameter queue preserves FIFO order independently");

    auto libraries = host.start(id, "LibraryMask");
    const auto* mask = libraries.result ? as_sequence(*libraries.result) : nullptr;
    expect(mask && mask->size() == 15 && std::all_of(mask->begin(), mask->end(), [](const auto& value) {
        const auto* flag = std::get_if<bool>(&value.storage()); return flag != nullptr && *flag;
    }), "only base/coroutine, string, and table libraries are exposed");
    auto dofile = host.start(id, "UseDofile");
    expect(dofile.result && *as_number(*dofile.result) == 37.0F, "dofile reads through the logical VFS");

    auto missing = host.start(id, "MissingApi");
    expect(!missing.succeeded() && missing.error &&
        missing.error->diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api &&
        missing.error->api_name == "Lock_Controls" && !missing.error->traceback.empty(),
        "known unimplemented engine call has distinct contextual diagnostic");
    expect(host.start(id, "AbsentFunction").succeeded() && !host.start(id, "AbsentFunction").result,
        "absent host-to-Lua call produces no result without missing-API failure");
    expect(host.start(id, "not_function").succeeded() && !host.start(id, "not_function").result,
        "nonfunction host-to-Lua call produces no result");
    auto failing_function = host.start(id, "FailFunction");
    expect(!failing_function.succeeded() && failing_function.error &&
        failing_function.error->diagnostic.code == eawr::script::diagnostic_codes::lua_execution,
        "failing host-to-Lua call follows the ordinary Lua error path");

    auto trace = host.module_trace(id);
    expect(trace && trace.value().size() == 5 && trace.value()[1].cache_hit,
        "module request trace includes exact truthy cache hits and false retries");
    auto reentry = host.start(id, "DestroyDuringCall");
    expect(!reentry.succeeded() && reentry.error &&
        reentry.error->diagnostic.code == eawr::script::diagnostic_codes::invalid_instance,
        "destroy during callback rejects callback re-entry and unwinds safely");
    expect(static_cast<bool>(host.destroy(id)), "destroy succeeds");
    expect(static_cast<bool>(host.destroy(id)), "destroy is idempotent");
    expect(!host.start(id, "Isolation").succeeded(), "operations after destroy return invalid-instance");
    if (second) expect(static_cast<bool>(host.destroy(second.value())), "second instance destroys independently");
}


} // namespace host_test_support
