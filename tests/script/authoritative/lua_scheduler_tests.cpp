#include "lua_sandbox_support.hpp"
#include "../../support/reverse_partition_executor.hpp"

namespace lua_sandbox_test_support {

namespace {
struct WalletShadow final : auth::ImmediateEffectState {
    std::int64_t balance{};
    std::uint64_t next_id{1};
    std::vector<std::uint64_t> objects;
    std::unique_ptr<auth::ImmediateEffectState> clone() const override { return std::make_unique<WalletShadow>(*this); }
    eawr::core::Result<auth::ValueList> apply(std::string_view action, const auth::ValueList& args) override {
        using Result = eawr::core::Result<auth::ValueList>;
        if (action == "spawn") {
            objects.push_back(next_id++);
            return Result::success({auth::Value{auth::Handle{9, objects.back()}}});
        }
        balance += b64::to_int64_truncate(std::get<LuaNumber>(args.at(0).data).repr);
        if (action == "fail") {
            eawr::core::Diagnostic error; error.code = "test.refused"; error.message = "action refused after a private write";
            return Result::failure(error);
        }
        return Result::success({});
    }
    eawr::core::Result<auth::ValueList> query(std::string_view name, const auth::ValueList&) const override {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(
            name == "count" ? static_cast<std::int64_t>(objects.size()) : balance))});
    }
};
struct WalletHost final : auth::ImmediateEffectHost {
    WalletShadow state;
    unsigned publications{};
    mutable unsigned captures{};
    bool refuse{};
    eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>> capture() const override {
        ++captures;
        return eawr::core::Result<std::unique_ptr<auth::ImmediateEffectState>>::success(state.clone());
    }
    eawr::core::Result<void> publish(std::unique_ptr<auth::ImmediateEffectState> shadow) override {
        if (refuse) {
            eawr::core::Diagnostic error; error.code = "test.publish"; error.message = "publication refused";
            return eawr::core::Result<void>::failure(error);
        }
        state = dynamic_cast<const WalletShadow&>(*shadow);
        ++publications;
        return eawr::core::Result<void>::success();
    }
};

