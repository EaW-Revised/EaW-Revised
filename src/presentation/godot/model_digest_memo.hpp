#pragma once

#include "eawr/assets/assets.hpp"
#include "model_recipe.hpp"
#include "upload_identity.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <unordered_map>

namespace eawr::presentation::godot_backend::detail {

// The upload digest of each shared model object (#5197): every surface of one model asks for the same digest, so it is
// computed once. An entry holds its model weakly and answers only for the exact object it was made for, so a freed
// model's address being reused by another model recomputes.
class ModelDigestMemo final {
public:
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] UploadIdentity identity(const std::shared_ptr<const assets::Model>& model) {
        return identity(model, [](const assets::Model& value) { return model_identity(value); });
    }

    // `compute` is model_identity in production; a counting stand-in in the contract test.
    template <class Compute>
    [[nodiscard]] UploadIdentity identity(const std::shared_ptr<const assets::Model>& model, Compute&& compute) {
        if (const auto found = entries_.find(model.get()); found != entries_.end() && found->second.owner.lock() == model) {
            return found->second.identity;
        }
        if (entries_.size() >= prune_above_) {
            std::erase_if(entries_, [](const auto& entry) { return entry.second.owner.expired(); });
            // Live entries stay: the next scan waits until the table has doubled, so a large live set is not rescanned per insert.
            prune_above_ = std::max<std::size_t>(minimum_prune_above, entries_.size() * 2);
        }
        const UploadIdentity result = compute(*model);
        entries_.insert_or_assign(model.get(), Entry{model, result});
        return result;
    }

private:
    struct Entry final {
        std::weak_ptr<const assets::Model> owner;
        UploadIdentity identity{};
    };
    static constexpr std::size_t minimum_prune_above = 4096;
    std::size_t prune_above_{minimum_prune_above};
    std::unordered_map<const assets::Model*, Entry> entries_;
};

} // namespace eawr::presentation::godot_backend::detail
