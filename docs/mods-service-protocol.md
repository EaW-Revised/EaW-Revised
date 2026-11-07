# `eawr_mods_service` protocol (`eawr-mods-protocol` 1.12)

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
- Methods marked **long** in section 6 (load check and records in 1.9) run in the background: while one is running,
  the service keeps reading and answering other requests, so their responses can arrive before the long
  request's response. The launcher matches responses by `id`, never by position.
- Responses and events are written whole: lines never interleave.

## 3. Versioning

- The protocol version is `MAJOR.MINOR`, now **1.14**.
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
| `EAWR-MODS-SVC-0014` | invalid load notes | the request's `id` | the load notes file is malformed, unsupported, or oversized |
| `EAWR-MODS-SVC-0015` | invalid settings | the request's `id` | the settings file is invalid or unsupported and was not changed |
| `EAWR-MODS-SVC-0016` | profile locked | the request's `id` | (1.12) `profile.save` or `profile.delete` named a profile that a running game instance holds, or a game instance's state file exists but cannot be read (fail closed); `error.data` = `{"profile": id, "instances": [n, ...]}` plus `"unreadable": [n, ...]` when a state could not be read. Nothing was written or deleted |

`EAWR-MODS-SVC-0012` is reserved for the `game.find` **result diagnostic** "installation not found";
it is not a whole-request error. `EAWR-MODS-SVC-0013` is a load-check **warning diagnostic** for an
unavailable optional file index (build, write, validation or installation failure); it does not fail the check.
Domain codes in result rows retain their core meanings:

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
| `EAWR-MODS-0020` | No content digest for a mod folder (the `note` of its `content` says why) |
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
| `mods.changes` | implemented (1.5) | Workshop metadata polling |
| `mods.discover` | implemented (MM-S1b) | |
| `chain.resolve` | implemented (MM-S1b) | |
| `settings.set_builtin_layer` | implemented (protocol 1.8) | |
| `chain.check` | implemented (MM-S1c, protocol 1.2) | |
| `profile.list`, `profile.load`, `profile.save`, `profile.delete` | implemented (MM-S1b) | |
| `loadcheck.start` | implemented (1.3) | long |
| `loadcheck.cancel`, `loadcheck.result` | implemented (1.3) | |
| `files.page` | implemented (1.4) | cached file index |
| `records.index`, `records.get` | implemented (1.9) | long; cached record index |
| `records.cancel` | implemented (1.9) | |
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
{"id": 1, "result": {"protocol": {"major": 1, "minor": 11}, "service_version": "0.1.0",
  "engine_build": "0.1.0+<commit>+gcsync3", "game_release": "0.1.0", "kit_build": "0.1.0 (<7-hex commit>)",
  "capabilities": ["chain.check", "chain.resolve", "files.page", "game.find", "hello", "loadcheck.cancel", "loadcheck.result", "loadcheck.start", "loadnotes.read", "mods.changes", "mods.discover", "profile.delete", "profile.list", "profile.load", "profile.save", "settings.set_builtin_layer", "shutdown"]}}
