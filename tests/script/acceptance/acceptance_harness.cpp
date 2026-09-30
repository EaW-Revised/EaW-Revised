#include "eawr/script/pglua.hpp"
#include "eawr/script/script_host.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using eawr::script::ScriptValue;
using eawr::script::ValueList;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct TempTree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-p007-acceptance-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree() { std::filesystem::create_directories(root); }
    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void i32(std::vector<std::byte>& output, std::int32_t value) {
    u32(output, static_cast<std::uint32_t>(value));
}

void text_string(std::vector<std::byte>& output, std::string_view value) {
    if (value.empty()) {
        u32(output, 0);
        return;
    }
    u32(output, static_cast<std::uint32_t>(value.size() + 1));
    for (const auto character : value) output.push_back(static_cast<std::byte>(character));
    output.push_back(std::byte{0});
}

constexpr std::array<std::byte, 22> pglua_header{
    std::byte{0x1b}, std::byte{0x4c}, std::byte{0x75}, std::byte{0x70},
    std::byte{0x51}, std::byte{0x01}, std::byte{0x04}, std::byte{0x04},
    std::byte{0x04}, std::byte{0x06}, std::byte{0x08}, std::byte{0x09},
    std::byte{0x09}, std::byte{0x08}, std::byte{0xb6}, std::byte{0x09},
    std::byte{0x93}, std::byte{0x68}, std::byte{0xe7}, std::byte{0xf5},
    std::byte{0x7d}, std::byte{0x41},
};

constexpr std::uint32_t return_word = 0x0000801bU;

void prototype(std::vector<std::byte>& output, std::int32_t persistence,
    bool nested, std::uint32_t instruction = return_word) {
    text_string(output, "=(none)");
    i32(output, 0);
    i32(output, persistence);
    output.insert(output.end(), {
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2},
    });
    i32(output, 0); // source-line vector
    i32(output, 0); // local-variable vector
    i32(output, 0); // upvalue-name vector
    i32(output, 0); // constant vector
    i32(output, nested ? 1 : 0);
    if (nested) prototype(output, persistence + 1, false);
    i32(output, 2);
    u32(output, instruction);
    u32(output, return_word);
}

std::vector<std::byte> valid_chunk(bool nested = false) {
    std::vector<std::byte> output(pglua_header.begin(), pglua_header.end());
    prototype(output, 1, nested);
    return output;
}

std::vector<std::byte> tagged_chunk(std::byte tag) {
    auto output = valid_chunk();
    // The root has no variable-length fields in this fixture.  Insert one
    // constant tag after the constant-count field and adjust that count.
    constexpr std::size_t constant_count_offset = 58;
    output[constant_count_offset] = std::byte{1};
    output.insert(output.begin() + 62, tag);
    return output;
}

const float* number(const ScriptValue& value) {
    return std::get_if<float>(&value.storage());
}

const bool* boolean(const ScriptValue& value) {
    return std::get_if<bool>(&value.storage());
}

const ScriptValue::Sequence* sequence(const ScriptValue& value) {
    const auto* storage = std::get_if<ScriptValue::SequenceStorage>(&value.storage());
    return storage != nullptr && *storage ? storage->get() : nullptr;
}

