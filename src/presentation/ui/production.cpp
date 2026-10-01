#include "eawr/presentation/ui/production.hpp"

#include <algorithm>
#include <sstream>
#include "eawr/data/tag_trace.hpp"

namespace eawr::presentation::ui {

core::Result<ReinforcementColours> reinforcement_colours(const vfs::Vfs& filesystem) {
    using Result = core::Result<ReinforcementColours>;
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) return Result::failure(document.error());
    ReinforcementColours result{};
    constexpr std::array<std::string_view, 2> tags{"ReinforcementOverlayGoodColor", "ReinforcementOverlayBadColor"};
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const data::XmlNode* node = nullptr;
        for (const auto& child : document.value().root.children) {
            if (child.name == tags[index]) node = &child;
        }
        data::tag_trace::used(node);
        std::string text = node != nullptr ? node->raw_text : std::string{};
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream values(text);
        std::array<int, 4> channels{};
        bool valid = true;
        for (auto& channel : channels) valid = static_cast<bool>(values >> channel) && channel >= 0 && channel <= 255 && valid;
        std::string extra;
        if (!valid || values >> extra) {
            core::Diagnostic diagnostic;
            diagnostic.code = "EAWR-UI-0313";
            diagnostic.logical_path = "data/xml/gameconstants.xml";
            diagnostic.message = std::string(tags[index]) + " needs four colour channels in [0, 255]";
            return Result::failure(std::move(diagnostic));
        }
        for (std::size_t channel = 0; channel < 3; ++channel) result[index][channel] = 1.5F * static_cast<float>(channels[channel]) / 255.0F;
    }
    return Result::success(result);
}

namespace {

namespace tactical = sim::tactical;

// Q24 to whole credits: rounded to nearest (PU-62) or down (PU-07).
[[nodiscard]] std::int64_t nearest(const sim::math::Fixed value) noexcept {
    const auto raw = value.raw();
    const auto half = sim::math::Fixed::scale / 2;
    return raw >= 0 ? (raw + half) / sim::math::Fixed::scale : -((-raw + half) / sim::math::Fixed::scale);
}

[[nodiscard]] std::int64_t floor_whole(const sim::math::Fixed value) noexcept {
    const auto raw = value.raw();
    const auto whole = raw / sim::math::Fixed::scale;
    return raw < 0 && whole * sim::math::Fixed::scale != raw ? whole - 1 : whole;
}

} // namespace

std::vector<BuildButton> layout_build_buttons(const tactical::StationMenu& menu, const sim::math::Fixed credits,
    const std::array<std::size_t, tactical::build_queue_count>& queue_sizes, const std::size_t max_queue,
    const std::size_t slots) {
    std::vector<BuildButton> buttons;
    for (std::size_t index = 0; index < menu.options.size() && buttons.size() < slots; ++index) {
        const auto& option = menu.options[index];
        BuildButton button;
        button.slot = buttons.size();
        button.option = index;
        button.type = option.type;
        button.price = nearest(option.price);
        const auto queue = static_cast<std::size_t>(option.queue);
        const bool room = queue < queue_sizes.size() && queue_sizes[queue] < max_queue;
        const bool affordable = credits.raw() >= option.price.raw();
        // PU-61: the state says what stops an affordable-or-not button; room first, as FoC sets it.
        if (!affordable) {
            button.state = BuildButtonState::unaffordable;
        } else if (!room) {
            button.state = BuildButtonState::queue_full;
        }
        button.enabled = option.available && affordable && room;
        button.room = room;
        buttons.push_back(button);
    }
    return buttons;
}

std::optional<std::size_t> queue_component(const tactical::BuildQueue queue, const std::size_t index) noexcept {
    if (index >= queue_slot_count) return std::nullopt;
    return queue == tactical::BuildQueue::upgrades ? index : queue_slot_count + index;
}

std::vector<QueueSlot> layout_build_queue(
    const std::array<std::vector<tactical::QueueEntry>, tactical::build_queue_count>& queues, const std::uint64_t frame) {
    std::vector<QueueSlot> slots;
    for (std::size_t queue = 0; queue < queues.size(); ++queue) {
        const auto kind = static_cast<tactical::BuildQueue>(queue);
        const auto& entries = queues[queue];
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto component = queue_component(kind, index);
            if (!component) break;
            QueueSlot slot;
            slot.component = *component;
            slot.queue = kind;
            slot.index = index;
            slot.type = entries[index].type;
            if (index == 0) {
                // PU-64: the front's fraction of its build frames done; "%d%%" truncates.
                const auto& front = entries[index];
                double done = 0.0;
                if (front.frames > 0) {
                    const auto left = front.complete_frame > frame ? front.complete_frame - frame : 0U;
                    const auto remaining = std::min<std::uint64_t>(left, front.frames);
                    done = static_cast<double>(front.frames - remaining) / static_cast<double>(front.frames);
                }
                slot.progress = done;
                slot.percent = std::to_string(static_cast<int>(done * 100.0)) + "%";
            }
            slots.push_back(std::move(slot));
        }
    }
    std::sort(slots.begin(), slots.end(), [](const QueueSlot& left, const QueueSlot& right) {
        return left.component < right.component;
    });
    return slots;
}

std::vector<PoolSlot> layout_pool(const std::span<const tactical::TypeId> pool, const std::uint32_t population,
    const std::uint32_t population_cap, const std::function<std::uint32_t(tactical::TypeId)>& population_of,
    const std::size_t slots) {
    std::vector<PoolSlot> result;
    for (const auto type : pool) {
        const auto found = std::find_if(result.begin(), result.end(), [type](const PoolSlot& slot) { return slot.type == type; });
        if (found != result.end()) {
            ++found->count;
            continue;
        }
        if (result.size() >= slots) continue;
        PoolSlot slot;
        slot.slot = result.size();
        slot.type = type;
        slot.count = 1;
        result.push_back(slot);
    }
    const auto room = population_cap > population ? population_cap - population : 0U;
    for (auto& slot : result) {
        if (slot.count > 1) slot.text = "x" + std::to_string(slot.count);
        const auto value = population_of ? population_of(slot.type) : 0U;
        slot.enabled = value == 0 || value <= room;
    }
    return result;
}

std::size_t pool_rows(const std::size_t filled) noexcept {
    if (filled == 0) return 0;
    return std::min<std::size_t>((filled - 1) / pool_columns, pool_row_limit - 1);
}

std::string credits_text(const sim::math::Fixed credits) {
    return std::to_string(std::max<std::int64_t>(floor_whole(credits), 0));
}

std::string population_text(const std::uint32_t population, const std::uint32_t cap) {
    return std::to_string(population) + "/" + std::to_string(cap);
}

} // namespace eawr::presentation::ui
