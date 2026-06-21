# Crimson Threading Architecture Notes

Date anchored: 2026-06-21.

## Purpose

This note captures the current Crimson threading shape and the main risks that
showed up while investigating refined subject-mask RLE playback performance.

The short version: Crimson's threading model is pragmatic and currently fast
enough, but several synchronization boundaries are implicit. Before adding more
asynchronous loaders or render-side background work, those boundaries should be
made explicit.

## Current Shape

Crimson is not built around one general task scheduler. It has a handful of
specialized worker paths coordinated by the GUI/main thread.

The main GUI thread owns:

- the GLFW/ImGui frame loop in `src/red.cpp`
- OpenGL presentation
- camera view composition
- most playback state transitions
- UI window construction and perf logging

Long-lived media workers include:

- one decoder thread per camera video, started from `src/red.cpp`
- optional image-loader threads for image-sequence inputs
- a stimulus playback decoder thread when stimulus media is loaded

Short-lived or ad hoc background work includes:

- an owned refined mask chunk prefetch worker in
  `ZarrDetectionLoader`
- `std::async` work for some write workflows
- YOLO worker threads on legacy detection paths

The current architecture is closer to:

```text
GUI/main thread
  -> owns frame timing, UI, rendering, playback decisions
  -> reads latest decoder state
  -> marks displayed ring-buffer slots writable again

decoder worker threads
  -> demux/decode media
  -> publish frames into per-camera ring buffers
  -> update latest-decoded atomics

Zarr/mask code
  -> mostly synchronous reads on demand
  -> refined mask chunks can be prefetched by a bounded loader-owned worker
```

than to a formal actor model, job system, or reactive pipeline.

## Good Parts

The media decoder lifecycle is recognizable:

- threads are created when media is loaded
- workers observe `DecoderContext::stop_flag`
- the app joins decoder threads on shutdown
- latest decoded frame numbers are published through atomics
- decode timing is exposed through `DecoderPerfSample`

That shape is useful. It lets decode run ahead of the GUI, while preserving a
single render owner. The GUI thread being the only OpenGL/ImGui owner is the
right default. The system should keep that property.

The recent refined subject-mask RLE work also showed that small background
prefetch can be useful. RLE chunk misses can cost around `120-140 ms`, but when
prefetched ahead of playback they do not dominate the hot frame path.

## Weak Boundaries

### Ring Buffer Metadata

`PictureBuffer` metadata in `src/decoder.h` is shared directly between decoder
threads and the GUI thread:

- `available_to_write`
- `frame_number`
- `local_frame_number`
- `frame_pts`
- `frame_source_code`
- `color_matrix`

Decoder workers write these fields in `src/decoder.cpp`. The GUI thread reads
and mutates them in `src/red.cpp` while selecting and releasing displayed
frames.

That protocol is simple and has been useful, but the fields are plain C++
objects, not atomics and not guarded by a slot-level lock. In strict C++ terms,
that is a data race. It may behave predictably on the current platform, but it
is not a hard synchronization contract.

### Global Runtime State

`src/global.h` exposes process-wide decode and YOLO coordination state:

- `g_mutexes`
- `g_cvs`
- `g_ready`
- `window_need_decoding`
- `latest_decoded_frame`
- `decoder_perf_samples`
- `g_seek_info_mutex`

These globals make it easy for older code paths to coordinate, but they make
ownership harder to reason about. They also make it easier for new features to
join the global state graph instead of declaring a narrow runtime dependency.

### Seek State Locking

Seek state is guarded by `g_seek_info_mutex`, but the contract is spread across
the GUI, decoder workers, image loader, and stimulus playback code. It works,
but the protocol is not represented as one object with explicit operations like
`requestSeek`, `claimSeek`, and `completeSeek`.

This increases the cost of changing seek behavior because correctness depends
on all paths preserving the same informal sequence.

### Mask Prefetch Worker

The refined mask chunk prefetch path used to create detached threads that
captured `this`:

