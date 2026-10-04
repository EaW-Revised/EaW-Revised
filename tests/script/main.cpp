#include "host_support.hpp"
#include "eawr/script/pglua.hpp"
#include "eawr/script/script_host.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace host_test_support {

int failures = 0;

TempTree::TempTree() { std::filesystem::create_directories(root); }
TempTree::~TempTree() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }


void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) throw std::runtime_error("failed to write synthetic Lua fixture");
}

std::vector<std::byte> read_hex(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::vector<std::byte> result;
    unsigned value = 0;
    while (stream >> std::hex >> value) result.push_back(static_cast<std::byte>(value));
    return result;
}

void test_pglua() {
    const auto root = std::filesystem::path(EAWR_SCRIPT_FIXTURES);
    const auto minimal = read_hex(root / "minimal-valid.pglua.hex");
    auto converted = eawr::script::convert_pglua(minimal, "synthetic/minimal.pglua");
    if (!converted) std::cerr << "minimal conversion: " << converted.error().code << ' ' << converted.error().message << '\n';
    const auto expected_size = 74U + (sizeof(std::size_t) - 4U);
    expect(converted && converted.value().bytes.size() == expected_size,
        "minimal PGLua converts to native-width Lua 5.0.2");
    expect(converted && converted.value().prototypes.size() == 1 &&
        converted.value().prototypes[0].persistence_id == 1,
        "minimal PGLua retains persistence metadata");

    const auto nested = read_hex(root / "nested-debug.pglua.hex");
    auto nested_result = eawr::script::convert_pglua(nested, "synthetic/nested-debug.pglua");
    if (!nested_result) std::cerr << "nested conversion: " << nested_result.error().code << ' ' << nested_result.error().message << '\n';
    expect(nested_result && nested_result.value().prototypes.size() == 2,
        "independent nested/debug fixture decodes both prototypes");

    const auto malformed = read_hex(root / "malformed-trailing.pglua.hex");
    auto malformed_result = eawr::script::convert_pglua(malformed, "synthetic/malformed.pglua");
    expect(!malformed_result && malformed_result.error().code == eawr::script::diagnostic_codes::load_parse,
        "malformed trailing-data fixture is rejected before the VM");

    const std::array<std::pair<std::size_t, std::byte>, 7> header_mutations{{
        {3U, std::byte{0x61}}, {4U, std::byte{0x52}}, {5U, std::byte{0x00}},
        {7U, std::byte{0x08}}, {13U, std::byte{0x04}}, {38U, std::byte{0x00}},
        {33U, std::byte{0x7f}},
    }};
    for (const auto& [offset, replacement] : header_mutations) {
        auto mutation = minimal;
        mutation[offset] = replacement;
        expect(!eawr::script::convert_pglua(mutation, "synthetic/mutation.pglua"),
            "independent malformed PGLua mutation is rejected");
    }
    auto truncated = minimal;
    truncated.pop_back();
    expect(!eawr::script::convert_pglua(truncated, "synthetic/truncated.pglua"),
        "truncated PGLua fixture is rejected");
}

void test_wide_strings() {
    eawr::script::WideString value(std::u16string{u'A', char16_t{0xd83d}, char16_t{0xde00}, u'B'});
    expect(value.size() == 4, "wide length counts UTF-16 code units");
    expect(value.at(1).units() == std::u16string({char16_t{0xd83d}, char16_t{0xde00}, u'B'}),
        "wide at returns the suffix from a unit position");
    expect(value.substr(1, 1).units() == std::u16string({char16_t{0xd83d}}),
        "wide substring preserves a split surrogate unit");
    eawr::script::WideString ascii(u"AB");
    auto copy = ascii.append_ascii("C");
    expect(copy && ascii.units() == u"ABC" && copy.value().units() == u"ABC",
        "wide mutation returns a distinct equal wrapper");
    auto comparison = ascii.compare_ascii("ABC");
    expect(comparison && comparison.value() == 0.0F,
        "wide compare accepts the narrow ASCII compatibility type");
    auto found = ascii.find_ascii("BC");
    expect(found && found.value() == 1.0F, "wide found position is zero-based at the binary32 boundary");
    expect(!ascii.find_ascii("missing") &&
        ascii.find_ascii("missing").error().code == eawr::script::diagnostic_codes::unsupported_feature,
        "wide not-found sentinel remains an explicit unsupported gate");
    auto rejected = ascii.append_ascii("\xc3\xa9");
    expect(!rejected && rejected.error().code == eawr::script::diagnostic_codes::unsupported_feature,
        "non-ASCII narrow/wide conversion stays explicitly unsupported");
}

