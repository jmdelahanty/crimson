# Crimson Phase 5M Read-Only Zarr Boundary Plan

Date: 2026-07-17

Status: complete on 2026-07-18. Phase 5M.0 through Phase 5M.4 and the full
acceptance gate are complete.

Lifecycle: **archived completed plan**. The implementation contracts,
tests, and `docs/reference/phase5m/` are the retained authorities.

## Purpose

Phase 5M begins an incremental decomposition of `ZarrDetectionLoader` at a
stable read-only boundary. The chaser-distance polar inset is the pilot because
the maintained application already implements it, the representative recording
contains a complete production dataset, and its scientific inputs are bounded
enough to characterize precisely.

This is not a wholesale loader rewrite. `ZarrDetectionLoader` remains the
Linux/Windows compatibility facade for responsibilities that have not been
extracted. Storage and mutation contracts that are still changing remain out of
scope.

## Target Architecture

```text
legacy ZarrDetectionLoader adapter ----+
                                        +-> portable read-only repository data
TensorStore repository adapter --------+              |
                                                       v
                                            shared polar scene builder
                                                       |
                                      +----------------+----------------+
                                      v                                 v
                              maintained ImGui                    Metal renderer
```

Shared scientific and presentation code must not depend on Metal, CUDA,
OpenGL, AVFoundation, native decoder objects, or `ZarrDetectionLoader` concrete
types. Platform and storage adapters may depend on their owning implementations.

## Work Packages

### 5M.0 Responsibility and Behavior Inventory

Status: complete on 2026-07-17. The inventory, migration ledger,
characterization results, dependency baseline, production samples, and
fast-math correction are recorded in
`docs/archive/macos-port/phase5m/crimson_macos_phase5m0_zarr_loader_inventory.md`.

- Map the polar loader's run/component selection, schema paths, attribute
  interpretation, eager storage, frame lookup, color selection, radial scale,
  and UI consumers.
- Add characterization fixtures before changing behavior.
- Record remaining `ZarrDetectionLoader` responsibilities and consumers in a
  migration ledger rather than treating the pilot as full decomposition.
- Add a dependency rule that prevents new shared or Mac modules from including
  `zarr_loader.h`.

### 5M.1 Portable Polar Contract

Status: complete on 2026-07-17. The backend-neutral types, repository
interface, exact-frame and availability rules, convention validation, color
provenance, radial-scale policy, and focused tests are documented in
`docs/archive/macos-port/phase5m/crimson_macos_phase5m1_portable_polar_contract.md`. Mac passed 40/40
tests; the isolated Linux/NVIDIA build passed 29/29 tests and its authenticated
GoodCopBadCop 0:300 production smoke.

- Define backend-neutral descriptor, frame-sample, point, provenance, and
  availability types.
- Preserve exact camera-frame identity, run and component identity, distance
  units, coordinate frame, angle convention, chaser index, validity, and color
  provenance.
- Distinguish dataset unavailable, unsupported metadata, exact frame missing,
  valid frame with no valid points, ready frame, and read failure.
- Define radial-scale and color-precedence behavior explicitly instead of
  leaving it inside a renderer.
- Reject or normalize unsupported angle and coordinate conventions before scene
  construction; the renderer must not silently assume them.

### 5M.2 Storage Adapters

Status: complete on 2026-07-17. The read-only TensorStore repository, bounded
asynchronous buffer, maintained compatibility adapter, shared-fixture
agreement, production comparison, and non-mutation evidence are documented in
`docs/archive/macos-port/phase5m/crimson_macos_phase5m2_polar_storage_adapters.md`.

- Implement a read-only TensorStore repository with bounded asynchronous frame
  access for the native Mac path.
- Implement a legacy adapter over the maintained loader for Linux/Windows.
- Make both adapters produce the same portable descriptor and frame sample for
  the same archive and camera frame.
- Preserve the maintained latest-complete and compatibility fallback behavior
  unless fixtures demonstrate that it is unsafe.
- Do not create groups, update attributes, or open a write repository.

### 5M.3 Shared Scene and Platform Presentation

Status: complete on 2026-07-18. The backend-neutral scene builder, maintained
ImGui adapter, native Mac TensorStore/buffer/Metal adapter, exact-frame runtime
metrics, deterministic scene and Metal coverage, and checkpoint verification
are documented in
`docs/archive/macos-port/phase5m/crimson_macos_phase5m3_shared_polar_scene.md`.

- Build one backend-neutral polar inset scene from the portable frame sample
  and controls.
- Preserve front/left/right/behind orientation, radial rings, point positions,
  colors, labels, readout, opacity, sizing, and layer order.
- Adapt the maintained ImGui path and the Mac Metal path to the same scene.
- Remove direct `ZarrDetectionLoader::ChaserDistancePolarFrame` consumption
  from shared camera-view presentation code.

### 5M.4 Cross-Platform Acceptance

Status: complete on 2026-07-18. Adapter agreement, controlled same-frame
OpenGL/Metal captures, declared visual tolerances, final Mac and NVIDIA tests
and production smokes, fast-math audit, and the 10,736-file nonmutation proof
are documented in
`docs/archive/macos-port/phase5m/crimson_macos_phase5m4_cross_platform_acceptance.md`.

- Compare adapter descriptors and exact-frame samples on synthetic fixtures and
  the production GoodCopBadCop archive.
- Test every availability state, convention validation, invalid values,
  duplicate/missing frame IDs, color fallback, and radial-scale behavior.
- Apply the existing Phase 5 geometry, mask/vector, channel, and semantic-text
  tolerances to controlled polar scenes.
- Capture equivalent maintained and Mac polar-inset states at the same logical
  size and camera frame.
- Run Mac and isolated NVIDIA tests plus representative production smokes and
  verify that archive metadata remains unchanged.

## Acceptance Gate

Phase 5M is complete only when:

- both adapters agree on run/component provenance and exact-frame scientific
  values for the accepted fixtures;
- coordinate and angle conventions are carried, validated, and tested;
- missing frames cannot be mistaken for valid empty frames;
- the shared scene builder contains no storage or platform dependencies;
- shared UI and Mac sources do not consume concrete `ZarrDetectionLoader`
  types for the migrated polar feature;
- the maintained Linux renderer and Mac renderer consume the same scene
  semantics and pass declared visual tolerances;
- Mac and NVIDIA builds, deterministic tests, and production smokes pass; and
- no write contract, provisional schema, or unrelated loader responsibility is
  introduced or migrated.

## Follow-On Rule

Later phases extract only the read-only loader slice needed by the feature they
are implementing. Every extracted slice requires a portable contract, a
TensorStore adapter, a maintained compatibility adapter, shared fixtures, and
cross-platform acceptance. Full removal of `ZarrDetectionLoader` is considered
only after its migration ledger has no required consumers.

Phase 5N applies this pattern to the remaining stimulus event and step-direction
camera overlays. Production-tail acceptance, movement-trail coordinate policy,
chaser bounding-box densification, and edit/write workflows remain separate
gates.
