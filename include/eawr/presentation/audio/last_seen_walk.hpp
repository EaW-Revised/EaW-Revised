#pragma once

#include <cstdint>

namespace eawr::presentation::audio {

// Where each unit the player sees stands: for every instance (ascending by entity) the pose map (also ascending)
// either has its pose, which `record(instance, pose)` turns into the unit's last-seen record, or does not, and the
// unit's record is dropped. One forward cursor over the poses replaces a keyed lookup and a copy per instance. The
// result equals the keyed loop for ANY instance order: when an id is lower than the previous one the cursor is
// re-seeked, and duplicates read the same pose twice.
template <class Instances, class Poses, class LastSeen, class Record>
void walk_last_seen(const Instances& instances, const Poses& poses, LastSeen& last_seen, Record&& record) {
    auto pose = poses.begin();
    std::uint64_t previous = 0;
    for (const auto& instance : instances) {
        if (instance.entity_id < previous) pose = poses.lower_bound(instance.entity_id);
        previous = instance.entity_id;
        while (pose != poses.end() && pose->first < instance.entity_id) ++pose;
        if (pose != poses.end() && pose->first == instance.entity_id) {
            last_seen[instance.entity_id] = record(instance, pose->second);
        } else {
            last_seen.erase(instance.entity_id);
        }
    }
}

} // namespace eawr::presentation::audio