void immediate_contracts() {
    const std::string source = R"LUA(
function Good()
    local before = Read()
    Add(Self())
    Test_Report(before, Read(), Spawn(), Count())
end
function Failed()
    Add(1000) Spawn() Test_Report('discard') error('rollback')
end
function Caught()
    local before = Read()
    local ok = pcall(function() Fail(1000) end)
    Test_Report(ok, before == Read())
end
function Quota() Add(1000) Spawn() while true do end end
function DispatchFailure() Add(1000) Spawn() error('dispatch rollback') end
Register_Event('Bad', DispatchFailure)
)LUA";
    std::vector<std::string> baseline;
    std::string digest;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auth::SessionConfig config = default_config(); config.quotas.instructions_per_service = 10000;
        Session session = make_session({{"Main.lua", source}, {"Root.lua", "Add(7) Seen = Read()"},
            {"BadRoot.lua", "Add(999) Spawn() error('bad root')"}}, config);
        auto host = std::make_shared<WalletHost>();
        expect(static_cast<bool>(session.scheduler->register_binding("Self", [](auth::BindingContext& context, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(context.instance()))});
        })), "register instance identity");
        expect(static_cast<bool>(session.scheduler->set_immediate_effect_host(host, 4)), "register bounded immediate host");
        for (const auto& action : {"Add", "Fail", "Spawn"}) {
            const std::string verb = action == std::string_view("Add") ? "add" : action == std::string_view("Fail") ? "fail" : "spawn";
            expect(static_cast<bool>(session.scheduler->register_binding(action, [verb](auth::BindingContext& context, const auth::ValueList& args) {
                return context.apply_immediate(verb, args);
            })), "register immediate action");
        }
        for (const auto& name : {"Read", "Count"}) {
            const std::string query = name == std::string_view("Read") ? "credits" : "count";
            expect(static_cast<bool>(session.scheduler->register_binding(name, [query](auth::BindingContext& context, const auth::ValueList& args) {
                return context.query_immediate(query, args);
            })), "register immediate query");
        }
        expect(!session.scheduler->create_instance(8, "BadRoot.lua") && host->state.balance == 0 && host->state.next_id == 1,
            "failed root discards cash and identity allocation");
        expect(static_cast<bool>(session.scheduler->create_instance(9, "Root.lua")) && host->state.balance == 7, "successful root publishes immediately");
        expect(std::get<LuaNumber>(session.scheduler->read_global(9, "Seen").value()->data) == LuaNumber(7), "root reads its write");
        expect(static_cast<bool>(session.scheduler->remove_instance(9)), "remove root probe");
        for (const std::uint64_t id : {3U, 2U, 1U}) expect(static_cast<bool>(session.scheduler->create_instance(id, "Main.lua")), "reverse instance creation");
        const auto post = [&](std::uint64_t id, std::uint64_t sequence, std::string name) {
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, id, sequence};
            event.target = id; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name);
            expect(static_cast<bool>(session.scheduler->submit_event(std::move(event))), "post native contract event");
        };
        post(1, 0, "Good"); post(1, 1, "Failed"); post(1, 2, "Caught");
        auth::ScriptEvent dispatched = dispatch_event({1, auth::first_simulation_producer, 1, 3}, 1, "Bad", {});
        expect(static_cast<bool>(session.scheduler->submit_event(std::move(dispatched))), "post failed registered handler");
        post(2, 0, "Good"); post(2, 1, "Quota"); post(3, 0, "Good");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto result = session.scheduler->service(executor);
        expect(static_cast<bool>(result), "immediate service completes despite Lua error and quota fault");
        if (!result) continue;
        std::vector<std::string> commands;
        for (const auto& command : result.value().commands) commands.push_back(text(command));
        expect(host->state.balance == 11 && host->state.objects == std::vector<std::uint64_t>{1, 2} && host->state.next_id == 3,
            "failed handler and whole faulted instance discard all cash, objects and identities");
        expect(commands == std::vector<std::string>{"1#0 report 7 8 handle(9,1) 1", "1#1 report false true", "3#0 report 8 11 handle(9,2) 2"},
            "same-call reads and later canonical instance see successful writes only: " + joined(commands));
        expect(host->publications == 2, "one root publication and one whole-service publication");
        expect(result.value().diagnostics.size() == 3 && result.value().removed_instances == std::vector<std::uint64_t>{2}, "rollback diagnostics and quota removal");
        const auto hash = session.scheduler->state_digest();
        if (baseline.empty()) { baseline = commands; digest = hash; }
        else expect(commands == baseline && hash == digest, "immediate equality on one/two/four/eight workers");
        expect(!session.scheduler->set_immediate_effect_host(host, 1), "native bound rejects existing oversized set");
        host->refuse = true;
        auth::ScriptEvent event; event.key = {2, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Good";
        expect(static_cast<bool>(session.scheduler->submit_event(event)), "post refused publish");
        const auto refused = session.scheduler->service(executor);
        expect(!refused && refused.error().code == auth::codes::session_abort && host->state.balance == 11 && host->state.next_id == 3,
            "failed atomic publication aborts without leaking native effects");
        expect(!session.scheduler->service(executor), "native publication failure is terminal");
    }
    {
        Session session = script_session("function Main() Add(5) coroutine.yield(true) Add(999) error('resume rollback') end Create_Thread('Main')");
        auto host = std::make_shared<WalletHost>();
        expect(static_cast<bool>(session.scheduler->register_binding("Add", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("add", args);
        })), "register coroutine action");
        expect(static_cast<bool>(session.scheduler->set_immediate_effect_host(host, 1)), "coroutine native host");
        const eawr::sim::InlineExecutor executor;
        expect(static_cast<bool>(session.scheduler->create_instance(1, "Main.lua")), "create native coroutine");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "successful yield publishes writes");
        const auto saved = session.scheduler->save();
        expect(static_cast<bool>(saved), "save native coroutine at barrier");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "failed resume discards only current resume");
        expect(host->publications == 1, "rolled-back service needs no native publication");
        expect(saved && static_cast<bool>(session.scheduler->load(saved.value())), "load retains runtime immediate host");
        expect(static_cast<bool>(session.scheduler->service(executor)) && host->state.balance == 5, "restored coroutine has the same native rollback");
    }
    // S9a: chains. Instances of one owner run in ID order over a shadow of their own, the chains run in ascending
    // owner order, and every chain publishes before the next one captures. The worker count and the order the
    // partitions run in never show: the inline run, 1/2/4/8 workers and a highest-first executor agree.
    struct ChainRun {
        std::vector<std::string> commands;
        unsigned publications{}, captures{};
        std::int64_t balance{};
        bool operator==(const ChainRun&) const = default;
    };
    const auto run_chains = [&](const eawr::sim::PartitionExecutor& executor, bool chained, const std::string& chain_source) {
        Session session = make_session({{"Main.lua", chain_source}});
        auto host = std::make_shared<WalletHost>();
        expect(static_cast<bool>(session.scheduler->register_binding("Self", [](auth::BindingContext& context, const auth::ValueList&) {
            return eawr::core::Result<auth::ValueList>::success({auth::Value::number(LuaNumber(context.instance()))});
        })), "register chain identity");
        expect(static_cast<bool>(session.scheduler->set_immediate_effect_host(host, 4)), "chain host");
        expect(static_cast<bool>(session.scheduler->register_binding("Add", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("add", args);
        })), "register chain action");
        expect(static_cast<bool>(session.scheduler->register_binding("Read", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.query_immediate("credits", args);
        })), "register chain query");
        for (const std::uint64_t id : {1U, 2U, 3U}) {
            expect(static_cast<bool>(session.scheduler->create_instance(id, "Main.lua")), "create chain instance");
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, id, 0};
            event.target = id; event.kind = auth::ScriptEvent::Kind::call; event.name = "Good";
            expect(static_cast<bool>(session.scheduler->submit_event(std::move(event))), "post chain event");
        }
        auth::ServiceOptions options;
        if (chained) options.chain_of = [](std::uint64_t instance) -> std::uint64_t { return instance == 2 ? 10 : 20; };
        // Creating an instance captures too; only the service's own captures count.
        const unsigned captured_before = host->captures, published_before = host->publications;
        auto result = session.scheduler->service(executor, options);
        expect(static_cast<bool>(result), "chained immediate service completes");
        ChainRun run;
        run.publications = host->publications - published_before; run.captures = host->captures - captured_before;
        run.balance = host->state.balance;
        if (result) for (const auto& command : result.value().commands) run.commands.push_back(text(command));
        return run;
    };
    {
        const std::string chain_source = "function Good() local before = Read() Add(Self()) Test_Report(before, Read()) end";
        const eawr::sim::InlineExecutor inline_executor;
        const auto plain = run_chains(inline_executor, false, chain_source);
        const auto chained = run_chains(inline_executor, true, chain_source);
        expect(plain.commands == std::vector<std::string>{"1#0 report 0 1", "2#0 report 1 3", "3#0 report 3 6"}, "one chain runs in ID order: " + joined(plain.commands));
        // Owner 10 (instance 2) runs first; owner 20 (instances 1 and 3) captures after its publication.
        expect(chained.commands == std::vector<std::string>{"1#0 report 2 3", "2#0 report 0 2", "3#0 report 3 6"}, "chains run per owner in owner order: " + joined(chained.commands));
        expect(plain.balance == 6 && chained.balance == 6, "both orders end with every write");
        expect(plain.publications == 1 && chained.publications == 2, "a chain publishes once, before the next chain captures");
        expect(plain.captures == 1 && chained.captures == 2, "a chain captures once, after the previous chain published");
        for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(run_chains(executor, false, chain_source) == plain, "the plain chain equals the inline run on every determinism worker count");
            expect(run_chains(executor, true, chain_source) == chained, "the owner chains equal the inline run on every determinism worker count");
        }
        for (const std::size_t reported : {std::size_t{1}, std::size_t{8}}) {
            eawr::test_support::ReverseOrderExecutor reverse(reported);
            expect(run_chains(reverse, true, chain_source) == chained, "running every partition highest-first changes no chain");
            // Every partitioned phase the service ran visited its partitions highest first.
            bool descending = true;
            for (const auto& phase : reverse.phases) descending = descending && phase.descending();
            expect(descending, "the reverse executor visited every phase's partitions highest first");
        }
        // Chains that change nothing publish nothing and share the one capture.
        const std::string readonly_source = "function Good() Test_Report(Read(), Self()) end";
        const auto readonly = run_chains(inline_executor, true, readonly_source);
        expect(readonly.publications == 0 && readonly.captures == 1 && readonly.balance == 0,
            "owner chains that change nothing neither publish nor capture again");
        for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(run_chains(executor, true, readonly_source) == readonly, "the read-only chains equal the inline run on every determinism worker count");
        }
    }
    {
        Session session = script_session("Immediate()");
        expect(static_cast<bool>(session.scheduler->register_binding("Immediate", [](auth::BindingContext& context, const auth::ValueList& args) {
            return context.apply_immediate("add", args);
        })), "register unconfigured native action");
        const auto rejected = session.scheduler->create_instance(1, "Main.lua");
        expect(!rejected && rejected.error().message.find(auth::codes::missing_api) != std::string::npos, "unconfigured profile fails explicitly");
    }
}
} // namespace

// ---- object turns (#3644 A2a, #3702) ----

namespace {

// Runs the partitions of a phase from the last to the first, on the calling thread.
class ReversePartitionExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 1; }
    [[nodiscard]] eawr::core::Result<void> execute(
        std::size_t partition_count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t index = partition_count; index-- > 0;) partition(index);
        return eawr::core::Result<void>::success();
    }
};

