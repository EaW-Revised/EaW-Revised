# Skirmish ship availability

The owner requested a temporary supported ship roster for the next build, with
unsupported mechanics restored after economy work. This is a remake release
policy, not a claim about the original game's availability rules.

| Rule | Policy | Evidence |
| --- | --- | --- |
| RG-01 | Gate an obtainable ship only for an unsupported primary weapon, locomotor, deployment lifecycle, or required launched/spawned object. Minor tag registry gaps never gate ships. | Owner release policy; unit census UC-01..06 |
| RG-02 | Each primary weapon must fire its authored projectile with its damage and guidance. Unsupported hardpoint families or unreviewed projectile capabilities gate the ship. | Reviewed `src/units/unit_combat.cpp` and `src/units/unit_tables.cpp`; mass-driver weapons are supported under MD-01..06 and blast-area damage under WAD-01..30 (`src/sim/tactical/blast.cpp`), with Diamond Boron rocket termination under WAD-04. Special hardpoints remain gated. |
| RG-03 | Disable an unsupported optional ability with a reason, retaining its ship. A projectile spawned only by that disabled ability does not gate the ship. Required craft, containers and carrier launches propagate gates recursively. | Owner policy; `ability_table` and the supported simulation dispatch; BARRAGE point activation BARR-01/WAD-38; squadron and carrier services FL-03 and AB-60. |
| RG-04 | Keep gated entries visible and disabled in the station's authored list, with their reason as a tooltip. Reject human/AI purchases and reinforcement commands, and omit gated types from AI discovery. Station upgrade purchases and level replacements also check the disabled type flags, including the replacement destination. | Owner policy; existing purchasing button layers PU-61. |
| RG-05 | Skip gated default forces and authored fleet entries at skirmish creation. Retain required station infrastructure and record its weapon gaps separately. | Coordinator's accepted infrastructure exception to owner release policy. |

`data/census/roster-review.json` records reviewed support where census registry
statuses cannot decide gameplay. The shield-leech hardpoint has no ordinary
projectile: its disabled optional ability is its entire action. It does not add a
second gate to the mass-driver ship. Carrier lists above Tech_0 are outside the
current skirmish tech state. Station menu unlocks remain the station upgrade
service's responsibility; ship support is independent of its presence.

`tools/inventory/roster_gate.py` generates the single runtime policy file,
`data/skirmish/roster-gate.json`. Its disabled units and ability reasons are stable
JSON records; each disabled ship has a detailed diagnostic reason and a short
player-facing tooltip without internal XML identifiers or tracking numbers. The
file has stable bytes with LF line endings. Configure generates an untracked C++ table from
that file; no game installation or live census is needed to build. Queries use
case-insensitive binary search without allocations. Reinforcement checks use a
sorted type-ID list; no simulation entity pass is added.

After implementing a capability, update the reviewed data and regenerate the
policy. Adding `mass-driver` to `supported_capabilities` re-enables ships whose
only blocker is mass drivers and clears station mass-driver gaps. Adding
`blast-damage` re-enables Broadside and Marauder with their ordinary
Diamond Boron missiles. Krayt and Peacebringer remain gated by their SPECIAL
hardpoints. Broadside and Marauder BARRAGE is admitted under RG-03: the proxy and
projectile override, authored timers, fixed scatter, height offset and
`FIRE_RATE_MULTIPLIER` are applied under WAD-38. Its button targets a visible world
point through BARR-01. Unsupported abilities and modifiers
are excluded from skirmish ability profiles while their authored buttons remain
disabled with explanations.

The mass-driver capability is enabled: Kedalbe and Vengeance can be deployed,
and the five Underworld stations retain their supported mass-driver batteries.
Their unsupported optional abilities keep separate disabled-button reasons.

Regenerate with `python tools/inventory/roster_gate.py`. The current-data CTest
checks exact bytes without retail data; the game-data test computes the gate from
a fresh census and skips without `EAWR_EAW_GAME_ROOT`. The census itself retains
its independent full staleness check. Existing gameplay, XML and presentation tag
semantics are unchanged; this policy applies no new XML tags.
