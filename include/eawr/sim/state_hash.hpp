#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace eawr::sim {

// A tick's state SHA-256 (lowercase hex), ready or still being computed off the stepping thread
// (#637, docs/simulation.md). get() returns the same digest either way; for a pending hash it
// blocks until the hasher has finished it. Copies share the one result.
class StateHash {
public:
    // Where a pending hash comes from; get() blocks until the digest is known.
    class Source {
    public:
        virtual ~Source() = default;
        [[nodiscard]] virtual const std::string& get() const = 0;
    };

    StateHash() = default; // no hash: get() is empty
    explicit StateHash(std::string ready);
    explicit StateHash(std::shared_ptr<const Source> pending) noexcept;

    [[nodiscard]] const std::string& get() const;

private:
    std::shared_ptr<const Source> source_;
};

// Hashes canonical state bytes off the stepping thread: hash() returns at once and the digest is
// sim::sha256_hex of the bytes, the value the synchronous path computes. The platform layer
// provides the thread (platform::ThreadStateHasher); the simulation never starts one itself.
class StateHasher {
public:
    using Derivation = std::function<std::string(const std::string& base_hex)>;

    virtual ~StateHasher() = default;
    [[nodiscard]] virtual StateHash hash(std::vector<std::uint8_t> canonical_bytes) = 0;
    // A hash computed from an earlier one this hasher returned, after it and off the stepping
    // thread as well: the scripted tick's hash, which combines the world's with the scripts'.
    [[nodiscard]] virtual StateHash derive(StateHash base, Derivation derivation) = 0;
};

} // namespace eawr::sim
