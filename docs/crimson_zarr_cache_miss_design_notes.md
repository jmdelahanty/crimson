# Crimson Zarr Cache Miss Design Notes

Date anchored: 2026-04-29.

## Purpose

Explain how to think about cache misses when Crimson reads chunked Zarr data,
especially for refined subject-mask overlays. This is written as a learning
note, not as a strict implementation contract.

## The Core Idea

A cache is a faster place to keep data you might need again.

A cache miss means:

> I need data, but it is not in the fast place, so I must go to a slower place.

In Crimson, one user-visible overlay can touch several cache layers:

1. NVMe/filesystem storage
2. OS page cache
3. TensorStore/Zarr chunk reads and decompression
4. Crimson’s decoded mask chunk cache
5. Crimson’s CPU display representation, currently sparse pixel indices
6. Crimson’s GPU texture cache

A miss at any layer can be fine in isolation. A visible hitch happens when a
frame change forces several misses in a row.

## The Mask Overlay Pipeline

For a refined subject mask overlay, the slow path looks like:

```text
display frame changes
-> resolve detection row / ROI row
-> find mask chunk for that ROI
-> read Zarr chunk from storage
-> decode/decompress chunk
-> scan dense mask pixels
-> build CPU display cache
-> create/upload OpenGL texture
-> submit ordered draw calls
```

The warm path should look more like:

```text
display frame changes
-> resolve detection row / ROI row
-> mask chunk already decoded
-> GPU texture already exists
-> submit ordered draw calls
```

The second path is what makes scrubbing and playback feel responsive.

## Chunked Formats

Zarr stores arrays in chunks. Instead of reading one enormous array at once,
Crimson reads blocks of the array.

For `masks_roi`, the logical shape is:

```text
roi x component x height x width
```

If the chunk layout groups nearby ROI rows and all mask components together,
then one chunk read can provide:

- the current frame’s ROI
- nearby ROI rows
- `subject_body`
- `eye_left`
- `eye_right`
- `swim_bladder`

That is good when the UI access pattern has locality.

## Locality

Locality means “nearby things tend to be needed soon.”

There are two important kinds:

- Spatial locality: if you read ROI 1000, you may soon read ROI 1001.
- Temporal locality: if you read ROI 1000 now, you may read it again soon.

Sequential playback has strong locality. Frame-by-frame scrubbing usually has
strong locality. Random review jumps have weaker locality.

Chunked storage works best when chunk shape matches locality.

## When Chunking Helps

Chunking helps when:

- playback moves mostly forward or backward through nearby frames
- neighboring displayed frames map to nearby ROI rows
- the selected mask components live in the same chunk
- the cache is large enough to keep the recent chunk alive
- prefetch can read nearby chunks before the user needs them

In that case, the first frame may pay a cold-read cost, but nearby frames hit
cache.

## When Chunking Hurts

Chunking can hurt when:

- the user jumps randomly across the archive
- chunks are huge and only one small slice is needed
- chunks are tiny and metadata/file overhead dominates
- each component requires a separate chunk read
- the app repeatedly converts the same chunk into display data
- the app repeatedly uploads equivalent GPU textures

The problem is not chunking itself. The problem is mismatch between chunk shape,
cache granularity, and access pattern.

## Working Set

The working set is the data needed to make the current interaction smooth.

For mask playback, the working set might be:

- current frame’s ROI/component masks
- a few frames before and after the current frame
- the current Zarr chunk
- adjacent Zarr chunks
- GPU textures for recently visible ROI/component pairs

For random review navigation, the working set is different:

- current review frame
- maybe previous/next review frames
- less value from broad sequential prefetch

For editing, the working set is narrower:

- active ROI
- active component
- immediate undo/redo state
- high priority for edit preview responsiveness

Good cache design starts by naming the working set for the user interaction.

## Cache Granularity

Cache granularity means “what unit do we keep?”

Possible units for Crimson masks:

- raw Zarr chunks
- decoded dense chunk data
- per-ROI dense mask planes
- per-ROI/component sparse pixel indices
- per-ROI/component row spans/RLE
- per-ROI/component GPU textures

