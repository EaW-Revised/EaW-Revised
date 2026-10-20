#include "eawr/core/json_text.hpp"
#include "eawr/core/text.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/script/script_host.hpp"
#include "eawr/vfs/vfs.hpp"

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
#include <vector>

namespace {

constexpr std::string_view eaw_archive_hash =
    "79774426a7d1442a2dd08eb87e16e3c9b39093f1224d862def7281d6587ccb35";
constexpr std::string_view foc_archive_hash =
    "f9e1d517c3b0990d764843827a3cf42aa71cd91c2bdd8b0b15505b73ffcd9dc9";
constexpr std::string_view pinned_script =
    "data/scripts/story/story_empire_acti_m02_fondor_land.lua";
constexpr std::string_view pinned_script_hash =
    "8ae61a1a0f1150dba3fd7d765cdb81c019df5ce38452d9d781de8c5895f5b983";

struct Arguments {
    std::string profile;
    std::filesystem::path game_root;
    std::string script;
    std::string entry;
    std::filesystem::path report;
};

void usage(std::ostream& output) {
    output << "Usage: script_smoke --profile <eaw|foc> --game-root <dir> "
              "--script <logical-path> --entry <function> --report <json>\n";
}

std::optional<Arguments> parse_arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") { usage(std::cout); return std::nullopt; }
        if (index + 1 >= argc) return std::nullopt;
        const std::string value(argv[++index]);
        if (option == "--profile") result.profile = value;
        else if (option == "--game-root") result.game_root = value;
        else if (option == "--script") result.script = value;
        else if (option == "--entry") result.entry = value;
        else if (option == "--report") result.report = value;
        else return std::nullopt;
    }
    if ((result.profile != "eaw" && result.profile != "foc") || result.game_root.empty() ||
        result.script.empty() || result.entry.empty() || result.report.empty()) return std::nullopt;
    return result;
}

bool iequals(const std::string_view left, const std::string_view right) {
    return eawr::core::iequals(left, right);
}

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::filesystem::path data_root(const std::filesystem::path& root, const std::string_view profile) {
    if (iequals(utf8(root.filename()), "data")) return root;
    if (profile == "eaw" && std::filesystem::is_directory(root / "GameData" / "Data")) {
        return root / "GameData" / "Data";
    }
    if (profile == "foc" && std::filesystem::is_directory(root / "corruption" / "Data")) {
        return root / "corruption" / "Data";
    }
    if (std::filesystem::is_directory(root / "Data")) return root / "Data";
    return root;
}

std::string json_string(const std::string_view value) {
    return eawr::core::json_quote(value);
}

std::string sha256_hex(const std::span<const std::byte> input) {
    return eawr::core::sha256_hex({reinterpret_cast<const std::uint8_t*>(input.data()), input.size()});
}

std::optional<std::string> hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) return std::nullopt;
    return sha256_hex(bytes);
}

void write_diagnostic(std::ostream& output, const eawr::script::ScriptDiagnostic& diagnostic) {
    output << "{\"code\":" << json_string(diagnostic.diagnostic.code)
        << ",\"message\":" << json_string(diagnostic.diagnostic.message)
        << ",\"instance\":" << (diagnostic.instance ? std::to_string(*diagnostic.instance) : "null")
        << ",\"coroutine\":" << (diagnostic.coroutine ? std::to_string(*diagnostic.coroutine) : "null")
        << ",\"operation\":" << json_string(diagnostic.operation)
        << ",\"api_name\":" << (diagnostic.api_name ? json_string(*diagnostic.api_name) : "null")
        << ",\"traceback\":" << json_string(diagnostic.traceback)
        << ",\"source_kind\":" << (diagnostic.source_kind ? json_string(*diagnostic.source_kind) : "null")
        << ",\"prototype_id\":" << (diagnostic.prototype_id ? std::to_string(*diagnostic.prototype_id) : "null")
        << ",\"pc\":" << (diagnostic.pc ? std::to_string(*diagnostic.pc) : "null") << '}';
}

} // namespace

