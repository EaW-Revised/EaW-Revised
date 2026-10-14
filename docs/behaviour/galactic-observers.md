# Campaign observer installation

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| GOBS-01 | A missing campaign `Planet_Auto_Reveal` starts enabled; an authored false disables it. This is independent of whether a planet is already discovered. | debug build: campaign-definition construction initializes the switch true before parsing; WGS-04 installs it in the campaign mode |

Campaign startup uses the explicit local player identity supplied by its caller.
The authored first matching faction `Home_Location` is resolved by the campaign
planner; a missing or nonplanet home does not create a substitute. Startup reveals
only that local home and explicitly refreshes its intelligence. It does not run
ordinary orbital-delay or topology discovery at tick zero. These are the distinct
[WGS-06 startup](walks/gc-session-save.md) and
[WGUI-01..05 ordinary discovery](walks/gc-ui-sensors.md) paths.

Home-only describes that explicit installation boundary, not a fully placed
campaign's first rendered frame. WGUI-01/04/107 permit an already discovered
allied home to expand to nearby planets and trade endpoints on the first ordinary
discovery service, including while paused. Foreign endpoints do not propagate
merely because they became discovered. A neutral layout has no allied home seed;
its home-only presentation retains the named pending starting-ownership provider,
owned by campaign starting ownership and forces (legacy EAWR-1149).
The bounded retail continuation WGUI-108 agrees with adjacent discovery after
ordinary service; its first still is Day 1, not tick zero, and does not settle
lane versus distance expansion or exact first-frame scheduling.

The campaign observer provider supplies complete live player, planet and type
facts after the existing placement, political-control and production providers.
Startup constructs the fresh Cartesian record table using WGUI-99: undiscovered,
zero reveal counter, zero mask, zero apparent control and zero apparent owner.
The local home receives the WGUI-99 ordinary explicit clear before recomputation,
including the distinct apparent-owner sentinel. WGUI-100 still owns the apparent
queries. Existing observer replay and checkpoint payloads preserve the resulting
start state; WGUI-102 player-load refresh remains a separate restoration path.

The validated metadata adapters apply `Minimum_Visible_Base_Level`,
`Benefits_From_Corruption` and independent `Planet_Starbase_Stealth` /
`Planet_Groundbase_Stealth` values to explicit producer inputs (WGUI-10/11/98/103).
Faction identity follows the canonical name lookup in WGUI-98. Absent metadata
keeps the producer's sourced value. It is not assigned a guessed stock default.
`Is_Stealth_Company` is retained as inherited type metadata, but does not itself
establish a live object's stealth, stealth-hero or superweapon predicate.

Remaining producer boundaries are named `campaign_observer_facts` (a caller with
no complete live fact provider), `observer_runtime_type_traits` (action-owned
stealth and superweapon facts), and `observer_starting_sources` (initial automatic
ability callbacks and their ordered source installation). No initial modifier
list or AI discovery is inferred. The viewer plan-only harness uses its own
explicit preview producer; it does not run the campaign placement and control
providers. Runtime/render qualification remains the walks' U-02 and the
frame-service programme's existing capture boundary.

The viewer's plan-only neutral-layout producer now installs a full observer table
with the selected human seat's local identity and authored home discovery. It
reports actual freshly created neutral ownership, empty bases and empty forces,
and refuses layouts that already contain live placement/control subsystems.
`observer_layout_nonlocal` retains the uninstalled other-client/AI initialization;
`observer_layout_empty_bases` supplies no threshold-based information when a
faction has no authored threshold. Starting ownership, forces and influence
remain pending providers. Presentation reads these immutable snapshot facts.

Session snapshots share an immutable derived observation vector. Observer-record
commits and fleet presence/membership commits advance its input generation; the
next publication rebuilds the projection once. Ordinary transit-position updates
and unrelated session publications reuse it. This cache is not serialized or
hashed; reconstruction and player-load refresh derive it from authoritative
inputs. `GalacticTick::Work::observer_projection_records` reports records rebuilt
during that step, including any intermediate conflict publications, and is zero
for clean publications. Old snapshots retain their own projection after changes.

A committed target-owner change also recomputes each observer's retained mask
for that planet (WGUI-06/07/08). Rebuilding the derived observation vector alone
cannot grant the new owner's production, structure and ground-content knowledge
when the planet was discovered before conquest. The refresh replaces the default
ownership intelligence while preserving independent, unexpired source modifiers;
the losing owner loses only the default grant. Quiet political interpolation
with an unchanged target owner does not repeat this refresh.

Capture keeps the existing military handover: hostile bases and structures are
cleared, authored slot capacity remains, and surviving invading transports keep
their identities and owner when moving from orbital fleet to surface. Political
control then commits ownership; production retains its normal control threshold
before accepting builds (WGM-57/58/60/66/72/74). These rules were rechecked in the
debug build. Campaign contracts exercise a directly staged tactical land child
and an automatic outcome at 1/2/4/8 workers, including fresh checkpoint restores;
the live M3 land-selection menu still routes ordinary land battles to auto-resolve.
The synthetic capture GPU case drives a real landing order and reads the resulting
build strip and troop roster without substituting post-capture HUD facts.

WGUI-13 display income uses validated `Planet_Credit_Value` (or the destroyed
value under WGEP-05) when current actual-income knowledge is absent. The named
`observer_actual_income` seam requires a complete producer for WGEP-05..09's
retained base/black-market, alignment, political, trade, structure and siphon
contributions; mining `IncomeState` alone cannot supply that getter. Known actual
income is displayed only when explicitly supplied, otherwise it stays unknown.
Apparent owner and allegiance-owner publications remain separate from current
ownership knowledge and overview name tint (WGUI-96/100).
The campaign-root installation API currently has no production caller:
`CampaignRoot::create` and `CampaignSetupServices::observer` are exercised by
contracts, while `campaign_observer_facts` remains a live-provider seam. The
viewer starts from the planner's neutral layout, so this API alone does not
install observer state in a running viewer. The separate viewer-layout producer
now wires that live start before `LiveGameSession::start`, installing the
neutral layout's complete observer table. Full starting ownership, forces and
automatic influence sources still need their separate live providers.
