#include "eawr/sim/state_hash.hpp"

#include <utility>

namespace eawr::sim {
namespace {

class ReadyHash final : public StateHash::Source {
public:
    explicit ReadyHash(std::string hex) : hex_(std::move(hex)) {}
    [[nodiscard]] const std::string& get() const override { return hex_; }

private:
    std::string hex_;
};

} // namespace

StateHash::StateHash(std::string ready) : source_(std::make_shared<const ReadyHash>(std::move(ready))) {}

StateHash::StateHash(std::shared_ptr<const Source> pending) noexcept : source_(std::move(pending)) {}

const std::string& StateHash::get() const {
    static const std::string none;
    return source_ ? source_->get() : none;
}

} // namespace eawr::sim
