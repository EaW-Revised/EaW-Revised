# Debug console transcript

The engine-free console runner accepts `sim_headless console --script <file> --out <file> [--ticks <n>]`.
It reads one command per line and writes one UTF-8 JSON object per nonempty command line, in input order.
The input and output paths must identify different files.

Each object contains:

| Field | Meaning |
| --- | --- |
| `input` | Original command line. |
| `output_keys` | Localisation keys for the result. |
| `output` | Text produced by the core runner. |
| `envelope` | Hex-encoded command envelope, empty for core commands. |
| `tick` | Applied simulation tick, or null when there is no active session. |
| `state_hash` | Resulting state hash, or null when there is no active session. |
| `requested_ticks` | The requested tick count, defaulting to zero. |

The core runner has no active game and does not advance a simulation. Its `tick` and `state_hash` fields are
therefore null; `requested_ticks` preserves the requested count for later session-backed runners. Battle and
galactic integrations will provide a session, advance it, and report its actual tick and hash.
