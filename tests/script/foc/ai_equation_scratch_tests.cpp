#include "../../support/check.hpp"
#include "ai_equations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
thread_local bool count_allocations{};
thread_local std::size_t allocations{};
}

#if defined(_MSC_VER)
#define EAWR_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define EAWR_TEST_NOINLINE __attribute__((noinline))
#else
#define EAWR_TEST_NOINLINE
#endif
EAWR_TEST_NOINLINE void* operator new(const std::size_t size) {
    if (count_allocations) ++allocations;
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
EAWR_TEST_NOINLINE void operator delete(void* const memory) noexcept { std::free(memory); }
EAWR_TEST_NOINLINE void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
#undef EAWR_TEST_NOINLINE

namespace {
namespace ai = eawr::script::foc::ai;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool value, const std::string_view message) {
    if (!value) local_checker.check(value, message, std::cerr, "FAILED: ");
}

ai::Equation equation(const std::string& payload) {
    const auto converters = [](std::string_view, std::string_view) -> std::optional<ai::Real> { return std::nullopt; };
    auto parsed = ai::parse_equation("scratch", "13 + Variable_Target.Health{Parameter_Type=\""
        + payload + "\"} + (1 # 9)", converters);
    if (!parsed) throw std::runtime_error("synthetic equation did not parse");
    return std::move(parsed).value();
}

struct Resolver : ai::LookupResolver {
    explicit Resolver(std::string expected) : payload(std::move(expected)) {}
    std::string payload;
    std::size_t calls{};
    bool valid{true};
    bool refuse{};
    bool throw_once{};
    std::optional<ai::Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>& bindings) override {
        ++calls;
        if (throw_once) { throw_once = false; throw std::runtime_error("resolver failure"); }
        if (!lookup.tokens.empty() && lookup.tokens.front() == "GAME" && bindings.empty()) return ai::real(2);
        const auto* text = bindings.size() == 1 ? std::get_if<std::string>(&bindings[0].value) : nullptr;
        valid = valid && text && *text == payload && bindings[0].parameter == "PARAMETER_TYPE";
        return valid && !refuse ? std::optional<ai::Real>{ai::real(2)} : std::nullopt;
    }
};

bool run_checked(const ai::Equation& program, ai::LookupResolver& resolver, const std::uint32_t seed) {
    ai::AiRandom random, reference;
    random.set_seed(seed); reference.set_seed(seed);
    const auto result = ai::run(program, resolver, random);
    const auto expected = ai::real(15) + reference.uniform(ai::real(1), ai::real(9));
    return result && result->repr == expected.repr && random.next() == reference.next();
}

void allocation_and_failure_contract() {
    const std::string payload(96, 'X');
    auto program = equation(payload);
    Resolver resolver(payload);
    allocations = 0; count_allocations = true;
    const bool cold = run_checked(program, resolver, 7);
    count_allocations = false;
    expect(cold && allocations > 0, "cold evaluation prepares storage and the allocation observer is active");

    const auto calls = resolver.calls;
    allocations = 0; count_allocations = true;
    bool exact = true;
    for (std::uint32_t seed = 0; seed < 256; ++seed)
        exact = run_checked(program, resolver, seed) && exact;
    count_allocations = false;
    expect(exact && resolver.valid && resolver.calls == calls + 256,
        "warm evaluation retains exact result/RNG bytes and still invokes every lookup");
    expect(allocations == 0, "a warmed equation and its long text binding allocate no heap storage");

    const auto converters = [](std::string_view, std::string_view) -> std::optional<ai::Real> { return std::nullopt; };
    auto unbound = ai::parse_equation("unbound", "13 + Game.Age + (1 # 9)", converters);
    expect(bool(unbound), "equation without bindings parses");
    if (unbound) {
        expect(run_checked(unbound.value(), resolver, 8), "a second binding shape warms independently");
        allocations = 0; count_allocations = true;
        bool mixed = true;
        for (std::uint32_t seed = 0; seed < 32; ++seed) {
            mixed = run_checked(program, resolver, seed) && mixed;
            mixed = run_checked(unbound.value(), resolver, seed) && mixed;
        }
        count_allocations = false;
        expect(mixed && allocations == 0, "interleaved parameter counts retain their own long-string storage");
    }

    resolver.refuse = true;
    ai::AiRandom random;
    expect(!ai::run(program, resolver, random), "a refused lookup still fails the equation");
    resolver.refuse = false; resolver.throw_once = true;
    bool caught = false;
    try { static_cast<void>(ai::run(program, resolver, random)); }
    catch (const std::runtime_error&) { caught = true; }
    expect(caught, "resolver exceptions retain their existing propagation");
    allocations = 0; count_allocations = true;
    const bool after_failure = run_checked(program, resolver, 12);
    count_allocations = false;
    expect(after_failure && allocations == 0, "refusal and exception both release the active scratch frame");

    ai::Equation large;
    large.name = "large";
    for (std::size_t index = 0; index < 800; ++index) {
        ai::Node node; node.kind = ai::Node::Kind::constant; node.value = ai::real(1);
        large.program.push_back(std::move(node));
    }
    for (std::size_t index = 1; index < 800; ++index) {
        ai::Node node; node.kind = ai::Node::Kind::add;
        large.program.push_back(std::move(node));
    }
    expect(ai::run(large, resolver, random) == ai::real(800), "stack storage follows authored program depth");
    allocations = 0; count_allocations = true;
    const auto large_again = ai::run(large, resolver, random);
    const bool smaller_again = run_checked(program, resolver, 13);
    count_allocations = false;
    expect(large_again == ai::real(800) && smaller_again && allocations == 0,
        "large and small programs reuse their high-water storage without a fixed program bound");

    for (auto& node : program.program) if (node.kind == ai::Node::Kind::text) node.text.assign(96, 'Y');
    resolver.payload.assign(96, 'Y');
    expect(run_checked(program, resolver, 9), "a changed source literal is read again rather than cached as a result");
}