```

- `protocol`: the version this service speaks.
- `service_version`: the service's own version; in v1 the game's project version.
- `engine_build`: the engine build identity (the same string the multiplayer handshake shows; it changes with
  every build and is informational).
- `game_release`: the game release version this service ships with (the project version for a local build). The
  launcher's release manifest range (for example `game >=0.3.0 <0.4.0`) is checked against this.
- `kit_build` (1.10): the player-facing form of `engine_build`, `<label> (<short commit>)` plus ` dirty` for a modified
  tree. A release build sets the label to the kit (for example `0.3.2g`) through the `EAWR_KIT_LABEL` configure
  override; a development build's label is the project version. Display only. `game_release` stays a plain version
  for the manifest range check. Older services omit it: show `engine_build`.
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

### `mods.changes` (1.5)

Params: `{previous?: string}`. `previous`, when present, is any string; an unknown, stale or malformed revision is a normal change, not an error.
A non-string value is invalid params. Result: `{revision: string, changed: boolean, complete: boolean}`.
An omitted `previous` establishes a baseline (`changed:false`). A complete call compares its opaque revision
with `previous`; the method is stateless and does not refresh or modify the discovered catalog.

The revision covers the resolved Steam libraries, the library map metadata, each library's
`steamapps/workshop/appworkshop_32470.acf` size/mtime, and the immediate numeric directory listing and directory
mtimes under `steamapps/workshop/content/32470`. It never walks item trees, opens asset bytes, downloads, or starts
watcher threads. Resolving libraries reads Steam's small `libraryfolders.vdf` files through the existing adapter.
Missing Workshop files/content are normal. An offline/inaccessible library, malformed library map, wrong file
type, unstable ACF/listing during the check, or oversized listing yields `complete:false, changed:false, revision:""`.
This is not a protocol error: clients retain their previous revision and rows and try again later.

Clients poll at intervals of at least three seconds, only while the manager window is visible or focused.
One `changed:true` schedules one discovery refresh (including its continuation pages); polls do not themselves
refresh discovery. Preserve the selected profile and unsaved order. Services older than minor 5 keep manual
Refresh with no unsupported request or error. Size/mtime polling cannot detect a deliberately rewritten file
whose size and timestamp are both preserved.

Workshop records distinguish `needs-update` (Steam lists an update, still usable) from `downloading` (Steam is
fetching a newer item or has not installed it yet, never usable for Play). Both ordinary `WorkshopItemDetails`
manifest/time and optional `latest_*` fields can identify newer items when the installed entry has a manifest and a positive update time. `NeedsDownload` changes a proven newer
item to downloading; `NeedsUpdate` is parsed as an app-wide hint, not proof that every item needs an update.
An unchanged unrelated item remains installed even when those app-wide hints are set.

### `mods.discover` (implemented, MM-S1b)

Discovers installed mods: Steam libraries (Workshop items for the game), the game's `Mods` folder, and manual
folders the player added. Result: mod records as in the design's "Mod record" (key, kind, name, alias, root,
role, state, fingerprint, warnings), paged with `cursor`.

### `chain.resolve` (implemented, MM-S1b)

Resolves the layer order for a leaf mod. Params: `leaf` (record key), `known_chains` (the launcher's reviewed
hints: Workshop ids and display names), `edges` (the player's own parent edges). Result: the chain, leaf first,
each entry with its provenance (`player`, `modinfo`, `known-chain`, `launch-script`), plus missing-parent and
update-state rows.

Protocol 1.8 appends a derived row to `rows` after all user layers, in highest-priority-first order:
`{key:"builtin:revised",kind:"builtin",name:"Revised",enabled:boolean,root:string|null,
why:"builtin",state:"installed"|"missing",note:string|null}`. The root is the player folder's
`Revised`, or the `revised_layer_path` development override when set. The root contains `Data`; a missing `Data` folder produces a
row with null root and a note, without changing launchability. A relative override is ignored with a note. `layers` remains the user layer list
used for profiles. `profile.load` appends the same derived row, including for old or empty profiles.

### `settings.set_builtin_layer` (protocol 1.8)

`{key:"builtin:revised",enabled:boolean}` writes the shared `eawr-settings.cfg` and returns
`{key:"builtin:revised",enabled:boolean}`. The preference defaults to true when absent. Invalid keys
or non-boolean values return `0005`; a write failure is an internal error. Invalid or unsupported settings return `0015` without writing. Unknown settings are preserved. The engine-side reader is part of the SK engine-mount follow-up (legacy EAWR-5443, EAWR-5453), not this service change. Once that reader ships, it reads this preference at start, so changes apply on the next start; the launcher disables the switch while a game runs. The row is never stored in a profile: `profile.save` rejects `builtin:` keys, and `profile.load` removes them from old files with a note.

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

### `profile.locks` (1.12), and the lock on `profile.save` / `profile.delete`

Several game instances can run at once (`EAWR.exe --instance n`, apps/launcher/README.md). While any live instance
runs a saved profile, that profile is read-only: `profile.save` (any change, including a rename or new layers) and
`profile.delete` for its id answer `EAWR-MODS-SVC-0016` and change nothing. Playing it again in another instance
is allowed and is not the service's business. The lock is derived by the service itself from the launcher's state
files: `launcher/game-running.json` (instance 1) and `instances/<n>/launcher/game-running.json` under the user-data
folder, each counted only while the process it names is alive (PID plus creation time, so a reused PID does not
hold a lock; a state written without `process_start_time` by an older launcher is judged by its PID alone). A
state without a profile id (a vanilla instance) and a missing state file hold nothing. A state file that exists
but cannot be opened, is larger than 64 KiB or is not the expected JSON is **not** free: which profile it names is
unknown, so every `profile.save` and `profile.delete` fails closed with `EAWR-MODS-SVC-0016` (message: the state
of instance n could not be read; `error.data.unreadable` lists the instances) until that file is fixed or removed.
Two managers see the same locks. It does not read pending `next-action.json` requests; a manager keeps its own
pending lock between submitting a request and the game's state file appearing.

The check and the write are not one atomic step: a game that starts between the lock check and the file write of
a `profile.save` takes the profile as it was written. This window (milliseconds) is accepted; the launcher reads
the profile again when it starts the next game, and the manager locks the profile before it submits a request.

`profile.locks` takes no params and returns `{"locked": [{"profile": "<id>", "instances": [1, 3]}, ...],
"unreadable": [n, ...]}`: `locked` sorted by profile id, instances ascending, an empty array when nothing is running;
`unreadable` the instances whose state file could not be read (normally empty). A client written for an earlier 1.x
service finds the method missing from `capabilities` and treats nothing as locked by the service.

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
through the core profile parser: `eawr-mod-profile` v1, chain order, no separators, front-end launch, separate render
thread, null frame-rate override and empty accepted state. A full profile uses the same parser and atomic writer.
`profile.list` returns `{profiles:[{id,name,status}],next_cursor}` sorted by filename.
`profile.load {id}` returns `{profile,rows:[{key,name,why,state}],status}` and uses core key re-resolution for missing
saved roots. Profile rows use the saved `order_source` (`chain` or `player`) as `why`; the profile format does not
retain each dependency edge's provenance. Status is `ok`, `missing` (a layer root is gone and its key has no
available catalog root), or (list only) `invalid` (an unreadable, malformed or mismatched profile). `profile.delete {id}` returns `{deleted:true}`;
a nonexistent profile is an error. Unsafe ids/folders and invalid profile content return `0005`; core profile
errors carry `error.data.diagnostics:[{code,message}]`. The profile file and its small per-profile load-check marker are deleted; a running check for that profile is cancelled so it cannot recreate the marker. Shared fingerprint-keyed report/index caches are retained.

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
Protocol 1.6 adds `elapsed_ms` to each completed report: an integer, nonnegative wall-clock duration in
milliseconds from helper process creation through observed exit, measured by the service with a monotonic
clock. It includes helper setup and waits; cached reports retain the original duration. The helper itself
cannot override this value. Earlier reports/clients may omit/ignore it. The service reserves 256 bytes
of the 1 MiB line limit for the result envelope (id, cursor, status and fingerprint); a larger completed
report is rejected before it or its per-profile marker is installed, so an accepted report remains retrievable.

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

Result retains `report` and `next_cursor` and, in 1.6, adds `chain_fingerprint` (the current key) and
`status`, an extensible enum: `current` means the completed report matches this profile's current chain;
`out_of_date` means a completed run is recorded but its fingerprint changed or its report is missing;
`not_run` means no valid completed-run marker exists for this profile. Clients must tolerate unknown status
strings by falling back to the report's presence and their older state logic. Stale/unavailable results
always return `report:null,next_cursor:null`; stale diagnostics never authorize Play. Current results return
the original report with only the requested diagnostic groups and `next_cursor`. Cursors expire when
the chain or engine build changes. The cache under `<user-data>/mods/cache/loadcheck/` uses the engine build
and ordered layer keys, roots and current tree fingerprints, including both base data layers; capped or
incomplete walks refuse reuse. A tree change during the pass refuses to cache its report. Mod roots remain
read-only. Cancelling never installs a partial report.

A versioned `last-<sha256(profile-id)>.json` marker records the completed fingerprint and elapsed duration
under this service user-data cache directory. There is at most one marker per profile, read with a hard
256-byte bound, replaced atomically only after a completed valid report is installed, and removed with the
profile. Shared keyed report bodies retain this profile marker's completed duration when returned. No marker or a corrupt/unsupported/redirected marker means `not_run` without an error. This state
survives a service/launcher restart. Existing keyed caches without a marker do not claim that the profile
was checked. A rerun and deletion serialize their marker commit/removal.

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

### `files.page` (1.4)

Pages the file index produced by the most recent load-check pass for this exact chain fingerprint and
engine build. It never mounts assets or starts a load check. As for `loadcheck.result`, current root metadata
fingerprints validate the cache; asset bytes are not read by this method. Clients written for earlier 1.x
versions work unchanged and need not call this capability.

```json
{"id": 20, "method": "files.page", "params": {
  "profile": "example", "limit": 100, "cursor": null,
  "filters": {"layer": "", "role": "either", "type": "", "path": "",
              "conflicts_only": false, "hide_identical": false}}}
