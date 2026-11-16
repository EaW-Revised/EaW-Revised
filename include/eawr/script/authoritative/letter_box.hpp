#pragma once

// Letter_Box_In(seconds) / Letter_Box_Out(seconds) (legacy #6634, HCB-72 and HCB-75): the one binding both script hosts register, so a
// galactic story script and a tactical script are checked alike. The call only stages a command of the host's own verb
// (arguments "in" or "out", then the seconds as text); the host turns it into a local presentation request. Nothing here
// reaches the simulation.

#include "eawr/script/authoritative/scheduler.hpp"

#include <string>
#include <string_view>

namespace eawr::script::authoritative {

// The verb a tactical script host stages (a presentation verb of ScriptedTacticalSession, never world input).
inline constexpr std::string_view verb_tactical_letter_box = "foc.letter_box";

// `name` is the Lua name (Letter_Box_In or Letter_Box_Out), `direction` "in" or "out", `verb` the command verb the host
// recognises. A call without exactly one finite, non-negative number is a named script fault for that thread, never a
// match abort. The seconds are written locale-free (shortest 6-digit general form of the fixed-point value).
[[nodiscard]] Binding letter_box_binding(std::string name, std::string direction, std::string verb);

} // namespace eawr::script::authoritative