const char* const turn_object_source = R"LUA(
Count = 0
Last = -1
Seed = Test_Random(0)
function Base_Definitions() end
function Barrier() Barrier_Last = Test_Random(0) Test_Report('barrier', Barrier_Last) end
function main()
    while true do
        Count = Count + 1
        Last = Test_Random(0)
        Test_Report('turn', Count, Last)
        coroutine.yield(true)
    end
end
)LUA";

Session make_object_session(const std::vector<std::uint64_t>& ids, const std::map<std::string, std::string>& extra = {},
                            const std::map<std::uint64_t, std::string>& modules = {}, auth::SessionConfig config = default_config()) {
    std::map<std::string, std::string> sources{{"Obj.lua", turn_object_source}};
    sources.insert(extra.begin(), extra.end());
    Session session = make_session(sources, std::move(config));
    std::vector<auth::ObjectScript> objects;
    for (const std::uint64_t id : ids) {
        const auto found = modules.find(id);
        objects.push_back({auth::Handle{1, id}, 1, id, found == modules.end() ? "Obj.lua" : found->second});
    }
    const auto attached = session.scheduler->reconcile_objects(objects);
    expect(attached && attached.value().empty(), "object turn attach");
    return session;
}

void submit_due(Session& session, std::uint64_t id, auth::ScriptEvent::Kind kind, std::uint64_t sequence, std::string name = {}) {
    auth::ScriptEvent event;
    event.key = {session.scheduler->completed_tick() + 1, auth::first_simulation_producer, id, sequence};
    event.target = id;
    event.kind = kind;
    event.name = std::move(name);
    expect(session.scheduler->submit_event(std::move(event)).has_value(), "submit object event");
}

// Pumps `ids` in one turn of the next tick; every report line is appended to `lines`.
auth::ServiceReport turn(Session& session, const eawr::sim::PartitionExecutor& executor, const std::vector<std::uint64_t>& ids,
                         std::uint64_t pump_sequence, std::vector<std::string>* lines = nullptr) {
    for (const std::uint64_t id : ids) submit_due(session, id, auth::ScriptEvent::Kind::pump, pump_sequence);
    auto report = session.scheduler->service_turn(executor, ids);
    expect(report.has_value(), "service_turn: " + (report ? std::string() : report.error().code + " " + report.error().message));
    if (!report) return {};
    if (lines != nullptr) {
        for (const auto& command : report.value().commands) lines->push_back(text(command));
        for (const auto& diagnostic : report.value().diagnostics) lines->push_back("diag " + diagnostic.code);
    }
    return std::move(report).value();
}

std::vector<std::string> only_issuer(const std::vector<std::string>& lines, std::uint64_t issuer) {
    std::vector<std::string> selected;
    const std::string prefix = std::to_string(issuer) + "#";
    for (const std::string& line : lines) if (line.rfind(prefix, 0) == 0) selected.push_back(line);
    return selected;
}

std::int64_t global_number(Session& session, std::uint64_t id, std::string_view name) {
    const auto value = session.scheduler->read_global(id, name);
    if (!value || !value.value() || !std::holds_alternative<LuaNumber>(value.value()->data)) return -1;
    return b64::to_int64_truncate(std::get<LuaNumber>(value.value()->data).repr);
}

// Test_Random(0) returns the draw's word >> 12 as an integer.
std::int64_t unit_word(std::uint64_t tick_key, std::uint64_t id, std::uint64_t draw) {
    return static_cast<std::int64_t>(auth::random_word(default_config().seed, tick_key, id, draw) >> 12);
}

