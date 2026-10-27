# `eawr_mods_service` protocol (`eawr-mods-protocol` 1.3)

This is the contract between the game repository, which owns `eawr_mods_service` (`apps/mods_service`), and the
launcher repository, whose Godot manager starts the service and talks to it. The design behind it is
`docs/design/mod-manager.md`, section 5 "Repository split". When this file and the design disagree about the wire
format, this file wins. Change it only together with the service and the recorded transcripts.

The service answers questions whose answer must equal the engine's: discovery, chain resolution, profile files,
load check, file and record indexes, identity codes. The launcher only displays the finished rows it returns.

## 1. Transport and framing

- The launcher starts one service process per manager session and talks to it over the process's stdin
  (requests, launcher to service: `c2s`) and stdout (responses and events, service to launcher: `s2c`).
- **stderr is a free-form UTF-8 log** for people. Nothing on stderr is part of the protocol, and the launcher may
  discard its contents after reading. It MUST continuously drain stderr or redirect it to a file or null sink
  while the service runs. stdout carries protocol lines only.
- Every message is one JSON object encoded as UTF-8 on one line, ended by `\n` (LF). The service accepts a `\r`
  before the `\n` and ignores it. It writes LF only. A line that is empty, or holds only spaces and tabs, is
  ignored without an answer.
