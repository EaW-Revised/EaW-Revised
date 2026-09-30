#pragma once

// Offline, test-only declaration probe for the pinned structurally compatible
// ALA/ALO pairs of the association audit.  For every pinned pair it asks one
// question of the effective XML catalog: does an active, winning, resolved
// object explicitly declare that the candidate model plays the clip's
// animation set?  The answer is metadata only.  Nothing here changes Player,
// the parsers, the XML resolver, baseline selection or the v1 ledger, and no
// disposition is an approval: "evidence_bearing" means a declaration exists
// and is recorded with its provenance, nothing more.
//
// The only link this probe accepts is the one field the catalog has that
// names an animation set: `Land_Model_Anim_Override_Name` (the scene layer
// reads it the same way).  A pair (clip C, candidate K) is linked when a
// resolved object's effective land model (Land_Model_Name, else Model_Name,
// as in the scene layer) is exactly K and its effective override names
// exactly the model whose set the frozen R0 rule attributed C to.  A model
// tag alone, another set, an SFX or icon string, a shadowed definition or a
// name that merely looks similar is recorded as a non-promoting observation.

#include "corpus_associations.hpp"
#include "corpus_diagnostics.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::declarations {

#include "corpus_declarations_other.hpp"
#include "corpus_declarations_space.hpp"
#include "corpus_declarations_land.hpp"

} // namespace eawr::tests::animation_corpus::declarations