void object_turn_contracts() {
    const eawr::sim::InlineExecutor executor;
    {
        Session session = make_object_session({3});
        const auto saved = session.scheduler->save();
        const auto hash = session.scheduler->state_hash();
        expect(saved && hash, "host-read accounting fixture saves and hashes");
        for (unsigned read = 0; read < 16; ++read) {
            expect(global_number(session, 3, "Count") == 0, "host global read returns the stored value");
            auto missing = session.scheduler->read_global(3, "Never_Authored_By_This_Module");
            expect(missing && missing.value() && std::holds_alternative<std::monostate>(missing.value()->data),
                "host global read preserves absent values");
        }
        expect(saved && session.scheduler->save().value() == saved.value(),
            "host global inspection leaves canonical save bytes and logical quotas unchanged");
        expect(hash && session.scheduler->state_hash().value() == hash.value(),
            "host global inspection leaves the script hash unchanged");
    }
    {
        Session session = make_object_session({3});
        turn(session, executor, {3}, 10);
        submit_due(session, 3, auth::ScriptEvent::Kind::call, 20, "Barrier");
        auth::ServiceOptions options;
        options.defer_instance = [](std::uint64_t id) { return id == 3; };
        for (unsigned held=0;held<4;++held) {
            auto barrier = session.scheduler->service(executor, options);
            expect(barrier && barrier.value().loads.empty()
                && std::holds_alternative<std::monostate>(session.scheduler->read_global(3,"Barrier_Last").value()->data),
                "object owner defers barrier callbacks without double servicing its instance");
        }
        const auto saved=session.scheduler->save();
        Session restored=make_object_session({3});
        expect(saved.has_value(),"deferred callback saves after several barriers");if (!saved) return;
        auto loaded=restored.scheduler->load(saved.value());
        expect(loaded.has_value(),"deferred overdue callback restores after several barriers");
        if (!loaded) { std::cerr<<loaded.error().message<<'\n'; return; }
        expect(restored.scheduler->save().value()==saved.value(),"deferred clock and pending callback bytes restore canonically");
        auto next=session.scheduler->service_turn(executor,{3});
        const eawr::platform::ThreadWorkerAdapter other_workers(4);
        auto back=restored.scheduler->service_turn(other_workers,{3});
        expect(next && back && next.value().commands.size()==1 && back.value().commands.size()==1
            && global_number(session,3,"Count")==1 && global_number(restored,3,"Count")==1
            && global_number(session,3,"Barrier_Last")==global_number(restored,3,"Barrier_Last")
            && session.scheduler->state_hash().value()==restored.scheduler->state_hash().value(),
            "saved deferred callbacks arrive once in the next turn on another worker count without a new pump");
    }
    {
        Session plain=make_session({{"Obj.lua",turn_object_source}});
        expect(bool(plain.scheduler->create_instance(9,"Obj.lua")),"plain deferral guard instance creates");
        const auto before=plain.scheduler->state_hash();
        auth::ServiceOptions options;options.defer_instance=[](std::uint64_t) { return true; };
        auto refused=plain.scheduler->service(executor,options);
        expect(before && !refused && plain.scheduler->state_hash().value()==before.value(),
            "subset deferral refuses an unattached instance without changing saved state");
    }
    constexpr std::uint64_t domain = auth::turn_random_domain;
    {
        // A subset turn equals the full turn restricted to the subset; the rest stay untouched.
        const std::vector<std::uint64_t> all{1, 2, 3, 4, 5, 6}, subset{2, 5};
        Session partial = make_object_session(all);
        Session full = make_object_session(all);
        std::vector<std::string> partial_lines, full_lines;
        turn(partial, executor, all, 10);
        turn(full, executor, all, 10);
        const auto partial_report = turn(partial, executor, subset, 11, &partial_lines);
        turn(full, executor, all, 11, &full_lines);
        expect(partial_report.tick == 1 && partial_report.loads.size() == 2 && partial.scheduler->completed_tick() == 0,
               "subset turn reports its instances and does not advance completed_tick");
        for (const std::uint64_t id : all) {
            const bool serviced = id == 2 || id == 5;
            expect(only_issuer(partial_lines, id).size() == (serviced ? 1U : 0U), "only the subset commands commit");
            if (serviced) {
                expect(only_issuer(partial_lines, id) == only_issuer(full_lines, id), "subset commands equal the full turn restricted");
                expect(global_number(partial, id, "Count") == 2 && global_number(partial, id, "Last") == global_number(full, id, "Last"),
                       "subset instance state equals the full turn");
            } else {
                expect(global_number(partial, id, "Count") == 1 && global_number(full, id, "Count") == 2, "other instances are not serviced");
            }
        }
    }
    {
        // The turn draws in its own key domain, on the instance's one counter, and the barrier continues after it.
        Session session = make_object_session({3});
        expect(global_number(session, 3, "Seed") == unit_word(1, 3, 0), "creation draws from the barrier domain");
        turn(session, executor, {3}, 10);
        expect(global_number(session, 3, "Last") == unit_word(1 | domain, 3, 1), "turn draws from its own domain");
        expect(global_number(session, 3, "Last") != unit_word(1, 3, 1), "turn never reuses the barrier key");
        submit_due(session, 3, auth::ScriptEvent::Kind::call, 20, "Barrier");
        const auto barrier = session.scheduler->service(executor);
        expect(barrier && session.scheduler->completed_tick() == 1, "barrier service advances completed_tick after the turn");
        expect(global_number(session, 3, "Barrier_Last") == unit_word(1, 3, 2), "barrier continues past the turn's draws");
        turn(session, executor, {3}, 10);
        expect(global_number(session, 3, "Last") == unit_word(2 | domain, 3, 0), "next tick's turn starts a fresh stream");
    }
    {
        // Save and restore between the turn and the barrier continues byte-equal.
        const std::vector<std::uint64_t> ids{1, 2, 3};
        Session original = make_object_session(ids);
        Session restored = make_object_session(ids);
        turn(original, executor, ids, 10);
        const auto saved = original.scheduler->save();
        expect(saved.has_value(), "save between turn and barrier");
        if (saved) expect(restored.scheduler->load(saved.value()).has_value(), "load between turn and barrier");
        expect(restored.scheduler->state_hash().value() == original.scheduler->state_hash().value(), "restored hash equals");
        std::vector<std::string> a, b;
        turn(original, executor, ids, 11, &a);
        turn(restored, executor, ids, 11, &b);
        expect(a == b && original.scheduler->state_hash().value() == restored.scheduler->state_hash().value(),
               "a second turn of the same tick continues the stream after restore");
        for (Session* session : {&original, &restored}) {
            submit_due(*session, 2, auth::ScriptEvent::Kind::call, 20, "Barrier");
            step(*session, executor);
        }
        expect(original.commands == restored.commands && original.scheduler->state_hash().value() == restored.scheduler->state_hash().value(),
               "barrier after restore is equal");
        a.clear();
        b.clear();
        turn(original, executor, ids, 10, &a);
        turn(restored, executor, ids, 10, &b);
        expect(a == b && original.scheduler->save().value() == restored.scheduler->save().value(), "later turn is equal after restore");
    }
    {
        // Timers set in the turn and at the barrier of one tick never collide and keep their order.
        const std::string timer_source = R"LUA(
Register_Event('Log', function(tag) Test_Report('log', tag) end)
function Base_Definitions() end
function Barrier() Test_Timer(0.1, 'Log', 'barrier') end
function main()
    Test_Timer(0.1, 'Log', 'turn')
    while true do coroutine.yield(true) end
end
)LUA";
        Session session = make_object_session({7}, {{"Timer.lua", timer_source}}, {{7, "Timer.lua"}});
        turn(session, executor, {7}, 10);
        submit_due(session, 7, auth::ScriptEvent::Kind::call, 20, "Barrier");
        step(session, executor, 6);
        expect(session.commands == std::vector<std::string>{"7#0 report log turn", "7#1 report log barrier"},
               "turn and barrier timers fire in order\n" + joined(session.commands));
    }
    {
        // Events of other instances stay queued; the turn delivers only what is due for its subset.
        Session session = make_object_session({1, 2});
        submit_due(session, 1, auth::ScriptEvent::Kind::pump, 10);
        submit_due(session, 2, auth::ScriptEvent::Kind::pump, 10);
        const auto report = session.scheduler->service_turn(executor, {1});
        expect(report && report.value().commands.size() == 1 && global_number(session, 2, "Count") == 0, "unlisted instance keeps its events");
        const auto later = session.scheduler->service_turn(executor, {2});
        expect(later && later.value().commands.size() == 1 && global_number(session, 2, "Count") == 1, "its events are delivered by its own turn");
        expect(session.scheduler->service(executor).has_value() && session.scheduler->completed_tick() == 1, "barrier follows");
    }
    {
        // Faults drop only that instance's turn output; quota and error cases.
        const std::map<std::string, std::string> extra{
            {"Spin.lua", "function Base_Definitions() end\nfunction main() Test_Report('spin') Test_Timer(0.1, 'Log', 'x') while true do end end\n"},
            {"Err.lua", "function Base_Definitions() end\nfunction main() Test_Report('err') error('turn boom') end\n"},
            {"Spawn.lua", "function T() end\nfunction Base_Definitions() end\n"
                          "function main() Test_Report('spawn') for i = 1, 10 do Create_Thread('T') end coroutine.yield(true) end\n"}};
        const std::map<std::uint64_t, std::string> modules{{2, "Spin.lua"}, {4, "Err.lua"}, {5, "Spawn.lua"}};
        auth::SessionConfig config = default_config();
        config.quotas.thread_slots = 4;
        Session faulty = make_object_session({1, 2, 3, 4, 5}, extra, modules, config);
        Session clean = make_object_session({1, 3}, {}, {}, config);
        std::vector<std::string> faulty_lines, clean_lines;
        const auto report = turn(faulty, executor, {1, 2, 3, 4, 5}, 10, &faulty_lines);
        turn(clean, executor, {1, 3}, 10, &clean_lines);
        std::vector<std::string> survivors = only_issuer(faulty_lines, 1);
        for (const auto& line : only_issuer(faulty_lines, 3)) survivors.push_back(line);
        std::vector<std::string> reference = only_issuer(clean_lines, 1);
        for (const auto& line : only_issuer(clean_lines, 3)) reference.push_back(line);
        expect(survivors == reference && survivors.size() == 2, "healthy instances commit beside faulted ones");
        expect(only_issuer(faulty_lines, 2).empty() && only_issuer(faulty_lines, 4).empty() && only_issuer(faulty_lines, 5).empty(),
               "a faulted instance drops its turn output\n" + joined(faulty_lines));
        expect(report.removed_instances == std::vector<std::uint64_t>{2, 4, 5} && faulty.scheduler->instances() == std::vector<std::uint64_t>{1, 3},
               "faulted instances are removed");
        expect(any_contains(faulty_lines, std::string("diag ") + std::string(auth::codes::instruction_budget)) &&
               any_contains(faulty_lines, std::string("diag ") + std::string(auth::codes::script_error)) &&
               any_contains(faulty_lines, std::string("diag ") + std::string(auth::codes::logical_quota)),
               "budget, error and quota faults are diagnosed\n" + joined(faulty_lines));
        expect(faulty.scheduler->completed_tick() == 0, "a faulting turn does not advance completed_tick");
        const auto barrier = faulty.scheduler->service(executor);
        expect(barrier && barrier.value().commands.empty() && barrier.value().removed_instances.empty() && barrier.value().diagnostics.empty()
               && faulty.scheduler->completed_tick() == 1 && faulty.scheduler->instances() == std::vector<std::uint64_t>{1, 3},
               "the barrier after a faulting turn is unaffected and finds no leftover event");
    }
    {
        // Refusals change nothing.
        Session session = make_object_session({1, 2});
        expect(session.scheduler->create_instance(9, "Obj.lua").has_value(), "plain instance");
        const auto before = session.scheduler->state_hash().value();
        const auto refused = [&](const std::vector<std::uint64_t>& ids) {
            const auto result = session.scheduler->service_turn(executor, ids);
            return !result && result.error().code == auth::codes::invalid_request && session.scheduler->state_hash().value() == before;
        };
        expect(refused({1, 1}) && refused({1, 77}) && refused({9}) && refused({2, 9, 1}), "duplicate, unknown and non-object instances are refused");
        expect(session.scheduler->service_turn(executor, {}).has_value(), "an empty turn is a no-op");
        auth::SessionConfig clock = default_config();
        clock.galactic_clock = true;
        Session galactic = make_session({{"Obj.lua", turn_object_source}}, clock);
        expect(!galactic.scheduler->service_turn(executor, {}), "the GC clock profile has no object turn");
        // An immediate-effect session and the query clock are refused too, even for a valid object list.
        Session immediate = make_object_session({1, 2});
        expect(static_cast<bool>(immediate.scheduler->set_immediate_effect_host(std::make_shared<WalletHost>(), 4)), "immediate host on an object session");
        const auto immediate_before = immediate.scheduler->state_hash().value();
        const auto immediate_turn = immediate.scheduler->service_turn(executor, {1});
        expect(!immediate_turn && immediate_turn.error().code == auth::codes::invalid_request
                   && immediate.scheduler->state_hash().value() == immediate_before,
               "an immediate-effect session has no object turn");
        auth::SessionConfig query = default_config();
        query.query_clock = true;
        Session queried = make_object_session({1}, {}, {}, query);
        const auto query_before = queried.scheduler->state_hash().value();
        const auto query_turn = queried.scheduler->service_turn(executor, {1});
        expect(!query_turn && query_turn.error().code == auth::codes::invalid_request
                   && queried.scheduler->state_hash().value() == query_before,
               "the query clock profile has no object turn");
    }
}