- A UTF-8 byte order mark is not allowed anywhere in the stream; a line that starts with one is malformed.
- On Windows the service is a GUI-subsystem executable: it never opens a console window, and it reads and writes
  the pipes it inherits from its parent (Godot's `OS.execute_with_pipe`).

## 2. Messages

### Request (`c2s`)

```json
{"id": 1, "method": "hello", "params": {}}
```

- `id`: an integer, `0 <= id <= 9007199254740991` (2^53 - 1, so it is exact in both a JSON double and a Godot
  `int`). The launcher picks the ids; it should not reuse an id while that request is still open.
- `method`: a non-empty string naming a method (section 6).
- `params`: an object. It may be omitted, which means `{}`. Members a method does not know are ignored, so a
  newer launcher can send optional fields to an older service of the same major version.

Members of the request other than these three are ignored.

### Response (`s2c`)

```json
{"id": 1, "result": {}}
{"id": 1, "error": {"code": "EAWR-MODS-SVC-0004", "message": "unknown method \"nope\""}}
```

- Exactly one response per request that has a usable `id`, either `result` (an object) or `error`.
- `error.code` is a stable code from section 4. `error.message` is English text for a log or a tooltip; it is not
  stable and the launcher never parses it. `error.data` (an object) is present only where section 4 says so.
- A line the service cannot attribute to a request (not JSON, not an object, too long, no valid `id`) gets an
  error response with `"id": null`.

### Event (`s2c`)

```json
{"event": "loadcheck.progress", "id": 7, "data": {"stage": "catalog", "done": 120, "total": 687}}
```

- `id` is the id of the request the event belongs to. A request gets zero or more events, all of them before its
  response. After the response no event carries that id again.
- Events are informational. A launcher that does not know an event name ignores it.

### Ordering and concurrency

- The service reads requests in order. Ordinary methods are answered in the order they arrive.
- Methods marked **long** in section 6 (only the load check in v1) run in the background: while one is running,
  the service keeps reading and answering other requests, so their responses can arrive before the long
  request's response. The launcher matches responses by `id`, never by position.
- Responses and events are written whole: lines never interleave.

## 3. Versioning

- The protocol version is `MAJOR.MINOR`, now **1.3**.
  - A minor version only adds: new methods, new optional request members, new result members, new events, new
    error codes. A launcher written for 1.0 works against any 1.x service.
  - A major version may break anything. The service speaks exactly one major version.
- **The first request is always `hello`** (section 6). Its `params.protocol` says which major the launcher speaks
  and the lowest minor it needs:
  - service major equals `params.protocol.major` and service minor `>= params.protocol.min_minor`: the session
    is open, and `hello` returns the result;
  - otherwise `hello` returns error `EAWR-MODS-SVC-0006` with `error.data` holding the same members as a
    successful `hello` result. The launcher compares the versions and shows its message: when the service is
    older ("this launcher needs a newer EAWR") or newer ("this EAWR needs a newer launcher"). The session stays
    unopened; the launcher closes stdin.
- `capabilities` in the `hello` result lists the methods this service implements. A launcher checks it before it
  uses a method that a minor version added, and treats a method missing from the list as not available (the
  service would answer `EAWR-MODS-SVC-0004`). Planned methods in section 6 appear there only once they are built.
- Every method name listed in `capabilities` is answered with the shape this file documents for the reported
  minor version.

## 4. Errors

Codes are `EAWR-MODS-SVC-` plus four digits. Codes are never reused for a different meaning; a code that is no
longer produced stays reserved. Method-specific failures that come from the engine side (for example a
discovery diagnostic) keep their own code families (`EAWR-MODS-*`, `EAWR-VFS-*`) inside a result's rows; the
codes below are protocol failures of a whole request.

| Code | Name | `id` in the response | When |
|---|---|---|---|
| `EAWR-MODS-SVC-0001` | malformed line | `null` | the line is not valid UTF-8 JSON, or not a JSON object |
| `EAWR-MODS-SVC-0002` | invalid request | the request's `id` when it is valid, else `null` | `id` missing or not an integer in range; `method` missing, empty or not a string; `params` present but not an object |
| `EAWR-MODS-SVC-0003` | line too long | `null` | a request line over the line limit (section 5). The service discards the bytes up to the next `\n` and goes on |
| `EAWR-MODS-SVC-0004` | unknown method | the request's `id` | `method` is not in `capabilities` (planned methods included) |
| `EAWR-MODS-SVC-0005` | invalid params | the request's `id` | a member the method needs is missing or has the wrong type or value |
| `EAWR-MODS-SVC-0006` | protocol not supported | the request's `id` | `hello` asked for a major or minor this service does not speak; `error.data` = the `hello` result members |
| `EAWR-MODS-SVC-0007` | hello required | the request's `id` | a known method other than `hello` before a successful `hello` (an unknown method gets `0004` first) |
| `EAWR-MODS-SVC-0008` | hello repeated | the request's `id` | `hello` after the session is open |
| `EAWR-MODS-SVC-0009` | cancelled | the request's `id` | a long request ended by its cancel method (implemented in 1.3) |
| `EAWR-MODS-SVC-0010` | busy | the request's `id` | a long method started while another of the same kind runs (implemented in 1.3) |
| `EAWR-MODS-SVC-0011` | internal error | the request's `id` | the service failed in a way that is its own bug, including an answer it could not fit into the line limit |

`EAWR-MODS-SVC-0012` is reserved for the `game.find` **result diagnostic** "installation not found";
it is not a whole-request error. Domain codes in result rows retain their core meanings:

| Code | Meaning |
|---|---|
| `EAWR-MODS-0001` | Workshop folder not listed by Steam |
| `EAWR-MODS-0002` | Workshop manifest unreadable |
| `EAWR-MODS-0003` | Parent not installed / unknown key |
| `EAWR-MODS-0004` | Tree walk capped |
| `EAWR-MODS-0005` | Duplicate Workshop item |
| `EAWR-MODS-0006` | Folder missing |
| `EAWR-MODS-0007` | Mods-folder list cut |
| `EAWR-MODS-0008` | Tree walk incomplete |
| `EAWR-MODS-0009` | Needs update |
| `EAWR-MODS-0010` | Downloading |
| `EAWR-MODS-0011` | Malformed modinfo |
| `EAWR-MODS-0012` | Malformed known-chain schema |
| `EAWR-MODS-0013` | Chain cycle |
| `EAWR-MODS-0014` | Duplicate dependency or explicit player layer |
| `EAWR-MODS-0015` | Not playable |
| `EAWR-MODS-0016` | Unconfirmed launch-script hint |
| `EAWR-MODS-0017` | Data overwrite detected (inventory comparison deferred) |
| `EAWR-MODS-0018` | Scan incomplete |
| `EAWR-MODS-0019` | Player order reverses a parent or declared sibling constraint |
| `EAWR-MODS-0100` | Malformed profile JSON |
| `EAWR-MODS-0101` | Unsupported profile format/version |
| `EAWR-MODS-0102` | Invalid profile id |
| `EAWR-MODS-0103` | Invalid profile fields |
| `EAWR-MODS-0104` | Invalid profile layer |
| `EAWR-MODS-0105` | Invalid launch settings |
| `EAWR-MODS-0106` | Profile I/O failure |
| `EAWR-MODS-0107` | Missing profile layer key |
| `EAWR-MODS-0108` | Invalid profile arguments |

A response line that would exceed the line limit is never written: the service sends `EAWR-MODS-SVC-0011` for
that request instead and logs the size to stderr. Methods whose answers can grow (lists, file and record pages)
are paged with a `cursor` (section 5), so this only happens on a bug.

## 5. Limits

- **Line limit: 1 MiB (1,048,576 bytes) each way**, counted without the ending `\n` (and `\r`). The service
  rejects longer requests with `EAWR-MODS-SVC-0003` and never writes a longer line.
- **Nesting:** JSON nesting deeper than 64 levels is malformed (`EAWR-MODS-SVC-0001`).
- **Paging.** A paged method takes `params.cursor` (absent or `null` for the first page) and `params.limit`
  (optional; the method documents its default and maximum). Its result holds the rows and `next_cursor`: an opaque
  string to pass back for the next page, or `null` after the last page. A cursor is valid only within the service
  process that issued it and only until the data it pages changes; a stale cursor gets `EAWR-MODS-SVC-0005`.
- **Files.** The service reads only the game folder, mod roots and its user-data folder, and writes only under the
  user-data folder, except the test-only `--transcript` file (section 9). It never writes into a mod root or the game folder.
- **Network.** The service opens no network connection.

## 6. Methods

Status: **implemented** methods are listed in `capabilities` today. **Planned** methods are reserved names with a
provisional shape; the ticket named builds them and fixes the shape in this file when it does.

| Method | Status | Long |
|---|---|---|
| `hello` | implemented (MM-S1a) | |
| `shutdown` | implemented (MM-S1a) | |
| `game.find` | implemented (MM-S1b) | |
| `mods.discover` | implemented (MM-S1b) | |
| `chain.resolve` | implemented (MM-S1b) | |
| `chain.check` | implemented (MM-S1c, protocol 1.2) | |
| `profile.list`, `profile.load`, `profile.save`, `profile.delete` | implemented (MM-S1b) | |
| `loadcheck.start` | implemented (1.3) | long |
| `loadcheck.cancel`, `loadcheck.result` | implemented (1.3) | |
| `files.page` | planned (MM-7) | |
| `records.query`, `records.get` | planned (MM-10) | |
| `source.get` | planned (MM-10) | |
| `identity.codes` | planned (MM-14) | |

### `hello` (implemented)

The first request of every session.

```json
{"id": 1, "method": "hello", "params": {"client": "eawr-launcher", "client_version": "0.1.0",
  "protocol": {"major": 1, "min_minor": 0}}}
```

- `client` (string, required): the client's name, for the log.
- `client_version` (string, required): the client's version, for the log.
- `protocol` (object, required): `major` (integer >= 0) and `min_minor` (integer >= 0).

```json
{"id": 1, "result": {"protocol": {"major": 1, "minor": 2}, "service_version": "0.1.0",
  "engine_build": "0.1.0+<commit>+gcsync3", "game_release": "0.1.0",
  "capabilities": ["chain.check", "chain.resolve", "game.find", "hello", "mods.discover", "profile.delete", "profile.list", "profile.load", "profile.save", "shutdown"]}}
```

- `protocol`: the version this service speaks.
- `service_version`: the service's own version; in v1 the game's project version.
- `engine_build`: the engine build identity (the same string the multiplayer handshake shows; it changes with
  every build and is informational).
- `game_release`: the game release version this service ships with (the project version for a local build). The
  launcher's release manifest range (for example `game >=0.3.0 <0.4.0`) is checked against this.
- `capabilities`: the implemented method names, sorted.

Errors: `EAWR-MODS-SVC-0005` when a required member is missing or has the wrong type, `EAWR-MODS-SVC-0006` on a
version mismatch (section 3), `EAWR-MODS-SVC-0008` when the session is already open.

### `shutdown` (implemented)

Ends the session.

```json
{"id": 9, "method": "shutdown", "params": {}}
{"id": 9, "result": {}}
```

The service answers, stops reading stdin, and exits with code 0. Running long requests are cancelled first (their
responses, `EAWR-MODS-SVC-0009`, come before the `shutdown` response). Needs an open session, like every method
other than `hello`.

### `game.find` (implemented, MM-S1b)

Finds the Forces of Corruption installation. Params: optional `hint` (an absolute folder the player picked). Result: the
game root, the installation kind (Steam, other) and diagnostics rows; no root plus a stable code when none is
found.

### `mods.discover` (implemented, MM-S1b)

Discovers installed mods: Steam libraries (Workshop items for the game), the game's `Mods` folder, and manual
folders the player added. Result: mod records as in the design's "Mod record" (key, kind, name, alias, root,
role, state, fingerprint, warnings), paged with `cursor`.

