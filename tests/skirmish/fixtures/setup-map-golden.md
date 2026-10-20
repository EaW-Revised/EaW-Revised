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