void test_pglua() {
    const auto valid = valid_chunk();
    auto converted = eawr::script::convert_pglua(valid, "synthetic/valid.pglua");
    expect(converted.has_value(), "independent valid PGLua reaches the public converter");
    if (converted) {
        expect(converted.value().bytes.size() == 74U + (sizeof(std::size_t) - 4U),
            "converter emits native-width standard Lua bytes");
        expect(converted.value().prototypes.size() == 1 &&
            converted.value().prototypes.front().persistence_id == 1 &&
            converted.value().prototypes.front().instruction_count == 2,
            "converter returns prototype identity and PC metadata");
        expect(converted.value().bytes.size() >= 4 &&
            converted.value().bytes[0] == std::byte{0x1b} &&
            converted.value().bytes[1] == std::byte{0x4c} &&
            converted.value().bytes[2] == std::byte{0x75} &&
            converted.value().bytes[3] == std::byte{0x61},
            "converter changes only the standard Lua signature at the boundary");
    }

    auto nested = eawr::script::convert_pglua(valid_chunk(true), "synthetic/nested.pglua");
    expect(nested.has_value() && nested.value().prototypes.size() == 2,
        "independent nested prototype survives conversion");

    auto unsupported = valid_chunk();
    const auto supported_but_unimplemented_jump = (131071U << 6U) | 28U;
    for (unsigned shift = 0; shift != 32; shift += 8) {
        unsupported[70 + shift / 8] = static_cast<std::byte>(
            (supported_but_unimplemented_jump >> shift) & 0xffU);
    }
    auto unsupported_result = eawr::script::convert_pglua(
        unsupported, "synthetic/unsupported.pglua");
    expect(unsupported_result.has_value() &&
        unsupported_result.value().unsupported_execution.size() == 1 &&
        unsupported_result.value().unsupported_execution.front().pc == 0,
        "retail-only unsupported opcode is reported with source PC");

    const std::array<std::pair<std::size_t, std::byte>, 5> mutations{{
        {3, std::byte{0x61}}, {4, std::byte{0x52}}, {5, std::byte{0}},
        {7, std::byte{8}}, {13, std::byte{4}},
    }};
    for (const auto [offset, replacement] : mutations) {
        auto malformed = valid;
        malformed[offset] = replacement;
        auto result = eawr::script::convert_pglua(malformed, "synthetic/header.pglua");
        expect(!result && result.error().code == eawr::script::diagnostic_codes::load_parse,
            "header mutation is rejected before VM execution");
    }
    auto trailing = valid;
    trailing.push_back(std::byte{0});
    auto trailing_result = eawr::script::convert_pglua(trailing, "synthetic/trailing.pglua");
    expect(!trailing_result && trailing_result.error().code == eawr::script::diagnostic_codes::load_parse,
        "trailing bytes are rejected");

    auto unknown = eawr::script::convert_pglua(
        tagged_chunk(std::byte{2}), "synthetic/unknown-tag.pglua");
    expect(!unknown && unknown.error().code == eawr::script::diagnostic_codes::load_parse,
        "unknown constant tags are rejected");
    auto bad_id = valid;
    bad_id[38] = std::byte{0};
    bad_id[39] = std::byte{0};
    bad_id[40] = std::byte{0};
    bad_id[41] = std::byte{0};
    auto bad_id_result = eawr::script::convert_pglua(bad_id, "synthetic/id.pglua");
    expect(!bad_id_result && bad_id_result.error().code == eawr::script::diagnostic_codes::load_parse,
        "nonpositive persistence ID is rejected");
    auto bad_opcode = valid;
    bad_opcode[70] = std::byte{35};
    auto bad_opcode_result = eawr::script::convert_pglua(
        bad_opcode, "synthetic/opcode.pglua");
    expect(!bad_opcode_result && bad_opcode_result.error().code ==
        eawr::script::diagnostic_codes::unsupported_feature,
        "unknown opcode is rejected");
    auto metadata = eawr::script::convert_pglua(
        bad_opcode, "logical/metadata-source.pglua");
    expect(!metadata && metadata.error().logical_path &&
        *metadata.error().logical_path == "logical/metadata-source.pglua",
        "parse diagnostics retain logical source metadata");
}

