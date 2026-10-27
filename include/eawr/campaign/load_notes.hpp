#pragma once
#include "eawr/core/load_note.hpp"
#include "eawr/data/xml.hpp"

namespace eawr::campaign {
// Setup callbacks execute in plan order on the load owner. Partitioned producers
// must use separate buffers and merge them only after joining their phase.
inline void campaign_load_note(core::LoadNoteBuffer* notes, std::string_view code,
    const data::XmlNode* node, std::string_view object, std::string_view tag,
    std::string effective, std::string retail = "unknown",
    core::LoadCategory category = core::LoadCategory::unknown_degrade,
    core::LoadAction action = core::LoadAction::default_used) {
    if (!notes) return;
    core::LoadNote note;
    note.code = code; note.classification = category; note.object = object; note.tag = tag;
    note.action = action; note.effective_value = std::move(effective); note.retail = std::move(retail);
    if (node) {
        note.layer.mod_id = node->source.layer_id;
        note.file = node->source.logical_path; note.line = node->source.line;
        note.authored_value = node->raw_text;
    }
    notes->append(std::move(note));
}
// Typed readers retain diagnostic provenance after parsing the XML value.
template<class Source>
void campaign_type_note(core::LoadNoteBuffer* notes, std::string_view code,
    const Source& source, std::string_view tag, std::string authored, std::string effective,
    std::string retail = "unknown", core::LoadCategory category = core::LoadCategory::unknown_degrade,
    core::LoadAction action = core::LoadAction::clamped) {
    if (!notes) return;
    core::LoadNote note;
    note.code = code; note.classification = category; note.object = source.name;
    note.file = source.file;
    if constexpr (requires { source.line; }) { if (source.line) note.line = source.line; }
    note.tag = tag; note.authored_value = std::move(authored); note.action = action;
    note.effective_value = std::move(effective); note.retail = std::move(retail);
    notes->append(std::move(note));
}
} // namespace eawr::campaign
