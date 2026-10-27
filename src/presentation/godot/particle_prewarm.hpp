#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

struct ParticleWarmCoverage final {
    std::size_t requested{}, admitted{}, refused{};
};

// Population-only pass. The backend qualifies/deduplicates actual creation
// identity and owns its warm holds; no logical effect, seed or clock is touched.
// A refusal must not prevent a later shared or smaller plan from warming.
template<class Plans, class Warm>
[[nodiscard]] ParticleWarmCoverage prewarm_particle_plans(const Plans& plans, Warm&& warm) {
    ParticleWarmCoverage coverage;
    for (const auto& plan : plans) {
        if (!plan.drawable) continue;
        ++coverage.requested;
        if (std::invoke(warm, plan)) ++coverage.admitted;
        else ++coverage.refused;
    }
    return coverage;
}

// GPU creation identity only; no effect name, RNG, age or geometry is retained.
struct ParticleWarmPolicy final {
    std::string texture, normal, program;
    std::int32_t priority{};
    friend bool operator<(const ParticleWarmPolicy& a, const ParticleWarmPolicy& b) {
        return std::tie(a.texture,a.normal,a.program,a.priority)<std::tie(b.texture,b.normal,b.program,b.priority);
    }
};

class ParticleWarmBudget final {
public:
    static constexpr std::size_t max_emitters=32;
    static constexpr std::uint64_t max_texture_bytes=8U*1024U*1024U;
    using TextureCharge=std::pair<std::string,std::uint64_t>;
    explicit ParticleWarmBudget(const std::size_t policies = max_emitters,
                                const std::uint64_t bytes = max_texture_bytes)
        : policy_limit_(policies), texture_limit_(bytes) {}
    [[nodiscard]] std::size_t policy_limit() const { return policy_limit_; }
    [[nodiscard]] std::uint64_t texture_limit() const { return texture_limit_; }
    [[nodiscard]] bool contains(const ParticleWarmPolicy& policy) const { return policies_.contains(policy); }
    [[nodiscard]] bool admits(const ParticleWarmPolicy& policy,std::span<const TextureCharge> charges) const {
        if(contains(policy))return true;
        if(policies_.size()>=policy_limit_)return false;
        std::uint64_t extra{};
        for(std::size_t index=0;index<charges.size();++index){
            const auto& [name,bytes]=charges[index];
            if(textures_.contains(name))continue;
            bool repeated{};
            for(std::size_t earlier=0;earlier<index;++earlier)repeated|=charges[earlier].first==name;
            if(repeated)continue;
            if(bytes>texture_limit_-bytes_ || extra>texture_limit_-bytes_-bytes)return false;
            extra+=bytes;
        }
        return true;
    }
    // Call only after actual backend creation succeeded.
    void commit(const ParticleWarmPolicy& policy,std::span<const TextureCharge> charges){
        policies_.insert(policy);
        for(const auto& [name,bytes]:charges)if(textures_.emplace(name,bytes).second)bytes_+=bytes;
    }
    void clear(){policies_.clear();textures_.clear();bytes_=0;}
    [[nodiscard]] std::size_t emitters() const { return policies_.size(); }
    [[nodiscard]] std::uint64_t texture_bytes() const { return bytes_; }
private:
    std::size_t policy_limit_;
    std::uint64_t texture_limit_;
    std::set<ParticleWarmPolicy> policies_;
    std::map<std::string,std::uint64_t> textures_;
    std::uint64_t bytes_{};
};

// Source demand is inventoried before admission, including refused/missing
// policies. Counts are authored creation identities, matching the backend.
class ParticleWarmDemand final {
public:
    void observe(const ParticleWarmPolicy& policy,
                 std::span<const ParticleWarmBudget::TextureCharge> charges, const bool resolved) {
        ++references_;
        policies_.insert(policy);
        if (!resolved) unresolved_.insert(policy);
        for (const auto& [name, bytes] : charges) {
            if (!textures_.emplace(name, bytes).second) continue;
            if (bytes > std::numeric_limits<std::uint64_t>::max() - bytes_) overflow_ = true;
            else bytes_ += bytes;
        }
    }
    void clear() { *this = ParticleWarmDemand{}; }
    [[nodiscard]] std::size_t references() const { return references_; }
    [[nodiscard]] std::size_t policies() const { return policies_.size(); }
    [[nodiscard]] std::size_t textures() const { return textures_.size(); }
    [[nodiscard]] std::size_t unresolved() const { return unresolved_.size(); }
    [[nodiscard]] std::uint64_t texture_bytes() const { return bytes_; }
    [[nodiscard]] bool complete() const { return unresolved_.empty() && !overflow_; }
private:
    std::size_t references_{};
    std::set<ParticleWarmPolicy> policies_, unresolved_;
    std::map<std::string, std::uint64_t> textures_;
    std::uint64_t bytes_{};
    bool overflow_{};
};

// Shader-only holds do not consume the texture-bearing emitter allowance.
class ParticleProgramWarmBudget final {
public:
    static constexpr std::size_t max_programs = 32;
    using Programs = std::set<std::string, std::less<>>;
    [[nodiscard]] bool contains(const std::string_view program) const { return programs_.contains(program); }
    [[nodiscard]] bool admits(const std::string_view program) const {
        return contains(program) || !full();
    }
    [[nodiscard]] bool full() const { return programs_.size() >= max_programs; }
    // Call only after an admitted backend acquisition succeeds.
    void commit(const std::string& program) { programs_.insert(program); }
    void clear() { programs_.clear(); }
    [[nodiscard]] std::size_t size() const { return programs_.size(); }
    [[nodiscard]] const Programs& programs() const { return programs_; }
private:
    Programs programs_;
};

} // namespace eawr::presentation::godot_backend::detail
