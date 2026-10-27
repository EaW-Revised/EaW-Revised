# Live land view

The viewer accepts `--eawr-live-session skirmish` on a land map with
`--eawr-populate`, explicit human lobby slots, and optionally
`--eawr-land-bindings <json>` (without it the authored adapter supplies the gates). Select a member to select its squad; right-click
terrain to move it and press the bound stop key to stop. `--eawr-live-player`
chooses the local human seat. This first view runs a standalone land session;
network admission, land fog, the land HUD and combat presentation are separate
work.

Interactive land battles create the land camera automatically; no camera XML
argument is needed. The project camera policy centres the existing land XML
pose on the local player's prepared starting units and uses the terrain extent
as its bounds. The root viewport follows the window, including content scaling
and later resizes, and picking uses the same camera dimensions. Fixed capture
runs retain their pinned viewport and camera identity.

The start uses the authored overload of `build_land_start`. Company members,
formation spacing, footprints, movement-class memberships and effective
locomotion come from the mounted land tables through the
[land content adapter](behaviour/land-content-adapter.md). The JSON file supplies
the adapter's remaining external inputs and has no physical-profile overrides.
There are no stock-unit or permissive-mask runtime defaults.

The input format is `eawr-land-inputs-v1`:

| Field | Value |
|---|---|
| `format` | `"eawr-land-inputs-v1"` |
| `unverified_passability_bits` | Object mapping each authored passability name to a distinct unsigned single bit; must include `Clear` and every membership named by the movement-class table. R2/R3 remain unverified. |
| `collision_class_order` | Object mapping authored collision-class names to independent unsigned ordering values; these are not terrain bits. LR3-10's external gate remains explicit. |
| `contacts` | Array of complexity quota, reset interval, prediction seconds and minimum prediction distance. These are explicit service inputs, not measured retail defaults. |
| `capacities` | Array of `[player, last_known_capacity]` pairs, with one distinct entry per occupied seat. The menu adapter uses the unverified constant 50 (`unverified_last_known_capacity`), not a measured retail value; its contacts use the named `unverified_contact_*` constants. |
| `occupations` | Array of objects with `object`, `primary_passability`, `secondary_passability`, `primary_cells` and `secondary_cells`. An explicit empty array supplies no static occupations. |

Keep binding files under ignored `out/`. The adapter incorporates these inputs
and the authored profiles into land content identity. The GPU fixture generates
explicit **unverified test assignments**, proving the wiring without claiming
retail enum qualification. Walk-transition objects retain the adapter's
animation-completion gate; this view does not fabricate clip completion.

Land model slots share the existing model upload, skin pose, fade and death-clone
operations. They use land asset tags and the L1 terrain height field. Picking
intersects that field, including shallow rays across multiple cells. Member
ownership and transforms come from immutable tactical publications. The live
adapter captures the actual startup member-to-squad roster before launching the
simulation thread, so partial creation and split squads retain their real IDs.
Presentation changes no canonical state or headless movement rules.

The existing land camera configuration remains available through
`--eawr-map-camera-config`. A fixed view can use `--eawr-map-target`,
`--eawr-map-zoom` and `--eawr-map-yaw`. Reports include `land_live.pose_trace`
with member/squad IDs, ownership, positions, yaw, drawn-model status and the L1
ground height. Trace rows are bounded and recorded only when a report is
requested. The usual live capture ticks and headless hash comparison also work.

## Starting a land battle from the menus

Build option `EAWR_ENABLE_LAND_SKIRMISH` (default OFF in every preset, including `base`, the Windows MSVC/clang presets that CI and the pool use, and
`ninja-release`; the one dev preset `dev-land` turns it ON, or pass `-DEAWR_ENABLE_LAND_SKIRMISH=ON`; the pool viewer build
always passes the value explicitly, ON only when `tools/offload/viewer-land-skirmish.flag` exists) turns on land in the skirmish setup and the LAN lobby. With it OFF
both screens list exactly what they listed before.

- **Skirmish setup:** the Land game type is selectable; it swaps the map list to the land maps
  (`land_setup_maps`, shown beside the space maps by `setup_domain_maps`). Start carries
  `FixtureOptions::domain = land`, and the viewer starts the same live land session as the command line. Land has no AI
  yet, so every occupied row starts as an unattended human seat (the opponent rows keep their faction and team). The
  session builds its land content from the authored adapter (`authored_land_content_inputs`: explicit unverified
  passability bits and collision order derived from the loaded tables); `--eawr-land-bindings <json>` stays an
  override. Choosing a land map does not overwrite the remembered space choices.
- **LAN lobby:** the Land host game type is selectable and lists land maps; the setup carries the land domain. A
  land LAN start stops with "Land battles over the network are not supported yet."
  (the typed `land_unsupported` refusal of the setup codec covers a land offer that reaches a peer).
