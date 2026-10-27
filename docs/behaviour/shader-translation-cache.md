# Activation shader translation cache

This is an EAWR cache policy, with no claim about the original game's cache.

- **STC-01 — content identity.** Activation resolves the effective compiled/source
  form and the entire ordered VFS include graph afresh before lookup. The framed
  digest includes logical roots, root/include bytes, include parent context,
  ordered macros/options, stage entries/profiles, verified complete compiler
  package/lock digest and ABI/capability manifest, descriptor/parser, translator,
  emitter, binding/state/technique/admission/selection/resolver policies, engine
  and shader ABI, language/backend/renderer/platform/architecture, and finite
  capability/variant tokens. Effective layer/mount and opt-out provenance remain
  in the outer activation result even when payloads share a key. Current time,
  team, light, fade and instance values stay dynamic. Timestamps are never content
  identity. Unsupported compiled content does not silently fall back to source.
- **STC-02 — owned and validated.** The cache holds opaque owned serialized bytes
  supplied by the compile/reflect owner. A required decoder/validator reconstructs
  P1 descriptors into activation-owned storage before admission on every lookup;
  the same validator runs before store. Length, version, key and payload SHA-256
  are checked first. Renderer state and borrowed or DLL-owned pointers never
  cross this boundary. A corrupt record is a recoverable miss, and regeneration
  belongs to activation. The cache does no translation or renderer publication.
- **STC-03 — optional player storage.** Only an injected dedicated player user-data
  directory is used. Missing, denied, busy or full storage reports a cache status
  and leaves uncached activation usable. Opt-out is checked before any cache I/O,
  including explicit invalidation, for both source and compiled selection and
  warm entries. No generated
  materials or source enter mod/game roots, reports or release packages.
- **STC-04 — bounded retention.** Injected file/entry/scan limits bound disk use,
  allocation and cleanup work. Verified hits refresh recency; eviction sorts by
  recency and then digest for deterministic ties. Recency never affects identity.
  Returned payload bytes are independently owned, so eviction or invalidation
  cannot invalidate an in-flight generation. P6 retains generation resources and
  owns cancellation and atomic renderer handover.
- **STC-05 — publication.** Writers acquire a nonblocking operating-system file lease and
  publish a complete versioned, hashed envelope with a same-directory rename.
  Readers accept only complete verified records. Concurrent writer leases make
  writes unavailable, with uncached activation still usable; readers may continue
  using complete records. The operating system releases the lease if its process
  exits or crashes; a remaining lock-file name does not block future writes.
  A failed write never admits its temporary file. Windows uses an exclusive open
  handle; POSIX uses an advisory exclusive file lock. Both refuse symlink locks.
  Root, entry, temporary and lease links are refused with an I/O error without
  modifying their targets. Contract tests hold an independent native lease,
  race two writer threads and exercise each link position. If host privileges
  prevent link creation, those cases print an explicit skip reason.

The compile/reflect payload format and finite capability/variant vocabulary
belong to their producers. This helper does not define another effect descriptor
or enable the production translation route.
