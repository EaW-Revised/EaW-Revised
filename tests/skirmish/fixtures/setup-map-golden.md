# Eager map setup baseline

These SHA-256 rows were captured using production sources from commit
`0c441279c3f44239ed0609097b3871044e47ef44`, before metadata filtering, lazy launch
preparation and map-list reuse. They contain hashes and logical map identifiers,
without installed asset bytes, XML text, source locations or machine paths.

Each row has a logical map key and three SHA-256 values: complete picker record,
complete Fixture values, and complete StartInputs values. The `@maps` row repeats
the complete ordered-list digest in all three columns. Input hashes include the
contents of the supplied empty unit tables instead of a process pointer. Failure
rows hash diagnostic codes and messages. All 24 installed maps are covered; the
synthetic corpora cover an authored two-team map and an unavailable empty catalog.

The stable encoding is in `../setup_map_golden.hpp`. Synthetic TED and XML content
is authored in that header and is generated identically by the capture and tests.

To regenerate deliberately:

1. Extract `src/skirmish/setup.cpp`, `src/skirmish/inputs.cpp` and
   `src/assets/map_decode.cpp` from the BASE commit above into a scratch directory
   using `git show`. Use distinct temporary filenames for their object outputs.
2. Recover the historical capture tool into the scratch directory with
   `git show 5679ea3e312e2ba8a96d145e13dbd0c0e86e1a23:tests/skirmish/capture_setup_map_golden.cpp`.
   Compile those three unmodified BASE translation units together with that tool,
   using the repository include directories and
   BASE libraries with the same compiler and runtime. Do not link the optimized
   production helpers from a newer revision. The capture does not call the test
   eager reference algorithm.
3. Run `capture_setup_map_golden <installed-game-root> <scratch-synthetic-dir>
   <scratch-output-dir>`. It reads the installation and writes three hash files;
   only the authored synthetic corpus is written to the synthetic directory.
4. Review the hash rows before replacing these files. The ordinary tests never
   regenerate or accept new expected values.

The optimized path must match the ordered list and each chosen map's fixture and
full start inputs. The synthetic tests also exercise 1/2/4/8 workers, forced thread
launch failure, ordered worker exceptions and the caller-thread map profile.

## Deliberate updates after the capture

The third column (complete StartInputs values) includes the script manifest digest, which binds the Lua sandbox
policy identity. When that identity changes, the input hashes of every row that reads inputs move with it, while the
map and fixture columns stay as captured.

- 2026-11-14, sandbox policy v3 to v4 (legacy EAWR-6803): the third column of the populated synthetic team row and of all 24
  installed rows was replaced by the current output. The map and fixture columns of all 25 rows are unchanged. For
  the synthetic row the cause was checked: the same inputs with the manifest digest of the v3 identity hash to the
  value captured before this update. The installed rows were taken from the test's own mismatch output (their
  manifests also hold the installed scripts, so the v3 value cannot be recomputed without the old build).
- 2026-11-17, unit-table identity v7 to v8 (legacy EAWR-7453): only the complete StartInputs hash column moves for the populated
  synthetic team and all 24 installed maps. The recovered capture front-end ran against current sources; every list,
  map and Fixture column stayed identical. Substituting the prior empty-table identity reproduced the committed files
  byte for byte, while the ordinary capture used v8. Independent Windows output matched all 24 Linux installed rows.