```

- `profile`: required saved profile id. Its ordered roots are re-resolved through the existing profile API.
- `limit`: optional integer 1..500, default 100. The 1 MiB response limit can shorten a page further.
- `cursor`: absent/null for the first page, otherwise the opaque token from `next_cursor`. Keep profile and
  filters unchanged. Limits may change. A changed chain, replaced cache, changed filters, different profile,
  unknown or evicted cursor returns `EAWR-MODS-SVC-0005`; restart at the first page.
- All filters are optional and combine with AND. `layer` matches an exact service layer key, or any layer when
  empty. `role` is `winner`, `loser` or `either` (default), relative to that layer; without a layer it tests any
  candidate. A sole provider also counts as a winner. A loser is a shadowed candidate, including an archive
  shadowed by loose bytes within the same layer.
- `type` is an ASCII-insensitive extension, with or without its dot. `path` is an ASCII-insensitive substring
  of the canonical logical path. `conflicts_only` keeps paths with at least two candidates. `hide_identical`
  hides paths where all candidates have identical bytes. Unknown layer keys simply match no rows.

A ready response has `ready: true`, `requires_loadcheck: false`, `index_version: 1`, `chain_fingerprint`,
`rows`, `layers`, `total` (matching path count), and `next_cursor` (string or null). Paths are sorted by
canonical logical path. Each row contains:

- `path`: the logical path;
- `winner`: the first candidate, also present at the start of `candidates`;
- `candidates`: every exact VFS candidate in precedence order, with `layer`, `origin` (`loose` or `archive`),
  `source` (the VFS source id), `size` in bytes, `sha256`, `role` (`win`, `lose`, `only`), and
  `identical_to_winner`;
- `identical`: true only when there are multiple candidates and every digest matches the winner.

Only paths with multiple candidates are opened and hashed. A sole-provider row has `sha256: null`,
`role: "only"` and `identical_to_winner: false`; its size and provenance come from VFS metadata. Conflicting
candidates have lowercase SHA-256 hex digests. A winning conflicting candidate has `role: "win"`; the rest
have `role: "lose"`. Equality is by bytes, not file name, size or archive name.

`layers` covers the entire unfiltered index in mount order, including empty mounted layers. A profile layer
dropped during manifest resolution is not listed. Each entry has `key`,
`files`, `wins`, `loses`, `only`. Each logical path counts once per contributing layer. `only` means that no
other layer supplies the path, even if loose/archive copies coexist within it. `wins` counts paths won over
another layer; `loses` counts paths won by another layer. Thus same-layer shadowed archive entries do not
make that layer lose to itself. Nonzero counts are the layer's win/lose/only flags.

When no current index exists, it is truncated/invalid, or its version differs, the result is:

```json
{"ready": false, "requires_loadcheck": true, "index_version": 1,
 "chain_fingerprint": "<current fingerprint>", "rows": [], "layers": [], "total": 0, "next_cursor": null}