void test_host() {
    TempTree tree;
    write_text(tree.root / "Scripts" / "main.lua", R"lua(
good_hits = 0
bad_hits = 0
error_hits = 0
event_a, event_b, event_c, event_d = 0, 0, 0, 0
event_mutated = false
function Good() return require("good") end
function Bad() return require("bad") end
function Broken() return require("broken") end
function Missing() return require("missing") end
function HitCounts() return {good_hits, bad_hits, error_hits} end
function CallEcho() local a,b=Function_Call(Echo, 3, "x"); return {a,b} end
function CallForeign() return Function_Call(Foreign) end
function Yielding() coroutine.yield(true); coroutine.yield(false) end
function StartYield() return Create_Thread("Yielding") end
function A() event_a=event_a+1 end
function B() event_b=event_b+1; if not event_mutated then event_mutated=true; Cancel_Event("scan", C) end end
function C() event_c=event_c+1 end
function D() event_d=event_d+1 end
function SetupEvents() Register_Event("scan", A); Register_Event("scan", B); Register_Event("scan", C); Register_Event("scan", D) end
function EventCounts() return {event_a,event_b,event_c,event_d} end
function Libraries() return {_G~=nil,_VERSION=="Lua 5.0.2",_LOADED~=nil,coroutine~=nil,require~=nil,loadfile~=nil,dofile~=nil,loadstring~=nil,string~=nil,table~=nil,package==nil,math==nil,io==nil,os==nil,debug==nil} end
function MissingApi() Lock_Controls(1) end
function DestroyDuringCall() DestroyNow() end
)lua");
    write_text(tree.root / "Scripts" / "lib" / "good.lua",
        "good_hits=good_hits+1; return good_hits\n");
    write_text(tree.root / "Scripts" / "lib" / "bad.lua",
        "bad_hits=bad_hits+1; return false\n");
    write_text(tree.root / "Scripts" / "lib" / "broken.lua",
        "error_hits=error_hits+1; error('synthetic module error')\n");
    const auto pglua = valid_chunk();
    write_bytes(tree.root / "Scripts" / "minimal.lua", pglua);
    write_bytes(tree.root / "Scripts" / "nested.lua", valid_chunk(true));

    const std::array mounts{eawr::vfs::MountSpec{
        "synthetic", tree.root, "data", {},
    }};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "standalone harness mounts its private VFS");
    if (!mounted) return;
    eawr::script::ScriptHost host(mounted.value());

    auto converted_minimal = host.load("data/scripts/minimal.lua");
    auto converted_nested = host.load("data/scripts/nested.lua");
    expect(converted_minimal.has_value() && converted_nested.has_value(),
        "host loads independently authored PGLua through native-width conversion");
    if (converted_minimal) expect(host.destroy(converted_minimal.value()).has_value(),
        "converted minimal PGLua instance shuts down cleanly");
    if (converted_nested) expect(host.destroy(converted_nested.value()).has_value(),
        "converted nested PGLua instance shuts down cleanly");

    std::optional<eawr::script::InstanceId> first;
    std::optional<eawr::script::InstanceId> second;
    std::size_t echo_calls = 0;
    eawr::script::InstanceId foreign_instance = 0;
    expect(host.register_api("Echo", "(number,string)->(number,string)",
        [&echo_calls](const eawr::script::ApiCallContext& context, const ValueList& args) {
            ++echo_calls;
            if (context.api_name != "Echo" || args.size() != 2) {
                return eawr::core::Result<ValueList>::failure(eawr::core::Diagnostic{
                    std::string(eawr::script::diagnostic_codes::invalid_value),
                    eawr::core::Severity::error,
                    "unexpected independent harness callback arguments",
                });
            }
            return eawr::core::Result<ValueList>::success({
                ScriptValue(4.5F), ScriptValue("echoed"),
            });
        }).has_value(), "public callback registration succeeds");
    expect(host.register_api("Foreign", "()->(host)",
        [&foreign_instance](const eawr::script::ApiCallContext&, const ValueList&) {
            return eawr::core::Result<ValueList>::success(ValueList{
                ScriptValue(eawr::script::HostReference{foreign_instance, 1}),
            });
        }).has_value(), "foreign-reference callback registration succeeds");
    expect(host.register_api("Lock_Controls", "(number)->()").has_value(),
        "missing engine API declaration is explicit");
    expect(host.register_api("DestroyNow", "()->()", [&host](
        const eawr::script::ApiCallContext& context, const ValueList&) {
            auto result = host.destroy(context.instance);
            if (!result) return eawr::core::Result<ValueList>::failure(result.error());
            return eawr::core::Result<ValueList>::success(ValueList{});
        }).has_value(), "reentry callback registration succeeds");

    auto first_loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    auto second_loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    expect(first_loaded.has_value() && second_loaded.has_value(),
        "two independent host instances load the same VFS script");
    if (!first_loaded || !second_loaded) return;
    first = first_loaded.value();
    second = second_loaded.value();
    foreign_instance = *second;

    auto first_good = host.start(*first, "Good");
    auto second_good = host.start(*first, "Good");
    auto isolated_good = host.start(*second, "Good");
    expect(first_good.succeeded() && second_good.succeeded() && isolated_good.succeeded() &&
        first_good.result && second_good.result && isolated_good.result &&
        number(*first_good.result) && number(*second_good.result) && number(*isolated_good.result) &&
        *number(*first_good.result) == 1.0F && *number(*second_good.result) == 1.0F &&
        *number(*isolated_good.result) == 1.0F,
        "module values cache by exact instance and do not cross instances");

    auto first_bad = host.start(*first, "Bad");
    auto second_bad = host.start(*first, "Bad");
    auto broken_one = host.start(*first, "Broken");
    auto broken_two = host.start(*first, "Broken");
    auto counts = host.start(*first, "HitCounts");
    const auto* count_values = counts.result ? sequence(*counts.result) : nullptr;
    expect(first_bad.succeeded() && second_bad.succeeded() && first_bad.result &&
        second_bad.result && boolean(*first_bad.result) && boolean(*second_bad.result) &&
        !*boolean(*first_bad.result) && !*boolean(*second_bad.result) &&
        !broken_one.succeeded() && !broken_two.succeeded() && count_values != nullptr &&
        count_values->size() == 3 && number((*count_values)[1]) && number((*count_values)[2]) &&
        *number((*count_values)[1]) == 2.0F && *number((*count_values)[2]) == 2.0F,
        "false and failing modules retry instead of becoming cache hits");
    auto missing = host.start(*first, "Missing");
    expect(!missing.succeeded() && missing.error &&
        missing.error->diagnostic.code == eawr::script::diagnostic_codes::load_parse,
        "missing module fails through VFS-only load path");

    auto echo = host.start(*first, "CallEcho");
    const auto* echo_values = echo.result ? sequence(*echo.result) : nullptr;
    expect(echo.succeeded() && echo_values != nullptr && echo_values->size() == 2 &&
        number((*echo_values)[0]) && *number((*echo_values)[0]) == 4.5F &&
        echo_calls == 1, "Function_Call forwards public values and callback context");
    auto foreign = host.start(*first, "CallForeign");
    expect(!foreign.succeeded() && foreign.error &&
        foreign.error->diagnostic.code == eawr::script::diagnostic_codes::invalid_value,
        "cross-instance host value is rejected at the public boundary");

    auto thread = host.start(*first, "StartYield");
    expect(thread.result && number(*thread.result), "public coroutine creation returns a slot");
    if (thread.result && number(*thread.result)) {
        const auto slot = static_cast<std::uint32_t>(*number(*thread.result));
        auto wrong_instance = host.resume({*second, slot});
        expect(!wrong_instance.succeeded() && wrong_instance.error &&
            wrong_instance.error->diagnostic.code == eawr::script::diagnostic_codes::invalid_instance,
            "coroutine slot cannot cross instance ownership boundary");
        expect(host.resume({*first, slot}).state == eawr::script::ResumeState::live &&
            host.resume({*first, slot}).state == eawr::script::ResumeState::ended,
            "true yield remains live and false yield ends a stable slot");
    }

    expect(host.start(*first, "SetupEvents").succeeded(),
        "event registration script succeeds");
    auto dispatched = host.dispatch(*first, "scan");
    auto event_counts = host.start(*first, "EventCounts");
    const auto* events = event_counts.result ? sequence(*event_counts.result) : nullptr;
    expect(dispatched.succeeded() && dispatched.invoked == 4 && events != nullptr &&
        events->size() == 4 && number((*events)[0]) && number((*events)[1]) &&
        number((*events)[2]) && number((*events)[3]) && *number((*events)[0]) == 1.0F &&
        *number((*events)[1]) == 2.0F && *number((*events)[2]) == 0.0F &&
        *number((*events)[3]) == 1.0F,
        "event mutation restarts at the contract-defined registration boundary");

    auto libraries = host.start(*first, "Libraries");
    const auto* library_values = libraries.result ? sequence(*libraries.result) : nullptr;
    expect(library_values != nullptr && library_values->size() == 15 &&
        std::all_of(library_values->begin(), library_values->end(), [](const ScriptValue& value) {
            const auto* flag = boolean(value);
            return flag != nullptr && *flag;
        }), "approved libraries are exposed and forbidden ones are absent");
    auto missing_api = host.start(*first, "MissingApi");
    expect(!missing_api.succeeded() && missing_api.error &&
        missing_api.error->diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api &&
        missing_api.error->api_name && *missing_api.error->api_name == "Lock_Controls" &&
        !missing_api.error->traceback.empty(),
        "missing engine API carries name, traceback, and distinct diagnostic");

    eawr::script::WideString wide(std::u16string{
        u'A', char16_t{0xd83d}, char16_t{0xde00}, u'B',
    });
    expect(wide.size() == 4 && wide.substr(1, 1).units() == std::u16string{char16_t{0xd83d}},
        "wide-string positions use UTF-16 code units");
    expect(!wide.find_ascii("missing") &&
        wide.find_ascii("missing").error().code == eawr::script::diagnostic_codes::unsupported_feature &&
        !wide.append_ascii("\xc3\xa9") &&
        wide.append_ascii("\xc3\xa9").error().code == eawr::script::diagnostic_codes::unsupported_feature,
        "unsupported wide conversion and not-found sentinel stay explicit");

    auto reentry = host.start(*first, "DestroyDuringCall");
    expect(!reentry.succeeded() && reentry.error &&
        reentry.error->diagnostic.code == eawr::script::diagnostic_codes::invalid_instance,
        "destroy during callback blocks re-entry safely");
    expect(host.destroy(*first).has_value() && host.destroy(*first).has_value(),
        "destroy is idempotent after re-entry shutdown");
    expect(!host.start(*first, "Good").succeeded(),
        "operations after destroy return an invalid-instance failure");
    expect(host.destroy(*second).has_value() && host.destroy(*second).has_value(),
        "second isolated instance can be destroyed independently");
}