// Many objects over many ticks: commands and digests equal on every executor and request order.
struct TurnOutcome {
    std::vector<std::string> digests, commands;
};

TurnOutcome run_turn_scenario(const eawr::sim::PartitionExecutor& executor, bool reverse_request) {
    std::vector<std::uint64_t> ids;
    for (std::uint64_t index = 1; index <= 12; ++index) ids.push_back(index * 5);
    Session session = make_object_session(ids);
    TurnOutcome outcome;
    for (std::uint64_t tick = 1; tick <= 24; ++tick) {
        std::vector<std::uint64_t> due;
        for (const std::uint64_t id : ids) if (tick == 1 || (tick + id) % 3 != 0) due.push_back(id);
        if (reverse_request) std::reverse(due.begin(), due.end());
        std::vector<std::string> lines;
        turn(session, executor, due, 10, &lines);
        for (const std::uint64_t id : ids) if (tick > 1 && id % 4 == tick % 4) submit_due(session, id, auth::ScriptEvent::Kind::call, 20, "Barrier");
        outcome.commands.insert(outcome.commands.end(), lines.begin(), lines.end());
        step(session, executor);
        outcome.digests.push_back(session.scheduler->state_digest());
        if (tick % 5 == 0) outcome.digests.push_back(session.scheduler->state_hash().value());
    }
    outcome.commands.insert(outcome.commands.end(), session.commands.begin(), session.commands.end());
    expect(session.diagnostics.empty(), "turn scenario diagnostics\n" + joined(session.diagnostics));
    return outcome;
}

void object_turn_workers() {
    const eawr::sim::InlineExecutor inline_executor;
    const TurnOutcome reference = run_turn_scenario(inline_executor, false);
    expect(reference.commands.size() > 150, "turn scenario issues commands");
    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const bool reverse_request : {false, true}) {
            const TurnOutcome outcome = run_turn_scenario(executor, reverse_request);
            expect(outcome.digests == reference.digests && outcome.commands == reference.commands,
                   std::to_string(workers) + " workers" + (reverse_request ? ", reversed request" : "") + ": object turns equal");
        }
    }
    const ReversePartitionExecutor reverse;
    const TurnOutcome outcome = run_turn_scenario(reverse, true);
    expect(outcome.digests == reference.digests && outcome.commands == reference.commands, "reverse partitions: object turns equal");
}

} // namespace

// ---- scheduler ----

