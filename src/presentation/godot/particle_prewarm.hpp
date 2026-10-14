#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

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
    [[nodiscard]] bool contains(const ParticleWarmPolicy& policy) const { return policies_.contains(policy); }
    [[nodiscard]] bool admits(const ParticleWarmPolicy& policy,std::span<const TextureCharge> charges) const {
        if(contains(policy))return true;
        if(policies_.size()>=max_emitters)return false;
        std::uint64_t extra{};
        for(std::size_t index=0;index<charges.size();++index){
            const auto& [name,bytes]=charges[index];
            if(textures_.contains(name))continue;
            bool repeated{};
            for(std::size_t earlier=0;earlier<index;++earlier)repeated|=charges[earlier].first==name;
            if(repeated)continue;
            if(bytes>max_texture_bytes-bytes_ || extra>max_texture_bytes-bytes_-bytes)return false;
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
    std::set<ParticleWarmPolicy> policies_;
    std::map<std::string,std::uint64_t> textures_;
    std::uint64_t bytes_{};
};

} // namespace eawr::presentation::godot_backend::detail