```text
requestEyeMaskChunkPrefetch(...)
  -> std::thread([this, chunk_id] { ensureEyeMaskChunk(...); }).detach()
```

The cache and in-flight sets prevent duplicate work, but detached loader-owned
threads have two architectural problems:

- Lifetime: the thread depends on the loader still being alive.
- Back-pressure: the app does not have a central place to limit, cancel, or
  flush prefetch work.

This is the part most likely to cause future trouble if more Zarr readers adopt
the same pattern.

Current status: this has been replaced with a loader-owned worker. Prefetch
requests now go through a bounded queue, duplicate suppression still uses the
existing in-flight chunk set, and the worker is stopped and joined before
archive reload, mask-state clear, and loader destruction.

## Performance Interpretation

The recent RLE playback smoke after caching the stimulus event timeline showed:

- playback rate around `99.9 fps` for a `100 fps` source
- UI build p50 around `2.8 ms`
- stimulus timeline p50 around `1.4 ms`
- refined subject-mask RLE data lookup p50 around `0.07 ms`
- mask draw p50 around `0.45 ms`

That means the immediate issue was not the decoder threading model and not RLE
decode in the hot path. The large visible slowdown came from repeated GUI-side
timeline work. Once the timeline was cached, the existing worker structure kept
up with realtime playback.

The remaining concern is architectural durability, not current throughput.

## Recommended Direction

Do not rewrite the whole threading model. Instead, add explicit ownership and
synchronization at the existing boundaries.

### 1. Make Buffer Slot State Explicit

Replace plain shared `PictureBuffer` metadata with either:

- atomics for simple state fields, or
- a small `FrameSlotState` guarded by a slot-level mutex, or
- a single-producer/single-consumer ring abstraction with acquire/release
  methods

The goal is to make these operations explicit:

```text
decoder claims writable slot
decoder publishes decoded frame metadata
GUI acquires readable slot
GUI releases displayed slot back to writer
```

This does not need to change the actual frame storage buffers.

### 2. Keep Mask Prefetch Owned

The refined mask path now uses a small loader-owned prefetch executor:

- one worker thread is enough initially
- bounded queue of chunk IDs
- duplicate suppression through the existing in-flight set
- cancellation on archive unload
- joined during loader/session teardown

The public behavior stays the same:

```text
ensure current chunk synchronously
queue adjacent chunk prefetch
```

The important change is that prefetch lifetime becomes owned and cancellable.

### 3. Wrap Decode Globals In A Runtime Object

Move the decode globals toward a `PlaybackRuntime` or `DecodeCoordinator`:

```text
PlaybackRuntime
  camera decode flags
  latest decoded frames
  decoder perf samples
  seek coordinator
  decoder thread handles
```

The first slice can be mechanical: preserve behavior, but pass a runtime object
instead of reaching through `global.h`.

### 4. Keep Rendering Single-Threaded

Do not move ImGui or OpenGL draw submission to worker threads. The current
single render owner is a good constraint. Background work should produce
immutable or explicitly synchronized data that the GUI thread consumes.

## Suggested Refactor Sequence

1. Add a `FrameSlotState` wrapper around `PictureBuffer` metadata and convert
   one decode path.
2. Convert the main camera decoder path to use explicit slot publish/release.
3. Convert image loader and stimulus playback to the same slot API.
4. Keep the owned mask prefetch worker covered by reload/shutdown smoke tests.
5. Add shutdown/load-unload tests or smoke tooling that exercises archive reload
   while prefetch is active.
6. Move global decode maps into a runtime object once the synchronization
   boundaries are explicit.

## Non-Goals

Do not use this work to:

- move rendering off the GUI thread
- rewrite playback timing
- replace TensorStore
- redesign the RLE or dense mask contracts
- introduce a large generic job system before the smaller boundaries are fixed

The near-term goal is not a new architecture. It is to make the current
architecture's ownership and thread handoffs explicit enough that future async
features are safe to add.
