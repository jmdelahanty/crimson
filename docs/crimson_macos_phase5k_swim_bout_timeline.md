# Crimson Phase 5K Swim-Bout Timeline

Date: 2026-07-15

Phase 5K adds the maintained swim-bout review context to the native macOS
Motion timeline. It introduces a portable candidate/interval contract, a
native TensorStore repository for both production layouts, bounded asynchronous
pages, and direct/core bout bands plus an optional detector-response trace.

This checkpoint does not claim full Phase 5 UI parity. Production-tail data
acceptance, edit/review workflows, the larger docked workspace, multiwindow
behavior, and the final screenshot/image-difference gate remain open.

## Maintained Semantics

A swim bout is an inclusive camera-frame interval. An optional core interval is
also inclusive and must be contained by the outer interval. The renderer maps
the persisted `start_frame`, `end_frame`, `core_start_frame`, and
`core_end_frame` directly; it does not add one to an endpoint. A
`gap_censored` flag is retained as provenance.

The portable descriptor preserves the candidate information needed for
selection and scientific review:

- source group, run, track, candidate, signal, role, and speed level;
- compact or hierarchical source layout;
- source motion run and detector/movement/path provenance;
- detector method, source path, label, and units;
- threshold, smoothing, duration, gap, peak-prominence, and peak-width
  parameters; and
- bout and detector-sample counts plus latest/default flags.

Candidate compatibility first matches the motion run and track. It then
matches the selected motion variant against the candidate speed level,
detector source level, movement source level, path-distance source level, or a
case-insensitive speed-level reference in the detector source path. A still
compatible persisted choice wins. Otherwise selection prefers latest plus
default, then latest, then default, then the first compatible candidate.

The compact layout preserves the maintained rule that one candidate ID is
selected for a run while every signal variant belonging to that candidate is
exposed. Candidate selection uses the explicit default ID when present, then
the first row marked default, then the first candidate row. Run-pointer
precedence is `latest`, `latest_completed`, `latest_complete`, then
`latest_success`.

Malformed outer or core intervals fail the page closed with `ReadFailed`.
This is an intentional contract clarification: corrupt scientific boundaries
are reported instead of being rendered as plausible bands.

## Zarr and TensorStore

`OpenSwimBoutTimelineRepository` reads both maintained archive organizations:

- compact tabular v2 under candidate/signal indexes, a shared bout table, and
  detector-signal matrices; and
- hierarchical v1 under per-speed-level bout and detector arrays.

The adapter accepts the integer, floating-point, boolean, native-string, and
fixed-width byte representations found in current and older archives. An
explicit `--swim-bout-run` restricts discovery to one run. Without it, all
readable runs are exposed and their latest/default metadata drives the initial
choice.

Detector data is lazy. Current compact recordings store each detector signal
as one physical row-sized chunk, so the first request reads that row once and
caches it inside the repository. Each 4,096-frame UI page then selects its
range and applies extrema-preserving decimation, retaining endpoints, the
cursor neighborhood, and bucket minima/maxima. This avoids repeated network
reads while bounding plot upload and rendering work.

The path is native C++ TensorStore code. It does not use Python, Python Zarr,
or the legacy playback `malloc` ring. Repository pages are immutable shared
objects. `SwimBoutTimelineBuffer` owns one worker, generation cancellation,
and a bounded page cache whose key includes the candidate and detector toggle;
stale results cannot replace a newer selection or seek.

## Native macOS UI

The Motion tab now includes:

- a compatible candidate selector that follows the active motion source;
- `Bout spans` and `Detector response` controls;
- translucent outer bout spans and stronger core spans behind speed traces;
- the detector-response line above the physical speed traces; and
- the normal presented-frame cursor and exact-frame plot seeking.

Bands and detector samples use the motion page's canonical, undecimated
frame/time mapping. This matters across sparse or irregular motion mappings:
decimated display traces must not redefine camera-frame placement. The
detector is explicitly labeled as a detector response and `not physical
speed`, even when its stored units resemble speed.

Launch controls are:

- `--no-swim-bout-timeline` disables the optional repository;
- `--require-swim-bout-timeline` makes it a deterministic playback-smoke
  requirement; and
- `--swim-bout-run <name>` requests one persisted run.

## Deterministic Coverage

`swim_bout_timeline_tests` covers run/track/variant compatibility, preferred
and default candidate selection, case-insensitive source-path matching,
inclusive interval intersection, strict outer/core validation, page bounds,
extrema-preserving detector decimation, asynchronous cache reuse, and stale
generation rejection.

`swim_bout_timeline_repository_tests` writes compact and hierarchical Zarr
fixtures through TensorStore. It verifies mixed numeric and string encodings,
run-pointer precedence, compact candidate/default rules, detector fallbacks,
all signal variants, requested and missing runs, metadata/provenance, and lazy
window reads. The motion timeline test also verifies that sparse-gap frame
mapping uses its undecimated canonical arrays.

The final macOS headless suite passed all 33 tests. The isolated NVIDIA build
completed with CUDA 12.4 architectures 80 and 86, TensorRT 10.0.1.6, OpenCV
4.10.0 with SFM, NVIDIA FFmpeg/NVDEC/OpenGL, all three CUDA translation units,
and the maintained `redgui`; all 22 portable tests passed.

## Production Validation

The mounted June 14 GoodCopBadCop archive exposed 10 compatible candidate
signals. The default was candidate 0, signal 4 from the latest compact run,
with 2,203 bouts and 120,221 detector samples. A representative page around
frame 70,656 covered frames 68,608 through 72,703, returned four intervals,
read 4,096 detector rows, and published 711-712 extrema-preserving points.

The mounted May 29 GoodCopBadCop archive exercised the hierarchical layout.
It selected
`swim_bouts_goodcopbadcop_arena1_core_20260714_v003/candidate_0/signal_4/speed_exponential`,
with 1,080 bouts and 143,305 detector samples. Its representative page
contained 23 intervals and 812 detector points.

The native Mac production smoke required the new repository and passed frames
68,608 through 69,008. It presented 244 swim-bout pages, resolved one network
page in 307 ms, then served 244 cache hits with three intervals, 4,096 source
detector rows, 683 plotted points, no missing/failed/discarded result, zero
final PTS error, and nominal thermal state.

The server-local NVIDIA production probe matched the Mac June archive result:
10 candidates, the same default signal, 2,203 bouts, 120,221 samples, and the
same representative interval/decimation result. The authenticated maintained
GUI smoke then passed frames 0 through 300 with 357 presentations in 2.994
seconds. The maintained application continues to use its existing eager
timeline loader; the new portable presentation path is gated by the native Mac
smoke and the cross-platform repository/probe tests.

## Remaining UI Parity

Phase 5K establishes the shared swim-bout contract and native read-only Motion
review surface. Phase 5 remains active. The revised next checkpoint is Phase 5L
maintained workspace parity using stable playback and read-only contracts. It
will inventory and reproduce the actual Linux/Windows window topology, visual
composition, stable panels, lifecycle behavior, and screenshot tolerances
before any edit/write implementation. Storage-dependent edit/review workflows
are explicitly deferred until their shared contracts stabilize. Production-tail
acceptance and stimulus overlay parity beyond event/context views also remain
open.
