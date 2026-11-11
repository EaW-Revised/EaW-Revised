#pragma once

#include "galactic_ai_actions.hpp"
#include "galactic_ai_intent_commit.hpp"

namespace eawr::script::foc::galactic_ai::refusal {

// GFS-A22: a refused effect goes back to its player, once, inside the ordered commit. The
// consequence reuses fields the native owner already persists, so no state is added and no
// format changes (the owner's rule "apply it in the commit slice"):
//   fleet (a refused hop)       the player's unfinished movement block for that fleet finishes
//   production, finish          the retained purchase is marked failed; its goal refunds the
//                               mark through the ordinary failed-build path
//   control, force, invasion, invasion release, orbit recheck, cash, journal
//                               nothing persisted refers to the speculative effect: a speculative
//                               handle resolves absent through the world queries, a refused cash
//                               never happens (the cap only adjusts the delta), and a derived
//                               journal row is dropped by the commit
// Handlers run on the commit thread against the root bound state of `actions`; the caller has
// merged the player's private rows into it before the commit. They are idempotent.
using Handler = std::function<core::Result<bool>(const IntentOutcome&)>;

[[nodiscard]] Handler fleet(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler control(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler cash(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler force(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler production(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler finish(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler invasion(NativeActions&, const IntentRecord&);
[[nodiscard]] Handler orbit_recheck(NativeActions&, const IntentRecord&);
// Dispatches on the intent kind (a journal row has no handler).
[[nodiscard]] Handler for_intent(NativeActions&, const IntentRecord&);

// The hooks a player's turn attaches to one retained intent: for_intent as on_refused. Force and
// production readmission also need their builder / preflight, which only the owner of the player's
// row can build; pass them in `extra`.
[[nodiscard]] IntentHooks standard_hooks(NativeActions&, const IntentRecord&, IntentHooks extra = {});

} // namespace eawr::script::foc::galactic_ai::refusal