int main(const int argc, char** argv) {
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments) {
        if (argc == 2 && std::string_view(argv[1]) == "--help") return 0;
        usage(std::cerr); return 2;
    }
    const auto selected_data = data_root(arguments->game_root, arguments->profile);
    std::vector<eawr::vfs::MountSpec> mounts;
    std::optional<std::filesystem::path> identity_archive;
    if (arguments->profile == "foc") {
        auto expansion = eawr::vfs::resolve_manifest_mount("expansion", selected_data);
        const auto base_root = arguments->game_root / "GameData" / "Data";
        auto base = eawr::vfs::resolve_manifest_mount("base", base_root);
        if (!expansion || !base) { std::cerr << "cannot resolve FoC/base VFS manifests\n"; return 2; }
        for (const auto& archive : expansion.value().mount.active_archives) {
            if (iequals(utf8(archive.path.filename()), "config.meg")) identity_archive = archive.path;
        }
        mounts.push_back(std::move(expansion).value().mount);
        mounts.push_back(std::move(base).value().mount);
    } else {
        auto base = eawr::vfs::resolve_manifest_mount("base", selected_data);
        if (!base) { std::cerr << eawr::core::format_diagnostic(base.error()) << '\n'; return 2; }
        for (const auto& archive : base.value().mount.active_archives) {
            if (iequals(utf8(archive.path.filename()), "config.meg")) identity_archive = archive.path;
        }
        // The approved oracle identifies the config.meg member itself, not a
        // later patch winner with the same logical name.  Keep all reads on
        // the VFS while mounting only that pinned archive for qualification.
        if (identity_archive) {
            eawr::vfs::MountSpec pinned{
                "base",
                selected_data,
                "__pinned_smoke_no_loose__",
                {eawr::vfs::ArchiveSpec{*identity_archive, "base:Data/config.meg", {}}},
            };
            mounts.push_back(std::move(pinned));
        }
    }
    if (!identity_archive) { std::cerr << "active config.meg identity archive was not found\n"; return 2; }
    const auto archive_hash = hash_file(*identity_archive);
    const auto expected_archive_hash = arguments->profile == "eaw" ? eaw_archive_hash : foc_archive_hash;
    if (!archive_hash || *archive_hash != expected_archive_hash) {
        std::cerr << "config.meg does not match the pinned " << arguments->profile << " corpus"
            << (archive_hash ? ": " + *archive_hash : ": unreadable")
            << " selected=" << utf8(*identity_archive) << '\n'; return 2;
    }
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    if (!mounted) { std::cerr << eawr::core::format_diagnostic(mounted.error()) << '\n'; return 2; }
    const eawr::script::RetailIdentity retail_identity{arguments->profile, *archive_hash};
    std::size_t corpus_members = 0;
    std::size_t corpus_prototypes = 0;
    std::size_t gated_members = 0;
    auto script_records = mounted.value().enumerate("data/scripts", ".lua");
    if (!script_records) { std::cerr << eawr::core::format_diagnostic(script_records.error()) << '\n'; return 2; }
    for (const auto& record : script_records.value()) {
        auto bytes = mounted.value().open(record.canonical_path);
        if (!bytes) { std::cerr << eawr::core::format_diagnostic(bytes.error()) << '\n'; return 2; }
        if (bytes.value().size() < 4 || bytes.value()[0] != std::byte{0x1b} ||
            bytes.value()[1] != std::byte{'L'} || bytes.value()[2] != std::byte{'u'} ||
            bytes.value()[3] != std::byte{'p'}) continue;
        auto converted = eawr::script::convert_pglua(
            bytes.value(), record.canonical_path, retail_identity
        );
        if (!converted) {
            std::cerr << eawr::core::format_diagnostic(converted.error()) << '\n';
            return 2;
        }
        ++corpus_members;
        corpus_prototypes += converted.value().prototypes.size();
        if (!converted.value().unsupported_execution.empty()) ++gated_members;
    }
    auto canonical = eawr::vfs::canonicalize(arguments->script);
    if (!canonical) { std::cerr << eawr::core::format_diagnostic(canonical.error()) << '\n'; return 2; }
    auto script_bytes = mounted.value().open(canonical.value());
    auto script_record = mounted.value().stat(canonical.value());
    if (!script_bytes || !script_record) { std::cerr << "script is absent from the mounted VFS\n"; return 2; }
    const auto script_hash = sha256_hex(script_bytes.value());

    eawr::script::ScriptHost host(mounted.value());
    auto suspend = host.register_api(
        "Suspend_AI", "(number=1)->()",
        [](const eawr::script::ApiCallContext&, const eawr::script::ValueList& values) {
            if (values.size() != 1) {
                eawr::core::Diagnostic diagnostic;
                diagnostic.code = eawr::script::diagnostic_codes::invalid_value;
                diagnostic.message = "Suspend_AI expects exactly one number";
                return eawr::core::Result<eawr::script::ValueList>::failure(std::move(diagnostic));
            }
            const auto* number = std::get_if<float>(&values[0].storage());
            if (number == nullptr || *number != 1.0F) {
                eawr::core::Diagnostic diagnostic;
                diagnostic.code = eawr::script::diagnostic_codes::invalid_value;
                diagnostic.message = "Suspend_AI smoke stub accepts only number 1";
                return eawr::core::Result<eawr::script::ValueList>::failure(std::move(diagnostic));
            }
            return eawr::core::Result<eawr::script::ValueList>::success({});
        }
    );
    auto missing = host.register_api("Lock_Controls", "(number)->()");
    if (!suspend || !missing) { std::cerr << "cannot register bounded smoke API surface\n"; return 2; }
    auto loaded = host.load(
        canonical.value(),
        {"Data/Scripts/Library", "Data/Scripts/Story"},
        retail_identity
    );

    eawr::script::CallOutcome outcome;
    if (loaded) outcome = host.start(loaded.value(), arguments->entry);
    const auto trace = loaded ? host.module_trace(loaded.value())
                              : eawr::core::Result<std::vector<eawr::script::ModuleRequest>>::failure(loaded.error());
    const bool pinned_request = arguments->profile == "eaw" && canonical.value() == pinned_script &&
        arguments->entry == "Intro_Cinematic" && script_hash == pinned_script_hash;
    const bool expected_boundary = loaded && !outcome.succeeded() && outcome.error &&
        outcome.error->diagnostic.code == eawr::script::diagnostic_codes::missing_engine_api &&
        outcome.error->api_name == "Lock_Controls" && outcome.error->prototype_id == 7 &&
        outcome.error->pc == 5U && host.invocations().size() == 2 &&
        host.invocations()[0].name == "Suspend_AI" && host.invocations()[1].name == "Lock_Controls";

    std::filesystem::create_directories(arguments->report.parent_path().empty()
        ? std::filesystem::path(".") : arguments->report.parent_path());
    std::ofstream report(arguments->report, std::ios::binary);
    if (!report) { std::cerr << "cannot create smoke report\n"; return 2; }
    report << "{\n  \"schema_version\": 1,\n  \"profile\": " << json_string(arguments->profile)
        << ",\n  \"qualified_eaw_oracle\": " << (pinned_request && expected_boundary ? "true" : "false")
        << ",\n  \"corpus\": {\"archive\":\"config.meg\",\"sha256\":" << json_string(*archive_hash)
        << ",\"converted_members\":" << corpus_members
        << ",\"prototype_count\":" << corpus_prototypes
        << ",\"execution_gated_members\":" << gated_members
        << "},\n  \"script\": {\"logical_path\":" << json_string(canonical.value())
        << ",\"source_id\":" << json_string(script_record.value().source_id)
        << ",\"size\":" << script_record.value().size << ",\"sha256\":" << json_string(script_hash)
        << ",\"entry\":" << json_string(arguments->entry) << "},\n  \"capabilities\": {\"fork_security\":false,\"state_pooling\":false,\"whole_state_persistence\":false,\"multi_value_coroutine_yield\":false,\"non_ascii_wide_conversion\":false},\n";
    report << "  \"module_trace\": [";
    if (trace) for (std::size_t i=0;i<trace.value().size();++i) {
        if (i) report << ',';
        const auto& row=trace.value()[i];
        report << "{\"name\":" << json_string(row.name) << ",\"resolved_path\":"
            << (row.resolved_path ? json_string(*row.resolved_path) : "null")
            << ",\"cache_hit\":" << (row.cache_hit?"true":"false");
        if (row.resolved_path) { auto bytes=mounted.value().open(*row.resolved_path); if(bytes) report << ",\"sha256\":" << json_string(sha256_hex(bytes.value())); }
        report << '}';
    }
    report << "],\n  \"calls\": [";
    for (std::size_t i=0;i<host.invocations().size();++i) {
        if(i) report << ',';
        const auto& call=host.invocations()[i];
        report << "{\"name\":" << json_string(call.name) << ",\"arguments\":[";
        for(std::size_t j=0;j<call.arguments.size();++j){if(j)report<<',';const auto* n=std::get_if<float>(&call.arguments[j].storage());if(n)report<<*n;else report<<"null";}
        report << "]}";
    }
    report << "],\n  \"outcome\": {\"expected_missing_api\":" << (expected_boundary?"true":"false") << ",\"diagnostic\":";
    if (outcome.error) write_diagnostic(report,*outcome.error); else report << "null";
    report << "}\n}\n";
    report.close();
    std::cout << "profile=" << arguments->profile << " modules=" << (trace ? trace.value().size() : 0)
        << " calls=" << host.invocations().size() << " expected_boundary=" << (expected_boundary?"yes":"no") << '\n';
    return pinned_request && expected_boundary ? 0 : 1;
}