void test_shadowed_module_diagnostics() {
    TempTree tree;
    const auto mod = tree.root / "mod";
    const auto base = tree.root / "base";

    // The mod layer wins every logical collision below.  Base files are
    // intentionally valid alternatives, so a diagnostic that reports the
    // base source (or only the root source) is a false acceptance.
    write_text(mod / "scripts" / "main.lua", R"lua(
function ParseFailure()
    return require("broken")
end
function NestedFailure()
    local value = require("outer")
    return value
end
function MissingBinding()
    local invoke = require("binding")
    local value = invoke()
    return value
end
function RetryFailure()
    return require("retry")
end
function RetryCount()
    return retry_hits
end
retry_hits = 0
)lua");
    write_text(base / "scripts" / "main.lua", "function ParseFailure() return 41 end\n");

    write_text(mod / "scripts" / "lib" / "broken.lua",
        "local marker = true\nthis is invalid\n");
    write_text(base / "scripts" / "lib" / "broken.lua", "return 41\n");

    write_text(mod / "scripts" / "lib" / "outer.lua", R"lua(local function call_inner()
    return require("inner")
end
return call_inner()
)lua");
    write_text(base / "scripts" / "lib" / "outer.lua", "return 42\n");
    write_text(mod / "scripts" / "lib" / "inner.lua",
        "local marker = true\nerror(\"nested module runtime failure\")\n");
    write_text(base / "scripts" / "lib" / "inner.lua", "return 43\n");

    write_text(mod / "scripts" / "lib" / "binding.lua", R"lua(local function invoke()
    Lock_Controls(7)
end
return invoke
)lua");
    write_text(base / "scripts" / "lib" / "binding.lua",
        "return function() return 44 end\n");
    write_text(mod / "scripts" / "lib" / "retry.lua", R"lua(
retry_hits = retry_hits + 1
error("retry module failure")
)lua");
    write_text(base / "scripts" / "lib" / "retry.lua", "return 45\n");

    const std::array mounts{eawr::vfs::MountSpec{
        "mod", mod, "data", {},
    }, eawr::vfs::MountSpec{
        "base", base, "data", {},
    }};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "diagnostic fixture mounts ordered mod/base VFS layers");
    if (!mounted) return;

    const auto expected_path = [](std::string_view name) {
        return std::string("data/scripts/lib/") + std::string(name) + ".lua";
    };
    const auto expected_source = [](std::string_view name) {
        return std::string("mod:loose:data/scripts/lib/") + std::string(name) + ".lua";
    };
    const auto expected_winner = [&](std::string_view name) {
        auto record = mounted.value().stat(expected_path(name));
        expect(record.has_value(), "diagnostic fixture has a winning VFS record");
        if (!record) return std::string{};
        expect(record.value().source_id == expected_source(name),
            "VFS winner is the higher-precedence mod source");
        return record.value().source_id;
    };
    const auto broken_source = expected_winner("broken");
    const auto inner_source = expected_winner("inner");
    const auto binding_source = expected_winner("binding");
    const auto retry_source = expected_winner("retry");

    eawr::script::ScriptHost host(mounted.value());
    expect(host.register_api("Lock_Controls", "(number)->()").has_value(),
        "missing engine API is registered for module diagnostic fixture");
    auto first_loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    auto second_loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    expect(first_loaded.has_value() && second_loaded.has_value(),
        "diagnostic fixture creates independent root instances");
    if (!first_loaded || !second_loaded) return;
    const auto first = first_loaded.value();
    const auto second = second_loaded.value();

    const auto has = [](const std::string& text, std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    const auto check_source = [&](const eawr::script::CallOutcome& outcome,
        std::string_view operation, std::string_view path, std::string_view source,
        std::uint64_t line, std::string_view root_function,
        std::string_view module_source = {}) {
        expect(!outcome.succeeded() && outcome.error.has_value(), operation);
        if (!outcome.error) return;
        const auto& error = *outcome.error;
        expect(error.instance && *error.instance == first,
            "diagnostic retains the root instance identity");
        expect(error.operation == operation, "diagnostic retains the public operation");
        expect(error.diagnostic.logical_path && *error.diagnostic.logical_path == path,
            "diagnostic names the canonical failing module path");
        expect(error.diagnostic.source_id && *error.diagnostic.source_id == source,
            "diagnostic retains the winning VFS source ID");
        expect(error.diagnostic.line && *error.diagnostic.line == line,
            "diagnostic retains the failing module line");
        expect(has(error.traceback, "data/scripts/main.lua") &&
            has(error.traceback, root_function),
            "diagnostic traceback retains the root caller context");
        if (!module_source.empty()) {
            expect(has(error.traceback, module_source),
                "diagnostic traceback retains the failing module context");
        }
    };

    const auto parse = host.start(first, "ParseFailure");
    check_source(parse, "require", "data/scripts/lib/broken.lua", broken_source, 2,
        "ParseFailure");
    if (parse.error) {
        expect(parse.error->diagnostic.code == eawr::script::diagnostic_codes::load_parse,
            "shadowed module parse failure has the load-parse code");
        expect(has(parse.error->diagnostic.message, ":2:"),
            "shadowed module parse failure preserves parser line text");
    }

    const auto nested = host.start(first, "NestedFailure");
    check_source(nested, "require", "data/scripts/lib/inner.lua", inner_source, 2,
        "NestedFailure", "data/scripts/lib/inner.lua");
    if (nested.error) {
        expect(nested.error->diagnostic.code == eawr::script::diagnostic_codes::lua_execution,
            "nested module runtime failure has the Lua-execution code");
        expect(has(nested.error->diagnostic.message, "nested module runtime failure"),
            "nested module runtime failure preserves its message");
    }

    const auto missing = host.start(first, "MissingBinding");
    check_source(missing, "engine-api", "data/scripts/lib/binding.lua", binding_source, 2,
        "MissingBinding", "data/scripts/lib/binding.lua");
    if (missing.error) {
        expect(missing.error->diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api,
            "module missing binding has the distinct API diagnostic code");
        expect(missing.error->api_name && *missing.error->api_name == "Lock_Controls",
            "module missing binding retains the API name");
    }

    // A failed require must not become a cache hit, while a second host state
    // must not share either the module cache or the mutable retry counter.
    const auto retry_one = host.start(first, "RetryFailure");
    const auto retry_two = host.start(first, "RetryFailure");
    const auto retry_count = host.start(first, "RetryCount");
    const auto* first_count = retry_count.result ? number(*retry_count.result) : nullptr;
    expect(!retry_one.succeeded() && !retry_two.succeeded() && first_count &&
        *first_count == 2.0F, "failing require is retried instead of cached");
    const auto retry_other = host.start(second, "RetryFailure");
    const auto retry_other_count = host.start(second, "RetryCount");
    const auto* second_count = retry_other_count.result ? number(*retry_other_count.result) : nullptr;
    expect(!retry_other.succeeded() && second_count && *second_count == 1.0F,
        "failed module state and counter remain independent across instances");
    if (retry_one.error) {
        check_source(retry_one, "require", "data/scripts/lib/retry.lua", retry_source, 3,
            "RetryFailure");
    }
}

