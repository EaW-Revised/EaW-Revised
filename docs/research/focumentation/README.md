# Focumentation wiki cross-check

Research note, 2026-10-08. The community modding wiki
[Focumentation](https://focumentation.fandom.com/wiki/Empire_at_War_Modding_Hub_Wiki) was
downloaded and compared with our behaviour notes, tag registry and Lua inventories.

## Source and licence

- The wiki text is CC BY-SA (Fandom licensing). It is **not** copied into this repository. The
  local copy is research input under the ignored `out/research/focumentation/`, produced by
  `python3 tools/fetch_focumentation.py`. That writes one wikitext file per page and a
  `MANIFEST.json` with the page and revision IDs, timestamps, SHA-256 and URLs. The download on
  2026-10-08 has 154 pages (namespaces main, project, template and category).
- The notes here are paraphrases in our own words and cite wiki pages by title. Tag, enum and
  Lua names are quoted only as identifiers.
- The wiki is **secondary evidence**. It was written by modders against retail behaviour,
  partly base EaW, with question marks the authors placed themselves. A rule backed by the debug
  build stands until a debug-build read or a rig capture settles a conflict. The AI pages appear
  to be a copy of the AI guide that `docs/behaviour/foc-tactical-ai.md` already cites. They are
  a public citation for that guide, not independent confirmation of it.

## Findings

| File | Wiki area | Conflicts | New | Agrees |
|---|---|---:|---:|---:|
| [combat-abilities.md](combat-abilities.md) | abilities, modifiers, projectiles, behaviours, XML tag tables | 11 | 22 | 30 |
| [ai.md](ai.md) | AI framework: goals, plans, taskforces, budgets, freestores, perception | 16 | 21 | 45 |
| [lua-story-gc.md](lua-story-gc.md) | Lua commands, story scripting, galactic conquest | 26 | 54 | ~70 |
| [presentation-assets.md](presentation-assets.md) | particles, animation, SFX tags, models, tools, mod loading | 6 | 18 | 32 |

Each file lists its conflicts, the new items grouped by the doc that should own them (marked
M2, M3 or later), and a suggested next step for each.

## Mechanical cross-reference

- **XML tags.** The wiki's full tag list has about 3,200 names. Almost all of them are in our
  retail-XML inventory (`plan/inventories/xml-tags.json`). Ten real tag names on the wiki do not
  appear in stock data, so our inventory-built registry cannot see them:
  `Cancel_Special_Ability`, `Corrupt_Planet_Ability`, `Fadeout_End_Frame`,
  `Land_Position_LeftTop`, `Overrun_Auto_Resolve_Multiple`, `Required_Corruption_Level`,
  `Respawn_Tied_To_Structure`, `Space_Position_LeftTop`, `Transition_Time_In_Secs` and
  `Perceptual_Function_Name`. Mods may author them. (The rest of the 47 unmatched names are enum
  values, faction names or wiki placeholders.)
- **Registry conflicts.** The wiki documents a use for tags that our registry marks
  `foc-ignores` on some class: `SpaceAutoResolveHitRate`, `SpaceAutoResolveStunRate`,
  `Lighting_Effect_Name`, `Is_Pulsing`, `Must_Be_Bought_On_Black_Market`, `Mod_Multiplier`
  and `Fire_Inaccuracy_Distance` (the last two have reviewed exceptions). The AI and story
  comparisons add `SpaceFreeStoreScript` / `LandFreeStoreScript` and the galactic markup hint
  tags.
- **Lua.** The wiki documents 168 names. Every function among them is in our debug-build
  registration list (`plan/inventories/foc-lua-registrations.json`); the four exceptions are
  script globals (`ServiceRate`, `LuaThreadTable`, `LuaWrapperMetaTable`) and one extractor
  artefact. Our list has 256 more names than the wiki's, so the wiki adds behaviour details,
  not names. The remake binds 34 of the 168.

## Follow-up

Verification work is tracked as research epic (legacy EAWR-2163) on GitHub, with sub-issues (legacy #2164-#2171)
per area. Each one checked the conflicts and M2/M3 items against the debug build, stock XML or
the rig, under the [clean-room rule](../../clean-room.md). The verdicts are recorded as a
`Verdict` column (tables) or a bracketed tail (bullets) in the four findings files, and
summarised below.

## Verification status (2026-10-08)

Verdict cells give the short verdict, the rule IDs where it was recorded, tickets and the
sub-issue. Rule IDs (WST-, WGA-, AB-, PS- and so on) live in the behaviour notes under `docs/behaviour/`
and are plain text here, not links. Items no sub-issue covered read "not checked".

| Sub-issue | Area | Settled, by verdict | Tickets filed | Captures needed |
|---|---|---|---|---|
| (legacy EAWR-2164) | Tag registry rows, unauthored tags | wiki right 4, ours right 3 (+2 unchanged), both partly 1; 9 of 10 unauthored names read by the debug build, 1 not a tag | none (existing (legacy EAWR-737, EAWR-1121, EAWR-1130, EAWR-760)) | none |
| (legacy EAWR-2165) | Ability modifier vocabulary, black-market gate | both partly 1, ours right 2, wiki right 1, stale own docs fixed 2 | (legacy EAWR-2194) | none |
| (legacy EAWR-2166) | Abilities, projectiles, barrage, unit cards | wiki right 2, confirmed 3, ours right 2, both partly 2; deferred M3/M4 8, not in scope 5 | (legacy EAWR-2190, EAWR-2191, EAWR-2192, EAWR-2193) | 4 |
| (legacy EAWR-2167) | Tactical AI | wiki right 3, ours right 5, both partly 4, noted 1 | (legacy EAWR-2188) | 1 |
| (legacy EAWR-2168) | Lua API | confirmed 8, partly confirmed 7, wiki right 2, ours right 1, both partly 3, documented 1; open 8 | (legacy EAWR-2177, EAWR-2178, EAWR-2179) | 2 |
| (legacy EAWR-2169) | Story scripting | conflicts: wiki right 6, ours right 12, both partly 2; NEW S1-S21 and G8/G9: settled about 17, open 6; C-L4 open | none | 1 |
| (legacy EAWR-2170) | Galactic AI, markup, presentation, victory | wiki right 7, ours right 3, both partly 4, covered/settled 4; open 2 | none new (existing (legacy EAWR-1121, EAWR-1114, EAWR-1150, EAWR-1991)) | 4 |
| (legacy EAWR-2171) | Particles, proxies, audio tags | wiki right 4, ours right 2, both partly 2, done 1 | (legacy EAWR-2176) | none (3 optional runtime checks) |

Counts are per report line, so an item two sub-issues touched is counted in both.

### Tickets filed

- Interdictor effects never shown (proxy emitter prefixes) (legacy EAWR-2176), from (legacy EAWR-2171).
- Reachable_Target returns the AI target (M3; also affects M2 freestore scripts) (legacy EAWR-2177), from (legacy EAWR-2168).
- GameRandom range and GlobalValue.Set value rules (M2) (legacy EAWR-2178), from (legacy EAWR-2168).
- sandbox rule for the wrapper metatable global (nice-to-have) (legacy EAWR-2179), from (legacy EAWR-2168).
- owner question: how the tag registry should carry engine tags that stock data never authors (the (legacy EAWR-2164) proposal; options A flagged rows, B a generated engine tag list, C defer to (legacy EAWR-1286)) (legacy EAWR-2187).
- hero plan attach, with superweapon AI (legacy EAWR-2188, EAWR-1943), from (legacy EAWR-2167).
- Interdictor (legacy EAWR-2190), cluster bomb (legacy EAWR-2191), TIE Scout reveal ping (legacy EAWR-2192), unit-card tooltip popup (legacy EAWR-2193) (all M2), from (legacy EAWR-2166).
- loader drops an ability with an unmodelled modifier name (M2, small) (legacy EAWR-2194), from (legacy EAWR-2165).

Existing tickets the verdicts point at: freestore script (legacy EAWR-737), markup/hints (legacy EAWR-1121), black-market gate, M3 (legacy EAWR-1130),
Enable_Ability rows (legacy EAWR-760), political transition bonus (legacy EAWR-1114), victory parser (legacy EAWR-1150), GC-only Lua guards and thread functions (legacy EAWR-1991).

### Capture-needed recipes (rule ID that holds the recipe)

- Barrage range source (BARR-U1).
- Missile shield versus direct shots (WMS-U1).
- Interdict with skirmish retreat (WIN-U1).
- Reveal ping on the TIE Scout (WFP-U1).
- Variable_Enemy in a free-for-all or 1v2 (AI-G09).
- Underworld chatter blocked by notification parameter (WST-U14).
- Is_Thread_Active after a thread ends (L-50).
- ServiceRate at exact frame multiples (L-29).
- Tarkin political transition timing (WGM-197).
- Missing markup file plus save and reload (U-01 in gc-ai).
- Game.Age at the first AI service in a GC-launched battle (WGA-52).
- Zoomed terrain indices (U-13 in gc-presentation).
- Optional runtime checks from (legacy EAWR-2171): two-emitter lifetime test (PS-50), space debris at z=0 (PS-48), field generator prs emitter (BP-73).

### Items not in a findings table

- Registry conflicts from the mechanical cross-reference: SpaceAutoResolveHitRate and
  SpaceAutoResolveStunRate (ours right, foc-ignores kept), SpaceFreeStoreScript and
  LandFreeStoreScript (wiki right, todo (legacy EAWR-737)), Lighting_Effect_Name (ours right: the engine reads
  the other spelling), Is_Pulsing (ours right), Must_Be_Bought_On_Black_Market (both partly,
  M3 gate (legacy EAWR-1130)), Mod_Multiplier and Fire_Inaccuracy_Distance (unchanged, AB-15 and DG-24 stand)
  (legacy EAWR-2164).
- Unauthored tags (legacy EAWR-2164): nine of the ten are read by the debug build (Cancel_Special_Ability,
  Corrupt_Planet_Ability, Required_Corruption_Level, Transition_Time_In_Secs, Fadeout_End_Frame,
  Space_Position_LeftTop, Land_Position_LeftTop, Overrun_Auto_Resolve_Multiple,
  Respawn_Tied_To_Structure); Perceptual_Function_Name is a wiki placeholder. The eight
  unauthored SFX tags (legacy EAWR-2171) and the story Event_Filter tag (legacy EAWR-2169) are engine-accepted too.
  Owner proposal: either mark such registry rows "unauthored" or generate an engine-tag list from
  the debug-build tag tables (recommended; serves (legacy EAWR-1286)). Not applied.
- The older own-document mismatch in `docs/rendering.md` (particle pre-roll) was stale and fixed (legacy EAWR-2171).
- ContainsHero token stub: ours right only with no hero on the map (PE-22, HA-06) (legacy EAWR-2167).
- Token count 205 vs 38 noted (AI-15) (legacy EAWR-2167). Time_Limit and budgets: see C-05 and C-03.
- Unmapped or ambiguous: NEW-L21 (the Lua cross-check (legacy EAWR-2168) lists it open; the story read (legacy EAWR-2169) covers it);
  the story read's summary line (legacy EAWR-2169) counts 3 ours-wrong, while its table gives 6 wiki-right story conflicts
  (the table is used above); the story read (legacy EAWR-2169) gave the NEW-S ranges only as a block, so each settled S item
  cites WST-83..85 generally.

### Where the verdicts live

The full verdicts and rule IDs are in the behaviour notes on `main`. The skirmish (M2) verdicts
are there already. The galactic conquest (M3) verdicts land with the GC Focumentation verdicts
(legacy EAWR-2197); main's 2026-10-10 GC audits had reused some of their IDs, so they were
renumbered there and are cited here by the new IDs (WGA-426..432, WGM-197, WGP-181..183,
gc-presentation U-13, WGS-180, WST-83..85, WST-U14/U15). The black-market ability gate is
main's WGEP-145. Until that change merges, those GC IDs do not resolve on main.