struct RecursiveResolver final : ai::LookupResolver {
    const std::vector<ai::Equation>& programs;
    const std::vector<std::string>& payloads;
    std::size_t depth{};
    bool valid{true};
    bool refuse_depth_three{};
    bool throw_depth_three{};
    RecursiveResolver(const std::vector<ai::Equation>& equations, const std::vector<std::string>& strings)
        : programs(equations), payloads(strings) {}
    std::optional<ai::Real> resolve(const ai::Lookup&, const std::vector<ai::Binding>& bindings) override {
        const auto level = depth;
        if (level == 2 && throw_depth_three) throw std::runtime_error("nested resolver failure");
        if (level == 2 && refuse_depth_three) return std::nullopt;
        const auto intact = [&] {
            const auto* text = bindings.size() == 1 ? std::get_if<std::string>(&bindings[0].value) : nullptr;
            return text && *text == payloads[level] && bindings[0].parameter == "PARAMETER_TYPE";
        };
        valid = valid && intact();
        if (level + 1 < programs.size()) {
            ++depth;
            struct RestoreDepth {
                std::size_t& value;
                ~RestoreDepth() { --value; }
            } restore{depth};
            valid = run_checked(programs[depth], *this, static_cast<std::uint32_t>(depth + 100)) && valid;
        }
        valid = valid && intact();
        return valid ? std::optional<ai::Real>{ai::real(2)} : std::nullopt;
    }
};

void recursive_and_thread_contract() {
    std::vector<ai::Equation> programs;
    std::vector<std::string> payloads;
    for (std::size_t level = 0; level < 12; ++level) {
        payloads.emplace_back(96, static_cast<char>('A' + level));
        programs.push_back(equation(payloads.back()));
    }
    RecursiveResolver resolver(programs, payloads);
    expect(run_checked(programs.front(), resolver, 33) && resolver.valid,
        "recursive equations preserve parent operands and long parameter bindings while scratch grows");
    allocations = 0; count_allocations = true;
    bool exact = true;
    for (std::uint32_t seed = 0; seed < 32; ++seed)
        exact = run_checked(programs.front(), resolver, seed) && exact;
    count_allocations = false;
    expect(exact && resolver.valid && allocations == 0, "warmed recursive calls reuse separate scratch frames");

    resolver.refuse_depth_three = true;
    ai::AiRandom random;
    expect(!ai::run(programs.front(), resolver, random) && resolver.depth == 0,
        "a refusal at depth three unwinds all parent evaluations");
    resolver.refuse_depth_three = false;
    resolver.valid = true;
    resolver.throw_depth_three = true;
    bool caught = false;
    try { static_cast<void>(ai::run(programs.front(), resolver, random)); }
    catch (const std::runtime_error&) { caught = true; }
    expect(caught && resolver.depth == 0, "a depth-three exception unwinds all parent evaluations");
    resolver.throw_depth_three = false;
    resolver.valid = true;
    allocations = 0; count_allocations = true;
    const bool recovered = run_checked(programs.front(), resolver, 34);
    count_allocations = false;
    expect(recovered && resolver.valid && allocations == 0,
        "nested refusal and exception release every scratch lease and retain warmed storage");

    struct Result { bool exact{}; std::size_t allocations{}; };
    std::array<Result, 8> results{};
    std::array<std::thread, 8> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) workers[index] = std::thread([&, index] {
        RecursiveResolver local(programs, payloads);
        bool okay = run_checked(programs.front(), local, static_cast<std::uint32_t>(index));
        allocations = 0; count_allocations = true;
        for (std::uint32_t repeat = 0; repeat < 32; ++repeat)
            okay = run_checked(programs.front(), local, repeat + static_cast<std::uint32_t>(index)) && okay;
        count_allocations = false;
        results[index] = {okay && local.valid, allocations};
    });
    for (auto& worker : workers) worker.join();
    for (const auto& result : results)
        expect(result.exact && result.allocations == 0, "concurrent AI callers own independent recursive scratch");
}

