# Land combat slice

The first land shooting slice binds the authored land member profiles to the shared
weapon, projectile and damage services (LC-26/27/30, LC-33/34, WWP-20, LR2-02/12/15/16).
Move, Stop and Attack are the supported orders. Attack names an owned squad and a
living hostile member or squad; the order is copied to its living members. A refused
replacement preserves the previous order and path. AttackMove and Guard remain refused.

Movement publishes copied member poses before combat. A partitioned target phase
selects each squad's ordered target or a nearest hostile member inside the leader's
authored planar attack distance plus target geometry; equal distances use entity ID.
Partitioned member weapon service reads the shared choice. Ordered commits assign shot
identities and deliver the existing shared direct and blast damage. Live shots begin
flying on the following tick. Terrain contact truncates a flight before blast recipient
preparation; an object wins an exact tie. A terrain/water terminal publishes one
projectile-expired event with ground-contact reason 6, at that contact, and detonates
through the shared blast service once.

The following choices remain unqualified retail questions. Their named constants in
`session_land_combat_internal.hpp` are replaceable seams, not retail equivalence claims:

- Q1: the first living member chooses one target shared with its squad. A squad named
  as an attack target resolves to its nearest living member. Full land priority-set,
  target replacement and special-attack branches remain outside this slice.
- Q2: LC-30 supplies a one-second scan interval plus keyed jitter up to half a second.
  Candidate ties use stable entity IDs; retail player/candidate order is unqualified.
- Q3: idle units do not chase. Out-of-range Attack retains the order and emits a
  diagnostic explaining that the player must move into range. No automatic path is
  inferred from the space order service.
- Q4: death frees movement/contact and occupancy state immediately. Surviving slots
  retain their offsets. Empty squad roots, requests and pending searches are removed.
  Corpse/death-clone selection and persistence belong to the presentation follow-up.
- Q5: object-weapon muzzles use the member origin until animated land bone poses exist;
  authored target Z adjustment, range, pulse timing, scatter and damage remain effective.
- Q6: terrain/water queries retain the explicit unqualified boundary policies in L1.
- Q7: no cover/surface/high-ground combat modifiers, fog, LOS, crush, garrison, abilities,
  reinforcement, victory/retreat, AI or hardpoint-vehicle combat are added here.

Land checkpoint section 18 is version 6. It retains each living squad's shared target
and scan deadline. Existing child-state records already retain member health/weapons,
live projectiles, projectile identity allocation and delayed damage; their retained
partition buffers also feed canonical hashes. Restore validates surviving identities,
slots and squad bindings against content instead of requiring the original roster size.
Land content domain 3 and adapter-v5 distinguish this active-combat policy. Space does
not emit the land section or use these policies; replay comparisons require equal space bytes.
