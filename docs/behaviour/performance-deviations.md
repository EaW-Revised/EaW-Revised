# Performance deviations from retail

Owner approved this policy on 2026-10-25: "This could be another option, retail deviation toggle for performance."
The registry is `include/eawr/presentation/ui/performance_deviations.hpp`.
Every registered deviation must also have its own behaviour rule, owner approval and measured evidence.
No deviations are registered yet; the deployment-grid proposal awaits its measurements.

## Policy

- A presentation optimization with no visible difference is always on and listed for transparency, without a toggle.
- A visible presentation deviation uses a per-player **Retail-exact / Faster** preference under **Options > Performance**, initially **Retail-exact**.
- A simulation deviation requires a host-selected match option shared by all peers, included in match identity and recorded in replays. Clients see the host's choice read-only; skirmish setup uses the same option. It initially selects **Retail-exact**. A later default change needs owner approval and before/after balance measurements.

This registry introduces no simulation option. Its first simulation entry must bring the match/lobby/replay implementation with it.

## Registry

The settings unit test checks this table exactly against the compiled metadata, including rule ID, setting key and default.
Update the header and table together when adding an approved deviation.

<!-- performance-deviations:begin -->
| Rule | Setting key | Name | Scope | Default | Retail behaviour | Our behaviour |
|---|---|---|---|---|---|---|
<!-- performance-deviations:end -->

## Performance options

The main and in-game Options hubs expose **Performance**. Its **Render thread (restart)** selector offers **Separate (default)** and **Safe**.
Apply and Accept save only that preference; Cancel discards it; Defaults selects Separate without saving until Apply or Accept.
The engine mode changes after restarting. Explicit launch flags and manager-selected profile modes retain their startup precedence.
Frame-rate limit and vsync stay under **Video**. See [player settings](../ui/ui-settings.md).