void multiple_text_contract() {
    const std::string first(96, 'P');
    const std::string second(161, 'Q');
    const auto converters = [](std::string_view, std::string_view) -> std::optional<ai::Real> { return std::nullopt; };
    auto parsed = ai::parse_equation("two-texts", "13 + Variable_Target.Health{Parameter_Type=\""
        + first + "\", Parameter_Category=\"" + second + "\"} + (1 # 9)", converters);
    expect(bool(parsed), "two differently sized text literals parse");
    if (!parsed) return;
    struct TextResolver final : ai::LookupResolver {
        const std::string& first;
        const std::string& second;
        bool valid{true};
        TextResolver(const std::string& a, const std::string& b) : first(a), second(b) {}
        std::optional<ai::Real> resolve(const ai::Lookup&, const std::vector<ai::Binding>& bindings) override {
            const auto* a = bindings.size() == 2 ? std::get_if<std::string>(&bindings[0].value) : nullptr;
            const auto* b = bindings.size() == 2 ? std::get_if<std::string>(&bindings[1].value) : nullptr;
            valid = valid && a && b && *a == first && *b == second
                && bindings[0].parameter == "PARAMETER_TYPE" && bindings[1].parameter == "PARAMETER_CATEGORY";
            return valid ? std::optional<ai::Real>{ai::real(2)} : std::nullopt;
        }
    } resolver(first, second);
    allocations = 0; count_allocations = true;
    const bool cold = run_checked(parsed.value(), resolver, 40);
    count_allocations = false;
    expect(cold && resolver.valid && allocations > 0, "cold text operands retain their distinct index mapping");
    allocations = 0; count_allocations = true;
    bool exact = true;
    for (std::uint32_t seed = 0; seed < 32; ++seed)
        exact = run_checked(parsed.value(), resolver, seed) && exact;
    count_allocations = false;
    expect(exact && resolver.valid && allocations == 0,
        "warmed differently sized text operands preserve order and allocate no heap storage");
}

void literal_value_contract() {
    const std::string payload(96, 'M');
    const auto converters = [](std::string_view, std::string_view) -> std::optional<ai::Real> { return std::nullopt; };
    auto parsed = ai::parse_equation("literal", "Variable_Target.Health{Parameter_Type=\"" + payload
        + "\", Parameter_Category=Game.Age}", converters);
    expect(bool(parsed), "literal lifetime equation parses");
    if (!parsed) return;
    struct SnapshotResolver final : ai::LookupResolver {
        ai::Equation& program;
        const std::string& expected;
        bool valid{};
        SnapshotResolver(ai::Equation& source, const std::string& literal) : program(source), expected(literal) {}
        std::optional<ai::Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>& bindings) override {
            if (!lookup.tokens.empty() && lookup.tokens.front() == "GAME") {
                for (auto& node : program.program) if (node.kind == ai::Node::Kind::text) node.text.assign(96, 'N');
                return ai::real(7);
            }
            const auto* text = bindings.size() == 2 ? std::get_if<std::string>(&bindings[0].value) : nullptr;
            const auto* number = bindings.size() == 2 ? std::get_if<ai::Real>(&bindings[1].value) : nullptr;
            valid = text && *text == expected && number && *number == ai::real(7);
            return valid ? std::optional<ai::Real>{ai::real(2)} : std::nullopt;
        }
    } resolver(parsed.value(), payload);
    ai::AiRandom random;
    expect(ai::run(parsed.value(), resolver, random) == ai::real(2) && resolver.valid,
        "literal values are frozen when pushed, before a later resolver callback can alter their source");
}
} // namespace

int main() {
    allocation_and_failure_contract();
    multiple_text_contract();
    recursive_and_thread_contract();
    literal_value_contract();
    return failures == 0 ? 0 : 1;
}