Each has tradeoffs.

Raw or decoded chunks:

- good reuse if many components or nearby ROIs are needed
- can be larger than one displayed frame needs

Sparse pixel indices:

- easy to draw as scatter fallback
- can be large for body masks
- not ideal as the long-term primary display cache

Row spans/RLE:

- compact for contiguous masks
- good source for filled textures and contours
- better long-term CPU display cache for masks

GPU textures:

- best for repeated filled rendering
- expensive on first upload
- must be invalidated if source mask changes

## Cache Keys

A cache key must identify exactly what data the cached object represents.

For mask textures, useful key fields include:

- archive/source identity
- run path
- dataset path
- ROI index
- component label
- source channel index
- mask shape
- source version or edit revision

If the key is too weak, Crimson can show stale or wrong overlays. If the key is
too strong, useful cache hits disappear.

## Eviction

Caches must be bounded. Otherwise, long sessions can grow memory or GPU usage
without limit.

Least-recently-used eviction is a reasonable default:

- recently visible frames stay warm
- old random jumps eventually fall out
- implementation stays simple

For GPU textures, eviction matters because texture memory can be limited and
leaks are painful.

## Prefetch

Prefetch means reading likely-needed data before it is requested.

Useful prefetch patterns:

- after loading chunk N, start loading chunk N+1 during forward playback
- when scrubbing slowly, keep nearby chunks warm
- when a review list is active, prefetch next/previous review frame chunks

Prefetch can hurt when:

- it competes with the current frame’s urgent work
- the user jumps randomly and prefetched data is never used
- it reads too much and evicts more useful cache entries

Good prefetch is modest and tied to the current interaction mode.

## Cold vs Warm Latency

Cold latency is the first-time cost.

Warm latency is the repeated-use cost.

For Crimson overlays, it is acceptable if the first frame after a large random
jump takes a little longer. It is not acceptable if every adjacent frame pays
the full cold cost.

The design target should be:

- cold miss: tolerable
- warm frame: smooth
- sequential scrub: mostly warm
- edit preview: immediate

## Drawing Order Is Different

Overlay draw order matters visually, but it is not usually the main performance
risk.

Drawing in order:

```text
body fill
swim bladder fill
eye fills
axes
headings
keypoints
editing handles
```

does mean the draw submissions happen in order. But a few transparent textured
quads and line/point overlays are normally cheap for the GPU.

The expensive part is usually preparing those draw calls:

- reading chunks
- scanning masks
- building CPU display caches
- uploading textures

So we should optimize cache behavior before worrying about the fact that layers
are ordered.

## Crimson-Specific Guidance

For refined subject masks, prefer reading all visible components from the same
chunk read.

Good:

```text
read chunk once
extract subject_body, eye_left, eye_right, swim_bladder
cache component display data
draw selected components in semantic order
```

Avoid:

```text
read body component
read left eye component
read right eye component
read swim bladder component
scan/upload each from scratch every frame
```

The current implementation is moving in the right direction because unified
subject masks are decoded through one chunk path and component visibility is a
render choice.

## Practical Questions To Ask

When performance is questionable, ask:

- Is this a cold frame or a warm frame?
- Did we read from Zarr this frame?
- Did we decode a new mask chunk?
- Did we upload new GPU textures?
- Are we rebuilding data because a visibility toggle changed?
- Are we reading one chunk per component?
- Is the user sequentially scrubbing or randomly jumping?
- Is the working set larger than our cache?
- Is the cache key too weak or too strong?

These questions usually identify the real bottleneck faster than guessing.

## Rule Of Thumb

Optimize for interaction mode:

- playback: adjacent chunk prefetch and rolling cache
- scrubbing: keep nearby chunks and textures warm
- random review jumps: tolerate cold misses, avoid repeated misses on the same
  frame
- editing: prioritize active ROI/component and avoid background work that blocks
  interaction

Access pattern is the center of the design. Chunked Zarr is helpful when chunk
shape, cache granularity, and UI movement line up.
