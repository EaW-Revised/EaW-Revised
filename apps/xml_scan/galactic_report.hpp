#pragma once

#include "eawr/data/galactic/galactic_data.hpp"

#include <filesystem>

namespace xml_scan {

// `--galactic-report`: the galactic tables of the loaded install as JSON (docs/galactic-data.md).
[[nodiscard]] bool write_galactic_report(const std::filesystem::path& path,
                                         const eawr::data::galactic::GalacticData& data);

} // namespace xml_scan