void test_pinned_smoke_metadata() {
    TempTree tree;
    constexpr std::string_view smoke_path =
        "scripts/story/story_empire_acti_m02_fondor_land.lua";
    write_text(tree.root / std::filesystem::path(smoke_path),
        "function Intro_Cinematic()\n    Lock_Controls(1)\nend\n");
    const std::array mounts{eawr::vfs::MountSpec{
        "synthetic-smoke", tree.root, "data", {},
    }};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "pinned smoke fixture mounts a synthetic story root");
    if (!mounted) return;
    const auto record = mounted.value().stat(
        "data/scripts/story/story_empire_acti_m02_fondor_land.lua");
    expect(record.has_value(), "pinned smoke fixture has a winning root source");
    if (!record) return;

    eawr::script::ScriptHost host(mounted.value());
    expect(host.register_api("Lock_Controls", "(number)->()").has_value(),
        "pinned smoke fixture declares the expected known-but-missing API");
    auto loaded = host.load("data/scripts/story/story_empire_acti_m02_fondor_land.lua");
    expect(loaded.has_value(), "pinned smoke fixture loads its canonical root script");
    if (!loaded) return;
    const auto outcome = host.start(loaded.value(), "Intro_Cinematic");
    expect(!outcome.succeeded() && outcome.error.has_value(),
        "pinned smoke fixture terminates at the missing Lock_Controls boundary");
    if (!outcome.error) return;
    const auto& error = *outcome.error;
    expect(error.diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api &&
        error.api_name && *error.api_name == "Lock_Controls",
        "pinned smoke diagnostic retains the API boundary and code");
    expect(error.source_kind && *error.source_kind == "pglua" &&
        error.prototype_id && *error.prototype_id == 7 &&
        error.pc && *error.pc == 5,
        "pinned smoke diagnostic retains the established PGLua prototype/PC metadata");
    expect(error.diagnostic.logical_path &&
        *error.diagnostic.logical_path ==
            "data/scripts/story/story_empire_acti_m02_fondor_land.lua" &&
        error.diagnostic.source_id &&
        *error.diagnostic.source_id == record.value().source_id &&
        !error.diagnostic.line && !error.diagnostic.column,
        "pinned smoke diagnostic retains root provenance without a fabricated source line");
    expect(error.traceback.find("Intro_Cinematic") != std::string::npos,
        "pinned smoke traceback retains the entry function");
}

} // namespace

int main() {
    test_pglua();
    test_host();
    test_shadowed_module_diagnostics();
    test_pinned_smoke_metadata();
    if (failures != 0) {
        std::cerr << failures << " independent P0-07 acceptance assertion(s) failed\n";
        return 1;
    }
    std::cout << "P0-07 independent acceptance passed\n";
    return 0;
}