### `chain.resolve` (implemented, MM-S1b)

Resolves the layer order for a leaf mod. Params: `leaf` (record key), `known_chains` (the launcher's reviewed
hints: Workshop ids and display names), `edges` (the player's own parent edges). Result: the chain, leaf first,
each entry with its provenance (`player`, `modinfo`, `known-chain`, `launch-script`), plus missing-parent and
update-state rows.

### `chain.check` (implemented, MM-S1c, protocol 1.2)

Checks an explicit player order, **first layer = top / highest priority**, as in profiles.
It preserves that order and neither inserts nor removes layers.

Params: `{layers:[{key}], accepted_warnings?:[code], known_chains?:object}`.
`layers` is required; an empty array means unmodded play. Keys and accepted codes must be nonempty strings.
`known_chains` uses the same schema as `chain.resolve`; the launcher supplies its reviewed hints on each
check (there is no retained hint state). Omitting it checks modinfo and script hints only.

Result: `{warnings:[{code,key,message}], diagnostics:[{code,key,message}], launchable}`.
Dependency selection, layouts, aliases, cycles, installation state and declared sibling order use the same
core graph builder as `chain.resolve`. Each explicitly selected layer is a graph root, so its own dependencies
are checked even if another layer declares it through a nonrecursive layout. Only the first selected layer
must be a playable leaf; lower parent-only layers are allowed.

- `EAWR-MODS-0019` warns when a parent is above its child or declared siblings are reversed.
- `EAWR-MODS-0014` warns for duplicate selected layers (including aliases). Repeated upstream dependencies
  are deduplicated without requiring acceptance.
- Unknown keys, required layers omitted from the selection (`0003`), cycles (`0013`), downloading (`0010`),
  incomplete top-level metadata scans (`0018`), missing Data / an unplayable leaf (`0015`) and unconfirmed script
  hints (`0016`) remain diagnostics that block launch. A needs-update layer remains usable. Fingerprint
  change-check limits (`0004`, `0008`) do not block either `chain.check` or the unchanged `chain.resolve` verdict.

`launchable` is true only when there are no blocking diagnostics and **every warning code** appears in
`accepted_warnings` (default `[]`). Acceptance never clears diagnostics, even if their codes are supplied.
Warnings remain in the result after acceptance. Malformed params or known-chain schema return `0005`,
with the same domain diagnostic data as `chain.resolve` for invalid known chains.

Launcher changes needed relative to the draft 1.2 adapter: send `known_chains` with each check so reviewed
Workshop dependencies participate; display `diagnostics` as well as `warnings` to explain blocked launches.
The draft's required request and result members retain their shapes. Protocol 1.1 clients continue to work.

### `profile.list`, `profile.load`, `profile.save`, `profile.delete` (implemented, MM-S1b)

The service owns the profile file format (`mods/profiles/<id>.json` under the user-data folder); the launcher never
writes a profile file. `profile.list` returns id, name and status per profile; `profile.load` takes `id` and
returns the profile object; `profile.save` takes a profile object, validates it and writes it, returning the saved
object (or diagnostics rows and an error); `profile.delete` takes `id`.

### Protocol 1.1 method details

`game.find {hint?: string|null}` returns `{root: string|null, kind: "steam"|"other"|"none",
diagnostics: [{code,message}]}`. A hint is validated directly (including the launcher's wrapper-folder rule);
without one, the service tries the launcher's saved game choice, then its Steam/GOG/uninstall discovery.
Validated installs under `steamapps/common` are classified as `steam`, including hints and saved choices.
A relative hint is invalid params (`0005`).
A missing game is a successful result with null root and diagnostic `EAWR-MODS-SVC-0012` (installation not found).
The chosen game's Mods folder becomes a discovery source. This does not modify the saved launcher choice.
No authoritative retail file inventory is available from the install discovery metadata, so data-overwrite
comparison remains deferred; an arbitrary installed directory listing is not a retail baseline.

`mods.discover` returns `{records,warnings,next_cursor}` using the catalog's record representation, including `has_data` and `metadata`. Optional
`steam_roots`, `steam_libraries` and `manual_folders` are arrays of absolute folder paths; they replace those
session discovery sources. `walk_trees` is boolean and defaults to true **on every request**, regardless of earlier requests. The default Steam roots come from the native
location adapter. No game is required to discover Workshop or manual folders. Every call refreshes discovery
and top-level mod metadata. No file bytes are changed.

`chain.resolve {leaf,known_chains?,edges?}` accepts the shipped known-chain **object**
`{format:"eawr-known-chains",version:1,chains:[{id,name,parents,role?}]}`. Player edges are an array of
`{child,parent}` record-key pairs, in parent precedence order. It returns `{layers:[{key,root}],
rows:[{key,name,why,state}],edges:[{child,parent,why}],diagnostics:[{code,key,message}],launchable}`.
Unknown keys, missing parents and cycles are finished resolver results with `launchable:false` and domain
diagnostics, not transport errors. Invalid known-chain JSON/schema is `0005` with domain diagnostics in
`error.data.diagnostics`. The launcher must check `launchable` before saving a chain for play.

`profile.save {profile}` returns `{profile}`. The create intent `{id,name,layers:[{key,root}]}` fills all defaults
through the core profile parser: `eawr-mod-profile` v1, chain order, no separators, front-end launch, safe render
thread, null frame-rate override and empty accepted state. A full profile uses the same parser and atomic writer.
`profile.list` returns `{profiles:[{id,name,status}],next_cursor}` sorted by filename.
`profile.load {id}` returns `{profile,rows:[{key,name,why,state}],status}` and uses core key re-resolution for missing
saved roots. Profile rows use the saved `order_source` (`chain` or `player`) as `why`; the profile format does not
retain each dependency edge's provenance. Status is `ok`, `missing` (a layer root is gone and its key has no
available catalog root), or (list only) `invalid` (an unreadable, malformed or mismatched profile). `profile.delete {id}` returns `{deleted:true}`;
a nonexistent profile is an error. Unsafe ids/folders and invalid profile content return `0005`; core profile
errors carry `error.data.diagnostics:[{code,message}]`. Only the profile file is deleted.

Both paged methods accept `limit` (default 100, maximum 500) and `cursor` (null/absent first page). Pages also
stop before the transport byte limit. A single row that cannot fit yields `EAWR-MODS-SVC-0011`;
the stream remains usable for later requests. Use the same discovery sources and `walk_trees` value for
subsequent pages (repeat `walk_trees:false` on every page of an unwalked listing). A changed result,
`game.find`, profile save/delete, another process, or eviction of one of the 64 retained cursors makes a cursor
stale (`0005`). A cursor is opaque and may be used only for the method that issued it.

The service uses the viewer's user-data directory by default. `--user-data <folder>` selects another user-data
root (also used for synthetic tests). Profile files live only under its `mods/profiles` directory. Linked
profile descendants are rejected. Mod/game folders are read-only.

These final shapes add `edges`, `diagnostics` and `launchable` to the launcher draft chain result; all other draft
result shapes and the create intent are retained. Domain failures are carried in results or `error.data` as above.

### `loadcheck.start` (1.3, long), `loadcheck.cancel`, `loadcheck.result`

`loadcheck.start` takes a profile id and runs one engine pass (mount, catalog, unit and type loaders, content
identity) in the background. It sends `loadcheck.progress` events (`stage`, `done`, `total`) and answers when the
pass ends, with the report summary, or with `EAWR-MODS-SVC-0009` after a cancel. One load check runs at a time
(`EAWR-MODS-SVC-0010` otherwise). `loadcheck.cancel` takes `request` (the `id` of the running `loadcheck.start`)
and answers `{"cancelled": true|false}` right away. `loadcheck.result` takes a profile id and returns the latest
cached report for it, with diagnostics rows paged by `cursor`.

Params are `{"profile":"<id>"}` for start and result; result additionally accepts `limit` (1..500,
default 100) and an opaque `cursor`. Start returns `{"report":{scope,stage,ok,error_count,warning_count,content_code,requirements,diagnostics}}`.
Each diagnostic group contains `code`, `severity` (`error`, `warning`, or `info`), `count` and at most three
`examples`; examples are limited to 2048 bytes. Severity comes from `core::Diagnostic::severity`, as emitted by
the VFS, catalog and content loaders. Rows without engine severity are errors only when they cause the stage
to fail, otherwise warnings (for example unresolved unit-table fields). If a code occurs at multiple severities,
its group uses the highest severity. `error_count` and `warning_count` sum the full group counts at those
severities. They and `ok` describe the entire run and stay unchanged across result pages; `ok` says whether
the engine pass completed, so a completed pass can still contain errors from skipped objects.
Requirements have `kind` (`foc` or `parent`), `key` and `satisfied`. The content code is ten RFC 4648 base32
characters for **space skirmish content**, identified by report `scope: "space-skirmish"`; land content coverage
is deferred. It uses the digest from `compute_match_identity` after excluding `map-path` and `map-digest`. Existing domains,
including units and any skipped types, are retained. The code predicts content compatibility for this engine
version; the multiplayer handshake remains authoritative.

Result returns `{"report":null,"next_cursor":null}` before a cached run or when its chain changes. Otherwise
it returns the same report with only the requested diagnostic groups and `next_cursor`. Cursors expire when
the chain or engine build changes. The cache under `<user-data>/mods/cache/loadcheck/` uses the engine build
and ordered layer keys, roots and current tree fingerprints, including both base data layers; capped or
incomplete walks refuse reuse. A tree change during the pass refuses to cache its report. Mod roots remain
read-only. Cancelling never installs a partial report.

The service stays a small static-CRT process. Its worker thread starts the adjacent GUI-subsystem
`eawr_loadcheck.exe` with `CREATE_NO_WINDOW`, contained in a kill-on-close Windows job. Cancel terminates the
job; shutdown and closed stdin cancel and join it. A crashed, missing or malformed helper returns
`EAWR-MODS-SVC-0011` with one grouped diagnostic in `error.data.diagnostics`. Helper arguments are profile
file, user-data folder, report path (inside user-data) and game root. Stdout carries progress objects and one
`{"report":...}` object; exit 0 means transport succeeded (loader failure is `ok:false`), 2 means invalid
arguments/profile, 3 means an internal/report-write failure. It calls the existing VFS manifest mount,
catalog, unit table and skirmish content APIs. It reads/hashes Lua source for the existing manifest domain;
it creates no Lua VM, executes no scripts and starts no battle. Manager-equipped release kits alone include
the helper beside the service.

There is no watchdog timeout: a helper that stops producing output remains active until it exits or the
client uses Cancel, shutdown or closes stdin. Only stdout's write end and the NUL stdin/stderr sink are inherited.

### `files.page` (planned, MM-7)

The file-conflict view. Params: profile id, filters (layer, winner or loser, type, path text, identical copies),
`cursor`, `limit`. Result: one row per logical path with its winner and every shadowed candidate in precedence
order, per-candidate digests, and `next_cursor`.

### `records.query`, `records.get` (planned, MM-10)

`records.query` searches objects by name, tag or filter (changed by any mod, changed by a layer, several mods
author the same tag), paged. `records.get` returns one object's finished per-tag rows: vanilla value, each layer's
authored value, effective value, how (`own`, `overridden`, `inherited`, `merged`, `added`, `DROPPED`), and the
source location of each cell.

### `source.get` (planned, MM-10)

Returns one definition's XML text, read through the VFS (archives included), and the line to mark.

### `identity.codes` (planned, MM-14)

Returns the profile's chain code and profile content code.

## 7. Process lifetime

- One service per manager session. The launcher starts it, sends `hello`, and keeps it running while the
  manager is open; the service holds caches in memory for that time.
- **The service exits when its stdin closes** (end of file), with code 0, after cancelling any long request. It
  also exits with code 0 after answering `shutdown`. A launcher that quits needs to do nothing else: closing its
  end of the pipe ends the service.
- When stdout can no longer be written (the launcher is gone) the service exits with code 1.
- Exit codes: 0 normal end, 1 the protocol pipe failed or the service failed internally, 2 bad command line.

## 8. Logging

The service logs to stderr only: one free-form UTF-8 line per entry, for example the client named by `hello`,
rejected lines (with their size, never their full text) and failures. Logs carry no protocol meaning, and the
launcher may show, store or drop their contents, but MUST continuously drain stderr or redirect it to a file
or null sink. Leaving an attached pipe unread can block synchronous log writes. The service caps total stderr
output at 2,048 bytes per session, including prefixes, line ends and one final `log suppressed` line; after that
line it stays silent. This bounds log volume but does not replace the launcher's draining requirement (pipe
capacities vary). The service writes no log file of its own.