void run_scheduler() {
    immediate_contracts();
    object_turn_contracts();
    const eawr::sim::InlineExecutor inline_executor;
    {
        const std::map<std::string, std::string> modules{{"Cache.lua", R"(
local hidden = 0
local shared = {value = 0}
function Change() hidden = hidden + 1; shared.value = hidden end
function Read() Test_Report(hidden, shared.value) end
)"}};
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(workers);
            auto original = make_session(modules);
            auto restored = make_session(modules);
            expect(original.scheduler->create_instance(1, "Cache.lua").has_value(), "sync-cache instance with closed-over state");
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            std::vector<std::string> hashes;
            for (std::uint64_t frame = 1; frame <= 16; ++frame) {
                if (frame % 3 == 1) {
                    auth::ScriptEvent event; event.key = {frame, 16, 1, 0}; event.target = 1;
                    event.kind = auth::ScriptEvent::Kind::call; event.name = "Change";
                    expect(original.scheduler->submit_event(event).has_value(), "sync mutation event accepted");
                    if (frame > 4) expect(restored.scheduler->submit_event(event).has_value(), "resumed sync mutation event accepted");
                }
                const auto encodes = original.scheduler->sync_graph_encodes();
                expect(original.scheduler->service(executor, options).has_value(), "sync original service");
                const auto digest = original.scheduler->sync_hash().value();
                if (frame == 2) expect(original.scheduler->sync_graph_encodes() == encodes, "paced quiet tick does no Lua graph serialization");
                expect(original.scheduler->sync_hash(true).value() == digest, "cached graph equals fresh full graph oracle including hidden upvalues");
                hashes.push_back(digest);
                if (frame == 4) {
                    expect(restored.scheduler->load(original.scheduler->save().value()).has_value(), "mid-window complete Lua restore");
                    expect(restored.scheduler->sync_hash().value() == digest, "Lua restored sync digest ignores local cache history");
                } else if (frame > 4) {
                    expect(restored.scheduler->service(executor, options).has_value(), "sync resumed service");
                    expect(restored.scheduler->sync_hash().value() == digest, "Lua continuation sync is identical after restore");
                    expect(restored.scheduler->state_hash().value() == original.scheduler->state_hash().value(), "full Lua archive oracle remains identical after restore");
                }
            }
            if (reference.empty()) reference = hashes; else expect(reference == hashes, "actual-state sync hashes agree on 1/2/4/8 workers");
        }
    }
    {
        auto config = default_config(); config.query_clock = true;
        auto session = make_session({{"Query.lua", "Created_At = GetCurrentTime()\nfunction Query() Test_Report(GetCurrentTime(), GetCurrentTime.Frame()) end\n"},
            {"Late.lua", "Created_At = GetCurrentTime()\n"}}, config);
        expect(session.scheduler->create_instance(1, "Query.lua").has_value(), "query clock instance");
        expect(std::get<LuaNumber>(session.scheduler->read_global(1, "Created_At").value()->data) == LuaNumber(0), "query root before any service sees zero");
        const auto before = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor) && session.scheduler->state_hash().value() == before,
            "query clock requires upstream time without advancing scheduler state");
        auth::ScriptEvent event; event.kind = auth::ScriptEvent::Kind::call;
        event.key = {1, 16, 1, 0}; event.target = 1; event.name = "Query";
        expect(session.scheduler->submit_event(event).has_value(), "sparse query event");
        auth::ServiceOptions options; options.time_tick = 9000;
        auto report = session.scheduler->service(inline_executor, options);
        expect(report && report.value().commands.size() == 1
            && text(report.value().commands.front()) == "1#0 report 300 9000"
            && session.scheduler->completed_tick() == 1, "query time advances independently of invocation count");
        expect(session.scheduler->create_instance(2, "Late.lua").has_value(), "late query root");
        expect(std::get<LuaNumber>(session.scheduler->read_global(2, "Created_At").value()->data) == LuaNumber(300), "root chunk reads last supplied query time");
        options.time_tick = 8999;
        const auto monotonic = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor, options) && session.scheduler->state_hash().value() == monotonic,
            "backwards query time refuses before changing state");
        options.time_tick = auth::max_script_tick;
        const auto valid = session.scheduler->state_hash().value();
        expect(!session.scheduler->service(inline_executor, options) && session.scheduler->state_hash().value() == valid,
            "query clock rejects out-of-format time before service");
        config.galactic_clock = true;
        expect(!auth::ScriptScheduler::create(config, {}), "clock profiles are mutually exclusive");
        config.galactic_clock = false; config.tick_duration = {1, 60};
        expect(!auth::ScriptScheduler::create(config, {}), "query clock requires logical 1/30 second frames");
    }

    // C-04 and FoC's same-pump rule for threads created during a pump.
    {
        Session session = script_session(
            "function A(...) Test_Report('A', arg.n) coroutine.yield(true) Test_Report('A again', arg.n) coroutine.yield(true) end\n"
            "function B() Test_Report('B') return true end\n"
            "function C() Test_Report('C') error('C fails') end\n"
            "function Spawner() Test_Report('spawn', Create_Thread('Late')) end\n"
            "function Late() Test_Report('late in first pump', GetThreadID()) end\n"
            "Test_Report(Create_Thread('A', 7, 8), Create_Thread('B'), Create_Thread('C'), Create_Thread('Spawner'), GetThreadID())\n");
        expect(run_script(session, 2).empty(), "C-04 create");
        expect(session.commands == std::vector<std::string>{
                   "1#0 report 0 1 2 3 -1", "1#1 report A 1", "1#2 report B", "1#3 report spawn 4",
                   "1#4 report late in first pump 4", "1#5 report A again 1", "1#6 report B"},
               "C-04 thread order\n" + joined(session.commands));
        expect(any_contains(session.diagnostics, "C fails"), "C-04 error diagnostic");
    }

    // L-23 retail rule (keep_slot_after_true_return): the topmost value decides,
    // and a slot that returned true starts again from the top without its parameter.
    {
        Session session = script_session(
            "runs = 0\n"
            "function Restart(...) runs = runs + 1 Test_Report('restart', runs, arg.n) return true end\n"
            "function LastTrue() Test_Report('last true') coroutine.yield(false, true) Test_Report('kept') coroutine.yield(false) end\n"
            "function LastFalse() Test_Report('last false') coroutine.yield(true, false) Test_Report('never') end\n"
            "function NoValue() Test_Report('no value') coroutine.yield() Test_Report('never') end\n"
            "Create_Thread('Restart', 1) Create_Thread('LastTrue') Create_Thread('LastFalse') Create_Thread('NoValue')\n");
        expect(run_script(session, 3).empty(), "L-23 create");
        expect(session.commands == std::vector<std::string>{
                   "1#0 report restart 1 1", "1#1 report last true", "1#2 report last false", "1#3 report no value",
                   "1#4 report restart 2 0", "1#5 report kept", "1#6 report restart 3 0"},
               "L-23 retail pump rule\n" + joined(session.commands));
    }

    // require sets _REQUIREDNAME while the module runs and restores it.
    {
        Session session = make_session({{"Main.lua", "_REQUIREDNAME = 'outer'\nTest_Report(require('Mod'), _REQUIREDNAME)\n"},
                                        {"Library/Mod.lua", "Test_Report(_REQUIREDNAME)\nreturn 5\n"}});
        expect(run_script(session, 1).empty(), "require create");
        expect(session.commands == std::vector<std::string>{"1#0 report Mod", "1#1 report 5 outer"},
               "_REQUIREDNAME\n" + joined(session.commands));
    }

    // C-05.
    {
        Session session = script_session(
            "function A() Test_Report('kill self', Create_Thread.Kill(GetThreadID())) Create_Thread.Kill_All() "
            "coroutine.yield(true) Test_Report('A still live') coroutine.yield(true) end\n"
            "function B() Test_Report('B ran') coroutine.yield(true) end\n"
            "Create_Thread('A') Create_Thread('B') Create_Thread('B')\n");
        expect(run_script(session, 2).empty(), "C-05 create");
        expect(session.commands == std::vector<std::string>{"1#0 report kill self", "1#1 report A still live"},
               "C-05\n" + joined(session.commands));
    }

    // C-06: independent callback and parameter FIFOs, skew and reset.
    {
        Session session = script_session(
            "function Red() end\n"
            "function Blue() end\n"
            "local function count(...) return arg.n end\n"
            "function Reader()\n"
            "  coroutine.yield(true)\n"
            "  local a, b = GetEvent(), GetEvent()\n"
            "  local p, q = GetEvent.Params(), GetEvent.Params()\n"
            "  Test_Report(a == Red, b == Blue, p[1], q[1], count(GetEvent()), count(GetEvent.Params()))\n"
            "  coroutine.yield(true)\n"
            "  local skew = GetEvent()\n"
            "  Test_Report('skew', skew == Red, count(GetEvent.Params()))\n"
            "  GetEvent.Reset()\n"
            "  Test_Report('after reset', count(GetEvent()), count(GetEvent.Params()))\n"
            "  coroutine.yield(true)\n"
            "end\n"
            "Create_Thread('Reader')\n"
            "Test_Signal(1, 0, 'Red', {10}) Test_Signal(1, 0, 'Blue', {20})\n"
            "function Later() Test_Signal(1, 0, 'Red') Test_Signal(1, 0, 'Blue', {30}) end\n"
            "Register_Event('later', Later)\n");
        expect(run_script(session, 1).empty(), "C-06 create");
        const eawr::sim::InlineExecutor executor;
        expect(session.scheduler->submit_event(dispatch_event({2, auth::first_simulation_producer, 0, 0}, 1, "later", {})).has_value(),
               "C-06 submit");
        step(session, executor, 3);
        expect(session.commands == std::vector<std::string>{
                   "1#0 report true true 10 20 0 0", "1#1 report skew true 0", "1#2 report after reset 0 0"},
               "C-06\n" + joined(session.commands));
    }

    // C-07: Register_Event mutation restart.
    {
        Session session = script_session(
            "calls = ''\n"
            "cancelled = false\n"
            "function A() calls = calls .. 'A' end\n"
            "function B() calls = calls .. 'B' if not cancelled then cancelled = true Cancel_Event('range', C) end end\n"
            "function C() calls = calls .. 'C' end\n"
            "function D() calls = calls .. 'D' end\n"
            "function Report() Test_Report(calls) end\n"
            "Register_Event('range', A) Register_Event('range', B) Register_Event('range', C) Register_Event('range', D)\n"
            "Register_Event('report', Report)\n");
        expect(run_script(session, 0).empty(), "C-07 create");
        expect(session.scheduler->submit_event(dispatch_event({1, 20, 5, 0}, 1, "range", {})).has_value(), "C-07 submit");
        expect(session.scheduler->submit_event(dispatch_event({1, 20, 6, 0}, 1, "report", {})).has_value(), "C-07 submit report");
        step(session, inline_executor);
        expect(session.commands == std::vector<std::string>{"1#0 report ABBD"}, "C-07\n" + joined(session.commands));
    }

    // Canonical event order and timer ties: (tick, producer, entity, sequence);
    // timers by (deadline, instance, registration).
    {
        Session session = make_session({
            {"One.lua", "function Log(tag) Test_Report(tag, GetCurrentTime.Frame()) end\nRegister_Event('log', Log)\n"
                        "Test_Timer(0.2, 'log', 'one-a') Test_Timer(0.1, 'log', 'one-b') Test_Timer(0.2, 'log', 'one-c')\n"},
            {"Two.lua", "function Log(tag) Test_Report(tag, GetCurrentTime.Frame()) end\nRegister_Event('log', Log)\n"
                        "Test_Timer(0.2, 'log', 'two-a')\n"},
        });
        expect(session.scheduler->create_instance(2, "Two.lua").has_value(), "timers create 2");
        expect(session.scheduler->create_instance(1, "One.lua").has_value(), "timers create 1");
        using auth::EventKey;
        for (const auto& [key, tag] : std::vector<std::pair<EventKey, std::string>>{
                 {{3, 40, 1, 0}, "p40"}, {{3, 17, 9, 1}, "p17-e9-s1"}, {{3, 17, 9, 0}, "p17-e9-s0"}, {{3, 17, 2, 5}, "p17-e2"},
                 {{2, 99, 0, 0}, "early"}}) {
            expect(session.scheduler->submit_event(dispatch_event(key, 1, "log", {auth::Value::text(tag)})).has_value(),
                   "submit " + tag);
        }
        const eawr::sim::InlineExecutor executor;
        step(session, executor, 8);
        const std::vector<std::string> expected{
            "1#0 report early 1",  "1#1 report p17-e2 2", "1#2 report p17-e9-s0 2", "1#3 report p17-e9-s1 2",
            "1#4 report p40 2",    "1#5 report one-b 4",  "1#6 report one-a 7",     "1#7 report one-c 7",
            "2#0 report two-a 7"};
        expect(session.commands == expected, "event and timer order\n" + joined(session.commands));
    }

    // GetCurrentTime and thread values.
    expect_runs(
        "Test_Report(GetCurrentTime(), GetCurrentTime.Frame(), ThreadValue('x'))\n"
        "ThreadValue.Set('x', 1)\n"
        "Test_Report(ThreadValue('x'))\n",
        "time and thread values outside threads", {"1#0 report 0 0", "1#1 report"});
    {
        Session session = script_session(
            "function T() ThreadValue.Set('x', 5) while true do Test_Report(GetCurrentTime.Frame(), ThreadValue('x')) "
            "coroutine.yield(true) end end\nCreate_Thread('T')\n");
        const eawr::sim::InlineExecutor executor;
        expect(run_script(session, 31).empty(), "time create");
        expect(session.commands.size() == 31 && session.commands[30] == "1#30 report 30 5", "frame 30");
    }
    expect_runs(
        "function T() while true do Test_Report(GetCurrentTime()) coroutine.yield(true) end end\n"
        "Create_Thread('T')\n",
        "time at tick 0");

    // Transactions: a failed resume publishes nothing and consumes no draws.
    {
        Session session = script_session(
            "function Bad() Test_Report('bad', Test_Random()) error('boom') end\n"
            "function Good() Test_Report('good', Test_Random()) end\n"
            "Create_Thread('Bad') Create_Thread('Good')\n");
        expect(run_script(session, 1).empty(), "rollback create");
        expect(session.commands.size() == 1 && contains(session.commands[0], "1#0 report good"), "rollback commands\n" + joined(session.commands));
        Session reference = script_session("function Good() Test_Report('good', Test_Random()) end\nCreate_Thread('Good')\n");
        expect(run_script(reference, 1).empty(), "rollback reference");
        expect(session.commands == reference.commands, "rolled-back draw is reused\n" + joined(reference.commands));
    }

    // Quota failure: the instance is removed and none of its tick output is
    // published; other instances are unaffected.
    {
        Session session = make_session({
            {"Busy.lua", "function Fine() Test_Report('before') end\nfunction Loop() while true do end end\n"
                         "Create_Thread('Fine') Create_Thread('Loop')\n"},
            {"Calm.lua", "function Tick() while true do Test_Report('calm') coroutine.yield(true) end end\nCreate_Thread('Tick')\n"},
        });
        expect(session.scheduler->create_instance(1, "Busy.lua").has_value(), "busy create");
        expect(session.scheduler->create_instance(2, "Calm.lua").has_value(), "calm create");
        const eawr::sim::InlineExecutor executor;
        step(session, executor, 2);
        expect(session.commands == std::vector<std::string>{"2#0 report calm", "2#1 report calm"}, "quota rollback\n" + joined(session.commands));
        expect(session.removed == std::vector<std::uint64_t>{1}, "faulted instance removed");
        expect(any_contains(session.diagnostics, std::string(auth::codes::instruction_budget)), "budget diagnostic");
        expect(session.scheduler->instances() == std::vector<std::uint64_t>{2}, "only the calm instance remains");
    }
    {
        auth::SessionConfig config = default_config();
        config.quotas.thread_slots = 4;
        Session session = script_session("function T() end\nfunction Spawn() for i = 1, 10 do Create_Thread('T') end end\n"
                                         "Create_Thread('Spawn')\n", config);
        expect(run_script(session, 1).empty(), "slot quota create");
        expect(session.removed == std::vector<std::uint64_t>{1} && any_contains(session.diagnostics, std::string(auth::codes::logical_quota)),
               "thread slot quota\n" + joined(session.diagnostics));
    }

    // _ScriptExit keeps the tick's output and ends the instance.
    {
        Session session = script_session(
            "function T() Test_Report('exiting') _ScriptExit() coroutine.yield(false) end\n"
            "function U() Test_Report('never') end\nCreate_Thread('T') Create_Thread('U')\n");
        expect(run_script(session, 2).empty(), "exit create");
        expect(session.commands == std::vector<std::string>{"1#0 report exiting"} && session.removed == std::vector<std::uint64_t>{1},
               "script exit\n" + joined(session.commands));
    }

    // Free_Random has no authoritative form.
    {
        Session session = script_session("");
        auto rejected = session.scheduler->register_binding(
            "GameRandom.Free_Random", [](auth::BindingContext&, const auth::ValueList&) {
                return eawr::core::Result<auth::ValueList>::success({});
            });
        expect(!rejected && rejected.error().code == auth::codes::forbidden_capability, "Free_Random is rejected");
    }
}

