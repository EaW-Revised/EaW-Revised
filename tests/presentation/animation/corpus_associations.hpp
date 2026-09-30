#pragma once

// Offline, test-only alternative-candidate validation for frozen baseline
// failures.  For each baseline binding failure it evaluates every additional
// R0 candidate with strict Player::create, the full diagnose_binding mirror
// and fixed-time sampling, and records the result as metadata.  Nothing here
// changes baseline selection, Player, parsers or the v1 ledger: a candidate
// that binds is "compatible_unapproved", never an association.  Approval needs
// separate, provenance-bearing evidence (for example an effective XML
// declaration) and is deliberately not representable in this receipt.

#include "corpus_diagnostics.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace eawr::tests::animation_corpus::associations {
#include "corpus_associations_baseline.hpp"
#include "corpus_associations_candidates.hpp"

} // namespace eawr::tests::animation_corpus::associations