## 9. Transcripts

A transcript is a recorded session used as a test fixture in both repositories. The game repository records them
under `tests/mods_service/transcripts/` from the real service; the launcher repository copies them unchanged for
its fake service. They are never edited by hand (hand-written drafts in the launcher repository are marked as
such, below).

- File: `<name>.jsonl`, UTF-8, one JSON object per line, LF line ends.
- Line 1 is a meta line: `{"dir": "meta", "msg": {"protocol": "1.3", "recorded_by": "eawr_mods_service"}}`. A
  hand-written draft has `{"dir": "meta", "msg": {"draft": true, "protocol": "1.0"}}` instead.
- Every other line is one exchanged message in the order it crossed the pipe:
  - `{"dir": "c2s", "msg": {...}}`: a request the service read;
  - `{"dir": "s2c", "msg": {...}}`: a response or event the service wrote.
- A `c2s` line that is not a JSON object cannot be stored as `msg`. It is stored as text instead:
  `{"dir": "c2s", "raw": "<the line, as a JSON string>"}` (bytes of a line that is not UTF-8 are written as
  `\u00XX` of the byte), and a line over the limit as
  `{"dir": "c2s", "oversize_bytes": <byte count>}`. (These two forms are a refinement of the first envelope: a
  malformed line is exactly what `malformed.jsonl` must show.)