void test_diagnostic_provenance() {
    TempTree tree;
    const auto mod_root = tree.root / "mod";
    const auto base_root = tree.root / "base";

    write_text(base_root / "Scripts" / "main.lua", R"lua(
function ParseShadowed() require("Broken") end
function MissingShadowed() require("MissingBinding") end
function RuntimeShadowed() require("RuntimeFailure") end
    )lua");
    write_text(base_root / "Scripts" / "lib" / "Broken.lua", "return true\n");
    write_text(base_root / "Scripts" / "lib" / "MissingBinding.lua", "return true\n");
    write_text(base_root / "Scripts" / "lib" / "RuntimeFailure.lua", "return true\n");

    write_text(mod_root / "Scripts" / "lib" / "Broken.lua", "local valid = true\nlocal = broken\n");
    write_text(mod_root / "Scripts" / "lib" / "MissingBinding.lua", R"lua(local function invoke()
    Lock_Controls(1)
end
invoke()
    )lua");
    write_text(mod_root / "Scripts" / "lib" / "RuntimeFailure.lua", R"lua(local function explode()
    error("shadowed runtime failure")
end
explode()
    )lua");

    const std::array mounts{
        eawr::vfs::MountSpec{"mod", mod_root, "data", {}},
        eawr::vfs::MountSpec{"base", base_root, "data", {}},
    };
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "shadowed diagnostic VFS mounts");
    if (!mounted) return;

    eawr::script::ScriptHost host(mounted.value());
    expect(static_cast<bool>(host.register_api("Lock_Controls", "(number)->()")),
        "shadowed diagnostic missing binding registers");
    auto loaded = host.load("data/scripts/main.lua", {"data/scripts/lib"});
    expect(static_cast<bool>(loaded), "shadowed diagnostic root script loads");
    if (!loaded) return;

    const auto id = loaded.value();
    auto parsed = host.start(id, "ParseShadowed");
    expect(!parsed.succeeded() && parsed.error &&
        parsed.error->diagnostic.code == eawr::script::diagnostic_codes::load_parse &&
        parsed.error->diagnostic.logical_path == "data/scripts/lib/broken.lua" &&
        parsed.error->diagnostic.source_id == "mod:loose:data/Scripts/lib/Broken.lua" &&
        parsed.error->diagnostic.line == 2,
        "shadowed module parse diagnostic retains canonical winner path, source id, and line");

    auto missing = host.start(id, "MissingShadowed");
    expect(!missing.succeeded() && missing.error &&
        missing.error->diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api &&
        missing.error->diagnostic.logical_path == "data/scripts/lib/missingbinding.lua" &&
        missing.error->diagnostic.source_id == "mod:loose:data/Scripts/lib/MissingBinding.lua" &&
        missing.error->diagnostic.line == 2 &&
        missing.error->traceback.find("MissingBinding.lua") != std::string::npos,
        "shadowed missing binding retains winning module provenance and Lua frame");

    auto runtime = host.start(id, "RuntimeShadowed");
    expect(!runtime.succeeded() && runtime.error &&
        runtime.error->diagnostic.code == eawr::script::diagnostic_codes::lua_execution &&
        runtime.error->diagnostic.logical_path == "data/scripts/lib/runtimefailure.lua" &&
        runtime.error->diagnostic.source_id == "mod:loose:data/Scripts/lib/RuntimeFailure.lua" &&
        runtime.error->diagnostic.line == 2 &&
        runtime.error->traceback.find("RuntimeFailure.lua") != std::string::npos,
        "shadowed runtime error retains winning module provenance and Lua frame");
}

} // namespace

using namespace host_test_support;

int main() {
    test_pglua();
    test_wide_strings();
    test_host();
    test_diagnostic_provenance();
    if (failures != 0) {
        std::cerr << failures << " script contract test(s) failed\n";
        return 1;
    }
    std::cout << "script contracts passed\n";
    return 0;
}