```

Run `loadcheck.start` explicitly to build it. No migration, guessed rows or partial cache is returned. An
incomplete root fingerprint can leave `chain_fingerprint` empty in this unavailable response. A mounted VFS
can still produce a valid file index when a later catalog/type stage reports content errors; file-conflict
information remains available to diagnose those errors. A cancelled/failed helper's scratch index is removed.
An index build/write failure continues the ordinary load check with warning `EAWR-MODS-SVC-0013` and discards
partial output. The service likewise discards an invalid or un-installable index without failing the report;
a failed rebuild removes any prior same-fingerprint index so `files.page` requires a new load check.

The helper builds the index on its existing mount before catalog loading: one `enumerate_raw` supplies paths,
`candidates` supplies exact precedence, and `open(candidate)` reads each conflicting provider once. Sole
providers are never opened. Cache version 1 is a binary length-framed user-data file beside the load-check
report, named by the same chain fingerprint. It starts with `EAWR-FILES` plus LF, a little-endian uint64 version,
a length-framed fingerprint, layer count and length-framed layer keys, row count and length-framed row JSON.
Every count/length is little-endian uint64; a row is bounded by the protocol line size. The service validates
its structure/key and atomically installs it only after the chain's before/after fingerprints agree. A single
validated parsed cache is retained in memory; replacing it invalidates its paging tokens. Mod roots remain
read-only. No file index bytes are added to the profile or the 1.3 load-check report envelope.

### `records.index`, `records.get`, `records.cancel` (protocol 1.9)

`records.index` accepts
`{"profile":"<id>","filters":{"object":"","tag":"","changed_by_layer":"",
"several_mods_same_tag":false,"dropped":false},"limit":100,"cursor":null}`. Filters are optional and
combine; object and tag search are case-insensitive substrings. A page returns
`{"chain_fingerprint":"<fingerprint>","layers":[{"key":"<layer>","kind":"<kind>","priority":0}],
"counts":{"own":0,"overridden":0,"inherited":0,"merged":0,"added":0,"dropped":0},
"total":0,"rows":[{"id":"<id>","type":"<type>","category":"<category>","layers":["<layer>"]}],
"next_cursor":null}`. Counts describe matching records' tag rows before paging; a continuation cursor is opaque
and bound to the profile, fingerprint and filters. Limits are 1..500. The first request for a chain builds its
index in the background and emits `records.progress` events with `stage`, `done` and `total`; its response follows
completion. `records.cancel` takes `{"request":<id>}` and returns `{"cancelled":true}` when it cancels the
active build, otherwise false. The cancelled request receives `EAWR-MODS-SVC-0009`.

`records.get` takes `{"profile":"<id>","id":"<object>","category":"<category>"}` and returns
`{"chain_fingerprint":"<fingerprint>","id":"<object>","type":"<type>","category":"<category>",
"variant_chain":["<type>"],"tags":[{"tag":"<tag>","vanilla":[<value>],
"authored":[{"object_id":"<object>","source":<location>,"registry_order":0,"definition_order":0,"winner":true,
"values":[<node>]}],"effective":[<value>],"how":"own"}]}`. The category disambiguates object IDs.
An authored column is a definition occurrence; `object_id` matches an entry in `variant_chain`, so the client can
place it under that type. Duplicates in one layer remain separate; its location identifies
the layer. A location is `{"logical_path":"<path>","source_id":"<source>","layer_id":"<layer>",
"line":1,"column":1}`. A node is `{"name":"<tag>","raw_text":"<text>","attributes":[{"name":"<name>",
"value":"<value>","source":<location>}],"children":[<node>],"source":<location>}`; empty raw text is
an authored empty value, rendered `(empty)` by the client. An effective value is
`{"node":<node>,"provenance":"<how>","source_object_id":"<type>"}`. `how` is `own`, `overridden`,
`inherited`, `merged`, `added` or `dropped`. Source locations and source object IDs preserve the origin of inherited
values. The service rejects missing, stale or unclean profiles rather than silently inspecting a different chain.

Both reads are long requests. Only one records request runs at a time; a second returns
`EAWR-MODS-SVC-0010`. An engine-linked `eawr_loadcheck --records` helper owns the index and resolves it on
its single request thread. Helpers remain in memory per saved profile and chain fingerprint until shutdown;
cancel, profile replacement/deletion, and installation changes discard affected helpers. There is no on-disk
record cache. Shutdown and stdin EOF cancel outstanding work and close all owned helpers.

`changed_by_layer` matches a tag authored by that layer whose parsed value differs from the vanilla effective
value (including a new tag). Comparison ignores source locations. `several_mods_same_tag` requires two distinct
mod layers authoring the same tag; duplicate occurrences in one layer count once. Tag, layer, same-tag and
dropped predicates must match the same tag row. Classification counts cover all tags of matching records,
including records absent from the profile after registry replacement. Category strings are `game_objects`,
`hardpoints`, `abilities`, `factions`, `campaigns` and `sfx`; `variant_chain` contains object IDs, derived to base.

Missing layers, redirected saved roots, stripped legacy built-ins and unresolved chain warnings/errors return
`EAWR-MODS-SVC-0005` before starting a helper. Registry-wide failures (an unreadable registry file; a single object's variant problem is flagged instead, protocol 1.11) and changes to the chain
while a request runs return `EAWR-MODS-SVC-0011`. An unknown object/category pair returns
`EAWR-MODS-SVC-0005`. The recorded `records.jsonl` session uses a tiny synthetic chain and exercises paging, filters and detail.

Protocol 1.11 (additive; a 1.9/1.10 client that ignores unknown members keeps working):

- **Flagged objects.** An object the engine cannot resolve (a variant cycle, including an object that is a variant
  of itself, or a missing variant base) no longer fails `records.index`. It stays in the index, and its `rows`
  entry (and its `records.get` answer) carries the optional member
  `"diagnostic":{"code":"<engine code>","message":"<text naming the chain>"}`. Its `records.get` answer is a normal
  detail with `"tags":[]`; an object whose variant chain leads into the broken one is flagged the same way. A
  flagged object contributes no tag rows to `counts` and matches only a request with no tag, `changed_by_layer`, `several_mods_same_tag` or `dropped` filter (an `object` filter alone still finds it). The
  member is absent for every other object. Other records index normally.
- **Size-capped detail.** `records.get` keeps its answer under the line limit by construction (a margin below
  1 MiB is reserved for the envelope). When all tags do not fit, the answer holds the leading tags and adds
  `"truncated":true`, `"next_tag_offset":N`, `"tag_count":<total>` and `"tag_offset":<this slice start>`; the client
  asks again with the optional request member `"tag_offset":N` (non-negative integer; default 0) and gets the next
  slice in the same shape, until `truncated` is absent. `tag_count` and `tag_offset` are also present on
  a follow-up slice. Answers that fit are byte-identical to 1.10 (no new members). A single tag row larger than half the answer budget (about 0.5 MiB)
  is sent as its name and `how` only, with empty `vanilla`, `authored` and `effective` arrays and
  `"too_large":true`. An out-of-range `tag_offset` returns `EAWR-MODS-SVC-0005`.
  Compatibility: a 1.10 service answered an oversize detail with `EAWR-MODS-SVC-0011`; a 1.11 service answers a
  partial `tags` list instead, so a client that wants every tag must honour `truncated` (a launcher that ignores
  it silently shows a partial list). A client checks `hello` for protocol minor >= 11 before relying on this.
- **Paging cost.** `records.index` computes the matching rows and `counts` once per filter and chain; further pages
  of the same filter only slice the list. A request answered by an already built helper checks the chain fingerprint
  once (before the answer), not again after it; the request that builds the index still checks before and after.
  No on-disk cache.

Protocol 1.13 (additive; older clients are unaffected):

- **Fingerprint reuse window.** A records request used to hash every file of the chain (mods and game) on each page,
  about 0.2-0.3 s warm. Now the service reuses the chain fingerprint it took for the same profile (same saved
  layers, helper still loaded) for **2 seconds**. Inside the window a page is answered by the loaded helper without a
  new walk. The window ends at once on any `profile.save`, `profile.delete`, `mods.discover`, `mods.changes`,
  `game.find`, `settings.set_builtin_layer` or `chain.*` request (every method other than `hello`,
  `profile.list`, `profile.locks`, `profile.load`, `loadnotes.read`, `files.page` and `loadcheck.*`, which only read the chain), and when the client sends the new
  optional request member `"fresh": true` (boolean, on `records.index` and `records.get`; default false; any other
  type returns `EAWR-MODS-SVC-0005`; a 1.12 service ignored a non-boolean `fresh`, 1.13 rejects it). A `fresh` request always re-fingerprints, and a changed chain then builds a new
  helper as before. Files edited by hand during the 2 seconds are seen by the next request after the window or by
  `fresh`. The window changes no answer content, only when a changed chain is noticed.
  The one case the window hides: a file rewritten on disk by something other than this service, within 2 seconds of
  the last fingerprint and without any service call in between. Only `fresh: true` notices that inside the window.
  Clients should send `fresh: true` on an explicit user Refresh (the launcher needs a follow-up to do so); ordinary
  paging must not.

Protocol 1.14 (additive; older clients ignore the new members):

- **Content digest.** Every mod record (`mods.discover` rows) gains `content`:
  `{"state": "ready"|"pending"|"unavailable", "sha256": "", "files": 0, "bytes": 0, "note": ""}`. It is a SHA-256
  over the mod's file bytes, not just the cheap `fingerprint.tree`: one line `<relative path>\t<size>\t<sha256 of
  the file>\n` per regular file, sorted by the path's UTF-8 bytes, '/' separators, original case, links and
  junctions not followed. `ready`: `sha256` covers `files` files / `bytes` bytes. `pending`: a background job is
  hashing; `files` / `bytes` are the progress so far. `unavailable`: `note` says why ("tree not walked",
  "capped walk", "incomplete walk", "no files", "folder missing", "unreadable file <relative path>",
  "changed while hashing: <relative path>", "folder changed since it was scanned", "cancelled", "internal error: ..."); an empty
  note means nobody asked. Where the service applies a digest to a record (the C++ `ContentDigests::apply`, used by the
  game's save path) a requested digest that cannot be made also adds the record warning `EAWR-MODS-0020`;
  `chain.resolve` rows carry only the `content` object, not that warning.
- **Who starts a job.** `mods.discover` only reports cache hits (`ready`) and never reads a mod file.
  `chain.resolve` returns immediately and, for each layer of the chain, adds `content` to its `rows` entry: `ready`
  from the cache, or `pending` with one low-priority job queued. The job runs on one worker thread, reads in 1 MiB
  chunks at background I/O priority and pauses briefly after every 64 MiB. When the records were discovered with
  `walk_trees: false` there is no tree fingerprint to key on: the answer is `unavailable` with note "tree not walked"
  and no job starts. The next `chain.resolve` or `mods.discover` shows the result.
- **Cache.** `<user data>/mods/cache/content/content-<id>.json`, keyed by the mod key plus `fingerprint.tree` and
  `tree_files`; an unchanged tree is a lookup with zero mod-file reads, a changed fingerprint recomputes. An
  interrupted run keeps `content-<id>.partial.json` (per-file hashes keyed by relative path, size and mtime) so the
  next run skips finished files. A missing, corrupt or old-format cache file is recomputed, never an error.
  In C++ (`ModRecord::content`, `eawr/mods/content_digest.hpp`) the same service object offers request, progress,
  cancel and wait-with-timeout for the game's save path.

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
- Line 1 is a meta line: `{"dir": "meta", "msg": {"protocol": "1.14", "recorded_by": "eawr_mods_service"}}`. A
  hand-written draft has `{"dir": "meta", "msg": {"draft": true, "protocol": "1.0"}}` instead.
- Every other line is one exchanged message in the order it crossed the pipe:
  - `{"dir": "c2s", "msg": {...}}`: a request the service read;
  - `{"dir": "s2c", "msg": {...}}`: a response or event the service wrote.
- A `c2s` line that is not a JSON object cannot be stored as `msg`. It is stored as text instead:
  `{"dir": "c2s", "raw": "<the line, as a JSON string>"}` (bytes of a line that is not UTF-8 are written as
  `\u00XX` of the byte), and a line over the limit as
  `{"dir": "c2s", "oversize_bytes": <byte count>}`. (These two forms are a refinement of the first envelope: a
  malformed line is exactly what `malformed.jsonl` must show.)
- `hello` results (and the `error.data` of `EAWR-MODS-SVC-0006`) contain `service_version`, `engine_build`,
  `game_release` and `kit_build`, which change with every build. The committed transcripts store them normalised, as
  `"service_version":"0.0.0"`, `"engine_build":"0.0.0+transcript"`, `"game_release":"0.0.0"` and `"kit_build":"0.0.0 (transcript)"`; every other byte
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

Name source order for discovery/presentation is player rename (launcher), explicit `modinfo.json` `name`,
its nested `steamdata.title`, reviewed known-chains name, cached Steam title (launcher), then
`Workshop item <id>`. The modinfo title fallback uses only an existing nonempty string, keeps the original
metadata, and never performs a Steam request. A valid explicit name takes precedence even when steamdata
is malformed; neither field supplies a name when its usable string is absent.

Transcript comparison normalizes service-version/build/release hello values, integer `elapsed_ms` to zero,
and native-root-dependent 64-hex `chain_fingerprint` values to 64 zeroes. Raw contract assertions validate
duration type/wall time, fingerprint stability and changes, and status transitions before normalization.

### `loadnotes.read` (protocol 1.7)

Request `{"profile":"<id>"}` reads the last game-written load notes for a saved profile.
Alternatively, `{"chain_fingerprint":"<64 lowercase hex characters>"}` reads notes written without a profile for that chain. Supply exactly one address.
The result is `{"report":<document-or-null>,"current_chain_fingerprint":"<key>"}`.
A missing file returns null. The report contains the writer's schema version, profile and
chain identity, game version, timestamp, totals, truncation flag and groups; each group's
`notes` are the finished rows described in [load notes](load-notes.md). Compare the report's
chain fingerprint with `current_chain_fingerprint` to detect a changed chain; an empty current fingerprint means
the identity is unavailable and the report is unverified. Check `capabilities` before calling this method.
An unsafe path returns `EAWR-MODS-SVC-0005`. Malformed
or unsupported documents return `EAWR-MODS-SVC-0014`.
