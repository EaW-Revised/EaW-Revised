#include "eawr/presentation/shader_activation.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace eawr::presentation::shader_activation {
namespace {
report::Route fallback(const Input& input) {
    if (input.form == report::SelectedForm::absent) return report::Route::missing_effect_green;
    return input.adapter.empty() ? report::Route::surface_omitted : report::Route::hand_adapter_approximation;
}
bool same(const Input& a, const Input& b) {
    if (a.cache_key.has_value() != b.cache_key.has_value() ||
        (a.cache_key && a.cache_key->sha256 != b.cache_key->sha256)) return false;
    if ((!a.cache_key && a.context != b.context) || a.snapshot_failure.has_value() != b.snapshot_failure.has_value()) return false;
    if (a.snapshot_failure) {
        const auto& x = *a.snapshot_failure; const auto& y = *b.snapshot_failure;
        if (std::tie(x.stage, x.reason, x.severity, x.logical) != std::tie(y.stage, y.reason, y.severity, y.logical) ||
            x.position.has_value() != y.position.has_value()) return false;
        if (x.position && (x.position->line != y.position->line || x.position->column != y.position->column)) return false;
    }
    return std::tie(a.effect, a.sha256, a.layer, a.mount, a.logical, a.origin, a.form, a.policy, a.adapter, a.bytes) ==
        std::tie(b.effect, b.sha256, b.layer, b.mount, b.logical, b.origin, b.form, b.policy, b.adapter, b.bytes);
}
}
Job::Job(std::uint64_t id, std::string chain_identity, std::vector<Input> inputs,
    Operations operations, Limits limits)
    : generation_(std::make_shared<Generation>()), operations_(std::move(operations)), limits_(limits) {
    generation_->id = id; generation_->chain_identity = std::move(chain_identity);
    if (id == 0 || generation_->chain_identity.size() > limits_.identifier_bytes || inputs.size() > limits_.effects) {
        progress_.state = State::failed; return;
    }
    std::size_t bytes = 0;
    for (const auto& input : inputs) {
        for (const auto* text : {&input.effect, &input.sha256, &input.layer, &input.mount, &input.logical, &input.origin, &input.adapter}) {
            if (text->size() > limits_.identifier_bytes) { progress_.state = State::failed; return; }
        }
        if (input.effect.empty() || input.bytes.size() > limits_.input_bytes - bytes) { progress_.state = State::failed; return; }
        bytes += input.bytes.size();
        if (input.cache_key && (input.cache_key->sha256.size() != 64 ||
            !std::all_of(input.cache_key->sha256.begin(), input.cache_key->sha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))) { progress_.state = State::failed; return; }
        if (input.snapshot_failure && input.snapshot_failure->logical.size() > limits_.identifier_bytes) {
            progress_.state = State::failed; return;
        }
        if (input.form == report::SelectedForm::absent || input.policy != Policy::allowed || input.snapshot_failure) {
            if (!input.bytes.empty() || !input.sha256.empty()) { progress_.state = State::failed; return; }
        } else if ((input.form != report::SelectedForm::compiled && input.form != report::SelectedForm::source) ||
            input.sha256 != core::sha256_hex({reinterpret_cast<const std::uint8_t*>(input.bytes.data()), input.bytes.size()})) {
            progress_.state = State::failed; return;
        }
    }
    const auto key = [](const Input& input) -> std::string_view { return input.cache_key ? input.cache_key->sha256 : std::string_view{}; };
    std::sort(inputs.begin(), inputs.end(), [&](const Input& a, const Input& b) {
        return std::pair(std::string_view(a.effect), key(a)) < std::pair(std::string_view(b.effect), key(b));
    });
    for (auto& input : inputs) {
        if (!generation_->effects.empty() && generation_->effects.back().input.effect == input.effect && key(generation_->effects.back().input) == key(input)) {
            if (!same(generation_->effects.back().input, input)) { progress_.state = State::failed; return; }
            continue;
        }
        const auto selected_fallback = fallback(input);
        generation_->effects.push_back({std::move(input), {}, {}, selected_fallback});
    }
    progress_.effects = generation_->effects.size();
    if (progress_.effects == 0) progress_.state = State::complete;
}
bool Job::retain(std::size_t count) {
    if (count > limits_.diagnostics - diagnostic_count_) { progress_.state = State::failed; return false; }
    diagnostic_count_ += count; return true;
}
void Job::advance() {
    // Published resources own their actual dependencies. Raw input/expanded
    // compiler text is transient and must not remain pinned by old generations.
    std::vector<std::byte>().swap(generation_->effects[effect_].input.bytes);
    generation_->effects[effect_].input.context.reset();
    prepared_ = {}; preparing_ = true; variant_ = 0; ++effect_;
    if (effect_ == generation_->effects.size()) progress_.state = State::complete;
}
void Job::cancel() noexcept { cancellation_.requested.store(true); }
void Job::step(std::size_t work_budget) {
    if (progress_.state != State::running) return;
    if (cancellation_.cancelled()) { progress_.state = State::cancelled; return; }
    while (work_budget != 0 && progress_.state == State::running) {
        --work_budget;
        auto& effect = generation_->effects[effect_];
        ++progress_.work;
        try {
            if (preparing_) {
                ++progress_.prepared;
                if (effect.input.form == report::SelectedForm::absent || effect.input.policy != Policy::allowed) {
                    const auto reason = effect.input.form == report::SelectedForm::absent ? report::Reason::missing_effect :
                        effect.input.policy == Policy::opted_out ? report::Reason::opted_out : report::Reason::malformed_policy;
                    if (!retain(1)) return;
                    effect.diagnostics.push_back({report::FailureStage::selection, reason, report::Severity::warning});
                    advance();
                } else if (effect.input.snapshot_failure) {
                    if (effect.input.snapshot_failure->reason == report::Reason::cancelled) { progress_.state = State::cancelled; return; }
                    if (!retain(1)) return;
                    effect.diagnostics.push_back(*effect.input.snapshot_failure); advance();
                } else {
                    if (!operations_.prepare) { progress_.state = State::failed; return; }
                    prepared_ = operations_.prepare(effect.input, cancellation_);
                    if (cancellation_.cancelled()) { progress_.state = State::cancelled; return; }
                    if (std::any_of(prepared_.diagnostics.begin(), prepared_.diagnostics.end(), [](const Diagnostic& item) {
                        return item.reason == report::Reason::cancelled;
                    })) { progress_.state = State::cancelled; return; }
                    if (!retain(prepared_.diagnostics.size())) return;
                    effect.diagnostics = std::move(prepared_.diagnostics);
                    if (prepared_.variants.size() > limits_.variant_requests) { progress_.state = State::failed; return; }
                    std::sort(prepared_.variants.begin(), prepared_.variants.end());
                    prepared_.variants.erase(std::unique(prepared_.variants.begin(), prepared_.variants.end()), prepared_.variants.end());
                    if (prepared_.variants.size() > limits_.variants - progress_.variants) { progress_.state = State::failed; return; }
                    if (!prepared_.artifact || prepared_.variants.empty()) {
                        if (effect.diagnostics.empty()) {
                            if (!retain(1)) return;
                            effect.diagnostics.push_back({report::FailureStage::technique_admission, report::Reason::no_fallback, report::Severity::error});
                        }
                        advance();
                    } else {
                        progress_.variants += prepared_.variants.size();
                        effect.variants.reserve(prepared_.variants.size()); preparing_ = false;
                    }
                }
            } else {
                if (!operations_.admit) { progress_.state = State::failed; return; }
                auto admitted = operations_.admit(effect.input, prepared_, prepared_.variants[variant_], cancellation_);
                if (cancellation_.cancelled()) { progress_.state = State::cancelled; return; }
                if (std::any_of(admitted.diagnostics.begin(), admitted.diagnostics.end(), [](const Diagnostic& item) {
                    return item.reason == report::Reason::cancelled;
                })) { progress_.state = State::cancelled; return; }
                if (!retain(admitted.diagnostics.size())) return;
                VariantResult result{prepared_.variants[variant_], effect.fallback, std::move(admitted.artifact), std::move(admitted.diagnostics)};
                if (result.artifact) { result.route = report::Route::translated; ++progress_.admitted; }
                else {
                    ++progress_.failed_variants;
                    if (result.diagnostics.empty()) {
                        if (!retain(1)) return;
                        result.diagnostics.push_back({report::FailureStage::godot_admission, report::Reason::admission_failure, report::Severity::error});
                    }
                }
                effect.variants.push_back(std::move(result));
                if (++variant_ == prepared_.variants.size()) advance();
            }
        } catch (...) {
            // Unexpected owner/allocator failure invalidates the candidate
            // generation. No partially processed generation may be published.
            progress_.state = State::failed; return;
        }
        if (cancellation_.cancelled()) progress_.state = State::cancelled;
    }
}
std::shared_ptr<const Generation> Job::result() const {
    return progress_.state == State::complete && !cancellation_.cancelled() ? generation_ : nullptr;
}
std::uint64_t Publisher::begin() {
    const std::lock_guard lock(mutex_);
    if (latest_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("shader generation exhausted");
    cancelled_ = false; return ++latest_;
}
void Publisher::cancel(std::uint64_t id) {
    const std::lock_guard lock(mutex_);
    if (id == latest_) cancelled_ = true;
}
bool Publisher::publish(const Job& job) {
    const std::lock_guard lock(mutex_);
    const auto candidate = job.result();
    if (!candidate || candidate->id != latest_ || cancelled_ || (active_ && active_->id == latest_)) return false;
    active_ = candidate; return true;
}
std::shared_ptr<const Generation> Publisher::active() const {
    const std::lock_guard lock(mutex_); return active_;
}
report::Route route(const EffectResult& effect, Variant variant) noexcept {
    const auto found = std::lower_bound(effect.variants.begin(), effect.variants.end(), variant,
        [](const VariantResult& candidate, Variant requested) { return candidate.variant < requested; });
    return found != effect.variants.end() && found->variant == variant ? found->route : effect.fallback;
}
} // namespace eawr::presentation::shader_activation