- `hello` results (and the `error.data` of `EAWR-MODS-SVC-0006`) contain `service_version`, `engine_build` and
  `game_release`, which change with every build. The committed transcripts store them normalised, as
  `"service_version":"0.0.0"`, `"engine_build":"0.0.0+transcript"` and `"game_release":"0.0.0"`; every other byte
  is what the service wrote. A consumer that checks versions treats these values as placeholders.
- Recording is a test-only switch: `eawr_mods_service --transcript <file>` writes the transcript while it serves
  stdin and stdout as usual.

Recorded transcripts (MM-S1a): `hello.jsonl` (hello, then shutdown), `unknown_method.jsonl` (hello, a planned
method, an unknown name), `malformed.jsonl` (a non-JSON line, a JSON array, a request without `id`, a request
before `hello`, then `hello` to show the session goes on).
`tests/mods_service/mods_service_contract.py --update-transcripts` re-records them from a built service.

Protocol 1.1 adds `model_methods.jsonl`, recorded from a synthetic installation with two empty mod trees. The
contract recorder normalises its temporary root to `<fixture>`, path separators to `/`, and opaque generated
cursors to `<cursor>` after recording. No installed game, mod files, account paths or network are used.

Recorded transcripts (MM-S1c): `chain_check_clean.jsonl`, `chain_check_warning.jsonl`,
`chain_check_accepted.jsonl`, `chain_check_missing.jsonl` and `chain_check_unknown.jsonl` cover a clean
player order, an unaccepted reversal, an accepted reversal, an omitted parent and an unknown key. Their
synthetic fixture paths and separators are normalised in the same way as `model_methods.jsonl`.
