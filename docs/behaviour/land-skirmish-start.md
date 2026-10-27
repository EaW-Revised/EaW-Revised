# Land skirmish start boundary

The P1 land start builder reads mounted map markers, faction default companies and the multiplayer company-cap curve. Physical movement masks, geometry, locomotion and contact settings enter through an explicit content adapter. Unbound scene objects remain in the ownership census until the land host supports inert roots. Economic rules are emitted for the later live land economy adapter; the current host runs movement only.

`MP_Default_Pre_Built_Base` is a copied, domain-neutral match option. The land builder explicitly takes the authored option (or an explicit match override), rather than the space fixture preset. Structure-marker replacements remain census-only unless the content adapter supplies their company bindings. Maximum tech stays authored; a value below the initial tech level of one (WLEN-54) is refused.

An absent `Land_Skirmish_AI_Default_Forces` tag is recorded as a start note naming the faction and creates no default companies. An authored empty list also creates none, but is distinguishable from absence. No walk rule currently establishes a required-tag failure; this is an explicit P1 input contract.

Spawn selection uses reverse TED record order, the shared marker-search rule SK-11 in docs/skirmish-start.md and `markers_named` in the existing space builder. WLEN-55 supplies the Team_NN_ land marker family. Ownership remapping reuses WLEN-53 without a separate land implementation.

LC-46 evaluates the authored faction curve at controllable-player count, then applies last-known capacity. The exact integer-conversion rounding is **unverified**. P1 exposes truncation toward zero at this conversion seam, rather than claiming debug-build evidence for a rounding rule. LC-47's non-multiplayer defender branch is outside this skirmish builder.

The existing SKSU recording metadata admits only space victory selections. P1 keeps land match/seat metadata in the start result and records its match identity in the tactical setup; it does not fabricate a space victory selection. Land victory and live economy remain separate work.