// ---- workers ----

struct Outcome {
    std::vector<std::string> digests;
    std::vector<std::string> commands;
};

Outcome run_scenario(const eawr::sim::PartitionExecutor& executor, bool reverse_creation, int ticks, int instances) {
    Session session = make_session({{"Library/Library.lua", auth::test::scenario_library}, {"Ai.lua", auth::test::scenario_ai}});
    std::vector<std::uint64_t> ids;
    for (int index = 1; index <= instances; ++index) ids.push_back(static_cast<std::uint64_t>(index) * 3);
    if (reverse_creation) std::reverse(ids.begin(), ids.end());
    for (const std::uint64_t id : ids) expect(session.scheduler->create_instance(id, "Ai.lua").has_value(), "scenario create");
    Outcome outcome;
    std::uint64_t sequence = 0;
    for (int tick = 1; tick <= ticks; ++tick) {
        for (const std::uint64_t id : ids) {
            if (tick % 7 != static_cast<int>(id % 7)) continue;
            auth::ValueList arguments{auth::Value{auth::Handle{7, id % 24}}, auth::Value::number(integer(tick))};
            expect(session.scheduler
                       ->submit_event(dispatch_event({static_cast<std::uint64_t>(tick), 30, id, sequence++}, id, "attacked", arguments))
                       .has_value(),
                   "scenario submit");
        }
        step(session, executor);
        outcome.digests.push_back(session.scheduler->state_digest());
    }
    outcome.commands = session.commands;
    expect(session.diagnostics.empty(), "scenario diagnostics\n" + joined(session.diagnostics));
    return outcome;
}

