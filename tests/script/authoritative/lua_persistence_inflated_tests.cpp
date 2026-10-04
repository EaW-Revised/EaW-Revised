#include "lua_persistence_support.hpp"

namespace lua_persistence_test_support {

std::uint32_t read_u32(const std::string& bytes, std::size_t position) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[position + index])) << (8 * index);
    }
    return value;
}

void write_u32(std::string& bytes, std::size_t position, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) bytes[position + index] = static_cast<char>((value >> (8 * index)) & 0xFF);
}

// Rejected like expect_rejected, and decoding allocated nothing near the size a
// forged count claims: no single allocation of 1 MiB, 16 MiB in all.
void expect_rejected_small(Session& session, const std::string& bytes, std::string_view reason, const std::string& label) {
    const std::string before = state_hash(session);
    alloc_probe::largest = 0;
    alloc_probe::total = 0;
    alloc_probe::armed = true;
    auto result = session.scheduler->load(bytes);
    alloc_probe::armed = false;
    const std::size_t largest = alloc_probe::largest;
    const std::size_t total = alloc_probe::total;
    std::cout << label << ": largest allocation " << largest << " bytes, " << total << " in all\n";
    expect(!result && result.error().code == auth::codes::load_rejected &&
               result.error().message.find(reason) != std::string::npos,
           label + ": " + (result ? std::string("accepted") : result.error().message));
    expect(largest < (std::size_t{1} << 20) && total < (std::size_t{16} << 20), label + ": decoding stayed small");
    expect(state_hash(session) == before, label + ": scheduler unchanged");
}

// Forged counts with matching padding (review of PR #353): each claims far
// more items than the save holds and is rejected before memory follows it.
void run_inflated_counts(const std::string& bytes) {
    // Lua graph objects: a graph header is (object count, registry 1, main
    // thread 2, four table IDs within the count). The last instance's graph
    // claims a million more objects, padded with 3 bytes (the smallest object
    // encoding) for each.
    std::size_t header = std::string::npos;
    for (std::size_t position = 0; position + 28 <= bytes.size(); ++position) {
        const std::uint32_t count = read_u32(bytes, position);
        if (count < 6 || read_u32(bytes, position + 4) != 1 || read_u32(bytes, position + 8) != 2) continue;
        bool tables = true;
        for (std::size_t field = 12; field < 28; field += 4) {
            const std::uint32_t id = read_u32(bytes, position + field);
            tables = tables && id >= 3 && id <= count;
        }
        if (tables) header = position;
    }
    expect(header != std::string::npos, "graph header found");
    if (header != std::string::npos) {
        constexpr std::uint32_t claimed = 1'000'000;
        std::string forged = bytes;
        write_u32(forged, header, read_u32(forged, header) + claimed);
        forged.append(std::size_t{claimed} * 3, '\0');
        Session target = persistence_session();
        expect_rejected_small(target, forged, "malformed Lua graph", "inflated graph object count");
    }

    // Handler lists: 200,000 handlers for one event name (quota 65,536), padded
    // with 12 bytes each.
    const std::string script = "Register_Event(\"ProbeHandlerName\", function() end)";
    Session source = script_session(script);
    expect(source.scheduler->create_instance(1, "Main.lua").has_value(), "create handler instance");
    auto saved = source.scheduler->save();
    expect(saved.has_value(), "handler save");
    if (!saved) return;
    std::string forged = saved.value();
    const std::string name("\x10\0\0\0ProbeHandlerName", 20);
    const std::size_t found = forged.find(name); // the instance record precedes the graph
    expect(found != std::string::npos && read_u32(forged, found + name.size()) == 1, "handler record found");
    if (found == std::string::npos) return;
    constexpr std::uint32_t handlers = 200'000;
    write_u32(forged, found + name.size(), handlers);
    forged.append(std::size_t{handlers} * 12, '\0');
    Session target = script_session(script);
    expect_rejected_small(target, forged, "quota exceeded", "handler count beyond the registration quota");
}

// Host values nest lists at most max_value_list_depth deep: submit_event
// admits exactly what a save can hold.
void run_value_depth(const eawr::sim::PartitionExecutor& executor) {
    const auto nested = [](int depth) {
        auth::Value value;
        for (int level = 0; level < depth; ++level) {
            auth::Value outer;
            outer.data = std::vector<auth::Value>{std::move(value)};
            value = std::move(outer);
        }
        return value;
    };
    expect(auth::max_value_list_depth == 64, "value list depth bound");
    Session session = persistence_session();
    barrier_actions(session, 0);
    auto accepted = session.scheduler->submit_event(dispatch_event({3, 30, 1, 1}, 5, "deep", {nested(64)}));
    expect(accepted.has_value(), "64 nested lists are admitted");
    auth::ScriptEvent signal = dispatch_event({3, 30, 1, 2}, 5, "deep", {});
    signal.kind = auth::ScriptEvent::Kind::thread_signal;
    signal.parameter = nested(64);
    expect(session.scheduler->submit_event(std::move(signal)).has_value(), "a 64-deep parameter is admitted");
    auto bytes = session.scheduler->save();
    expect(bytes.has_value(), "save with 64 nested lists");
    if (bytes) {
        Session other = loaded(bytes.value());
        auto resaved = other.scheduler->save();
        expect(resaved.has_value() && resaved.value() == bytes.value(), "64 nested lists round-trip");
        expect(state_hash(other) == state_hash(session), "64 nested lists: state hash");
        step(other, executor);
    }

    const std::string hash = state_hash(session);
    for (const bool as_parameter : {false, true}) {
        auth::ScriptEvent event = dispatch_event({3, 30, 1, 3}, 5, "deep", {});
        if (as_parameter) {
            event.kind = auth::ScriptEvent::Kind::thread_signal;
            event.parameter = nested(65);
        } else {
            event.arguments.push_back(auth::Value::number(integer(1)));
            event.arguments.push_back(nested(65));
        }
        auto rejected = session.scheduler->submit_event(std::move(event));
        expect(!rejected && rejected.error().code == auth::codes::invalid_request &&
                   rejected.error().message.find("deeper than 64") != std::string::npos,
               std::string("65 nested lists are rejected at submit") + (as_parameter ? " (parameter)" : ""));
    }
    expect(state_hash(session) == hash, "rejected deep events leave the scheduler unchanged");
}


} // namespace lua_persistence_test_support
