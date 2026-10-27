# Deliberate deviations from the original

Places where the remake intentionally differs from the original game. Each row names the evidence level of the
original behaviour; "unverified" means no capture or debug-build walk backs it.

| Id | Area | Original (evidence) | Remake | Reason | Issue |
| --- | --- | --- | --- | --- | --- |
| DEV-1 | Skirmish selection | Only own objects, plus allied community property in a multiplayer tactical game, can be selected (debug-build walk of the selection admission in the FoC debug build via GhidrAssist; no capture, no committed walk artifact). Whether skirmish against AI counts as that mode, and the panel contents, are unverified by capture. | A click on an allied structure inspects it: health card, no commands, build menu or abilities; viewer-local, no net message. Own and enemy objects unchanged. An allied click replaces your current selection with the inspect card; reselect your units afterwards. Allied build pads and community-property stations keep their existing shared control ((legacy EAWR-1571), WBP-08/09, WSU-16: selectable with build UI); DEV-1 covers the other allied structures. | Owner decision 2026-10-24: the mechanic never made sense. | (legacy EAWR-4919) |
