## What and why

<!-- What this changes and why. Name the behaviour-note rule IDs it implements, if any. -->

## Testing

<!-- The commands you ran and their results. Say whether you ran the game-data tests
     (EAWR_EAW_GAME_ROOT set) and, for viewer changes, which GPU. -->

## Checklist

- [ ] Every commit is signed off (`git commit -s`, see CONTRIBUTING.md).
- [ ] No game files, screenshots of the original game, decompiler output, binary addresses,
      original engine names or original shader source are added (CONTRIBUTING.md, clean-room rules).
- [ ] New behaviour has tests; tests that need the game skip without `EAWR_EAW_GAME_ROOT`.
- [ ] Simulation changes give the same results for 1, 2, 4 and 8 workers; moved replay pins
      are regenerated with the pin tools and explained above.
