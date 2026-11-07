# Shader generation activation

This is project policy for CPU activation, not a claim of device qualification.
Renderer translation remains disabled until P9.

- **SAC-01:** A job owns a content snapshot, selected provenance, contributor
  policy and immutable identity. It checks content hashes before preparation.
  Blocked/absent inputs contain no bytes or hash. Exact repeated effects are
  deduplicated by effect plus the complete P7 key when available, including
  source includes, macros, provenance, schemas and backend policy. Without a
  complete key only byte-identical metadata/input snapshots can deduplicate;
  cache access is disabled. Conflicting winners for the same identity refuse. Effects
  and variants run in canonical order, independent of worker step size.
- **SAC-02:** An activation worker calls `step()` with a finite work budget.
  One work unit is an effect preparation (including policy refusal), or one
  variant admission/prewarm. Counters expose the discovered total separately
  from completed work. Preparation must return the complete supported finite
  technique/state variant set; instance values do not create code variants.
  Injected limits bound inputs, identifiers, variants and retained diagnostics.
  `variant_requests` caps each prepare result before sorting/deduplication;
  `variants` caps unique variants across the job. At the default 65,536 effects
  and 1,048,576 raw requests per prepare, the job-level raw-request bound is
  68,719,476,736 entries processed over the job lifetime, with at most
  1,048,576 raw entries (16 MiB of variant IDs) live in one prepare result.
  Owners may inject smaller resource policy for activation workloads.
  Foreign compiler/device internal execution is not a hard CPU-time bound.
- **SAC-03:** Contributor opt-out and malformed policy bypass preparation,
  including cache reads. Missing inputs select the green placeholder. Existing
  inputs use only their qualified same-name adapter or explicit omission on
  failure. Each failed variant falls back independently. No successful variant
  masks another variant's failed admission.
- **SAC-04:** An owner-supplied preparation operation owns cache validation,
  parsing, translation and supported variant planning. Admission must return
  success only after program admission and prewarm. P5 expression evaluation,
  cleanup qualification, binding/state realization and device checks remain
  prerequisites; a compiler result alone is never an admitted material.
- **SAC-05:** Reserve a monotonic generation ID before dispatch. Only a fully
  completed, noncancelled latest job can publish. Beginning a newer generation
  invalidates older pending jobs. Publisher cancellation serializes with
  publication. A worker exclusively owns stepping and inspecting progress;
  publication follows worker completion/join. Cancellation may be requested
  concurrently. Unexpected callback/allocator failure invalidates the whole
  candidate; expected shader failures return typed diagnostics individually.
- **SAC-06:** Publication replaces one immutable shared generation under a
  mutex. Readers retain old resources until their last generation reference is
  released. No disk-cache eviction invalidates these pins. Late/unknown variant
  lookup is a pure fallback and never schedules compilation or prewarm.
  The future renderer owner gates Start on completed publication, not on the
  first battle draw. P9 owns enabling and installed cold/warm performance proof.

- **SAC-07:** `capture()` inventories all effective compiled effects, followed
  by mod source-only effects with compiled precedence across the mounted chain.
  It separately counts reachable requested stems; missing requested stems get
  absent records. No model/effect filename whitelist selects the inventory.
  The caller captures every contributing layer's policy, using the mod adapter
  for real preserved modinfo metadata and explicit retail-layer declarations.
  Missing or duplicate policy identities invalidate capture. Policy refusal
  precedes byte acquisition, preprocessing and key/cache construction.
- **SAC-08:** Source preprocessing runs during capture through SOURCE-A's
  mounted provider; expanded text and dependency/compiler/macro identities are
  owned afterward. Root and expanded hashes are checked before retaining it.
  Source P7 identities reuse SOURCE-B's graph/closure identity builder. No
  includes reopen from the job or a draw. Individual acquisition/preprocessing
  failures preserve the existing-input route and typed phase, never green.
  Snapshot cancellation discards all candidate inputs. Callback limits and
  bounded foreign-output acceptance apply as in SOURCE-A; external files are
  not a transactional filesystem snapshot.
- **SAC-09:** Optional cached preparation uses P7's validated owned payloads.
  Warm hits skip fresh translation but still run real admission/prewarm for
  every planned variant. Corrupt/invalid/missing entries fall back to fresh
  preparation individually; recoverable read/write warnings do not change
  the route. The owner codec must validate complete payload schemas and return
  independently owned descriptors/resources, never borrowed cache bytes.
- **SAC-10:** Completed effects release raw root bytes and preprocessing
  context before publication. Default retained-input budgets are 64 MiB root
  data plus 64 MiB expanded source; these are injected resource policy, not
  content compatibility limits. Peak additionally includes one bounded parser
  temporary, VFS inventory, compiler outputs, metadata and admitted resources;
  backend owners must bound those independently. Published generations retain
  metadata and required resources only. Keeping an old generation does not
  keep all raw compiler input snapshots alive.
  Numerically, for N simultaneously retained capture/job snapshots, the known
  root-plus-source budget is 128*N MiB: 128 MiB for one activation, or 256 MiB
  if two activations overlap. Including live raw variant IDs gives 144*N MiB
  (144 MiB for one, 288 MiB for two), plus retained generation resources G,
  VFS/inventory/report metadata I and compiler/parser/backend temporaries T.
  Thus the budget expression is 144*N MiB + G + I + T, not a claim that total
  process RAM is 144 MiB. A Job runs only one prepare/admit callback at a time;
  the owner bounds N and cancels/discards stale jobs promptly. Old completed
  readers add to G, not to the 128*N MiB raw-input term. P9 must measure G/I/T
  on installed content before enabling the route on the 4-GB acceptance host.
- **SAC-11:** `complete()` projects P8 details and publishes exactly once.
  Cancelled, failed, stale or duplicate jobs deliver nothing. Reporting counts
  each effect once; any failed variant gives that effect its fallback summary,
  while every exact variant route remains in the generation. Failure Details
  identify technique/state variant IDs. UI delivery and Start gating consume
  this completion in P9; this CPU slice does not enable the renderer route.
