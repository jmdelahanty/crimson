# Shared Affiliated-Media Discovery Checkpoint

Date: 2026-07-31

Status: portable path policy extracted; macOS and Linux shells adopted for
standard recording archives; clipped compatibility remains platform-owned

## Outcome

Crimson now has one backend-neutral policy for inferring a recording root and
resolving a persisted recording path. The policy handles:

- recording-relative paths;
- absolute paths that still exist on the current host;
- absolute paths whose mount prefix changed but whose recording identity and
  suffix remain stable; and
- unrelated absolute paths, which remain unresolved rather than being
  guessed from a basename.

`ArchiveContext` and the legacy `ResolveAffiliatedVideoPath` compatibility
helper both use this policy. This removes the duplicated macOS/Linux mount
relocation logic that caused packaged Linux releases to miss videos whose
metadata retained a `/groups/...` path while the same recording was mounted at
`/mnt/...`.

## Shared Discovery Contract

`AffiliatedVideoDescriptor` now reports both scientific metadata provenance
and host path-resolution provenance:

- stored path;
- inferred recording root;
- resolved path;
- metadata source;
- resolution kind; and
- metadata path/key.

The maintained Linux standard-archive path now calls
`DiscoverAffiliatedVideo` through `ArchiveContext`, matching macOS. Invalid or
stale authoritative metadata fails closed. The legacy loader hint is consulted
only when the shared repository reports that affiliated-video metadata is
absent; it is not a fallback for invalid authoritative metadata.

Clipped collections still use their existing clip-row resolver and legacy path
candidate rules. They do use the shared recording-root and foreign-prefix
relocation policy, but moving clipped media selection behind the strict shared
repository requires a separate clipped-media contract.

## Ownership Boundary

The shared layer owns metadata discovery and filesystem path policy. It does
not own:

- AVFoundation, FFmpeg, CUDA, Metal, or OpenGL resources;
- decoder creation or shutdown;
- macOS process relaunch;
- Linux decoder-thread and ring-buffer state;
- clipped run switching; or
- UI error presentation.

Those remain platform/application adapter responsibilities.

## Verification

The portable tests cover recording-root inference, relative paths, existing
absolute paths, foreign absolute relocation, unrelated recording identities,
and empty paths. Repository tests additionally cover authoritative Zarr v2/v3
inventory, legacy precedence, ambiguity, missing files, traversal rejection,
and reported resolution provenance.

The macOS release target and focused path/archive/repository tests pass. A
fresh isolated Ubuntu 22/CUDA 12.4/TensorRT 10 build also passes the portable
path, UI path, archive-context, and affiliated-video repository tests. Its
relocatable app drop has no unresolved libraries and passes the runtime audit
against the workstation NVIDIA driver.

The authenticated Linux GUI playback smoke resolved the maintained GoodCop
BadCop archive's recording-relative camera path through the shared repository,
auto-loaded both camera and stimulus media, and presented through frame 300 in
approximately 3.0 seconds. This validates the strict path in the real NVIDIA
shell rather than only in headless repository tests.

The first fresh Ubuntu 22 container configure also exposed a release-wrapper
variable mismatch: the wrapper supplied `CUDA_CUDA_LIBRARY`, while Crimson's
CMake target consumes `CUDA_DRIVER`. The wrapper now binds the host driver at
the exact variable used by `redgui` and `crimson_decode_smoke`; this prevents a
clean app-drop build from depending on a retained CMake cache.

## Remaining Work

The Linux application currently opens a short-lived shared `ArchiveContext`
for affiliated-video discovery after its legacy archive loader succeeds. A
future session-loader extraction should make the shared archive context a
session-owned dependency reused by every strict repository. That lifetime
change is intentionally separate from this path-policy adoption.