void run_workers() {
    object_turn_workers();
    constexpr int ticks = 90;
    constexpr int instances = 24;
    const eawr::sim::InlineExecutor inline_executor;
    const Outcome reference = run_scenario(inline_executor, false, ticks, instances);
    expect(reference.commands.size() > 100, "scenario issues commands");
    std::cout << "scenario: " << reference.commands.size() << " commands, final digest " << reference.digests.back() << '\n';
    for (const std::size_t workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const Outcome outcome = run_scenario(executor, false, ticks, instances);
        expect(outcome.digests == reference.digests, std::to_string(workers) + " workers: per-tick digests");
        expect(outcome.commands == reference.commands, std::to_string(workers) + " workers: commands");
    }
    // Storage perturbation: instance creation order does not change results.
    {
        const eawr::platform::ThreadWorkerAdapter executor(4);
        const Outcome outcome = run_scenario(executor, true, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands, "reversed creation order");
    }
    // Allocation perturbation: shifted heap addresses and a different
    // collection schedule are unobservable.
    {
        std::vector<std::unique_ptr<char[]>> ballast;
        for (int index = 0; index < 4096; ++index) ballast.push_back(std::make_unique<char[]>(static_cast<std::size_t>(17 + (index * 37) % 509)));
        for (std::size_t index = 0; index < ballast.size(); index += 3) ballast[index].reset();
        const eawr::platform::ThreadWorkerAdapter executor(2);
        const Outcome outcome = run_scenario(executor, false, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands, "allocation perturbation");
    }
    // Locale perturbation.
    for (const char* locale : {"", "de_DE.UTF-8", "German_Germany.1252", "tr_TR.UTF-8"}) {
        if (std::setlocale(LC_ALL, locale) == nullptr) continue;
        const Outcome outcome = run_scenario(inline_executor, false, ticks, instances);
        expect(outcome.digests == reference.digests && outcome.commands == reference.commands,
               std::string("locale '") + locale + "'");
        std::setlocale(LC_ALL, "C");
    }
}


} // namespace lua_sandbox_test_support
