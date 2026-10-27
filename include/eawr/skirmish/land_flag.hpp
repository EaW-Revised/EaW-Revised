#pragma once

// The EAWR_ENABLE_LAND_SKIRMISH build option (#3776 L3): when ON the skirmish setup screen and the LAN lobby
// list land maps. OFF (the release default) leaves every setup list and screen as it was.
#ifndef EAWR_LAND_SKIRMISH
#define EAWR_LAND_SKIRMISH 0
#endif

namespace eawr::skirmish {

inline constexpr bool land_skirmish_enabled = EAWR_LAND_SKIRMISH != 0;

} // namespace eawr::skirmish
