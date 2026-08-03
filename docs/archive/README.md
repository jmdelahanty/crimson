# Crimson Documentation Archive Ledger

Date: 2026-08-03

This directory tracks documents that are safe to remove from the active
planning surface. Archive-ready does not mean disposable: implementation
records, acceptance evidence, and frozen contracts remain useful historical
evidence. Files stay at their current paths until inbound links are migrated in
one deliberate cleanup change.

## Lifecycle Labels

- `active`: current requirements, roadmap, or unresolved work.
- `deferred`: valid work intentionally postponed; do not archive as complete.
- `archive-ready`: closed or superseded planning material with an identified
  replacement or completed acceptance record.
- `historical-reference`: evidence that should remain discoverable even after
  it moves out of the active documentation root.
- `reconcile`: an older TODO whose runtime outcome is not sufficiently proven
  to call complete or obsolete.

## Archive-Ready Now

| Bundle | Documents | Reason | Retained authority |
| --- | --- | --- | --- |
| Original CLI/modularization plan | `crimson_cli_and_modularization_todo.md` | The CLI recording path and most mechanical splits exist; its unchecked boxes no longer describe repository state. | `crimson_app_architecture_refactor_todo.md` and current source/tests |
| macOS Phase 0 | `crimson_macos_phase0_inventory.md` | Frozen pre-port baseline; all later implementation decisions are recorded in completed phase checkpoints. | `crimson_macos_port_codex_goal.md` and phase acceptance records |
| Phase 5L workspace parity | `crimson_macos_phase5l_workspace_inventory.md`, `crimson_macos_phase5l_workspace_parity_plan.md`, `crimson_macos_phase5l_portable_workspace_state.md`, `crimson_macos_phase5l2_workspace_composition.md`, `crimson_macos_phase5l3_stable_workflows.md`, `crimson_macos_phase5l4_visual_acceptance.md` | The macOS and Linux/NVIDIA workspace gate is closed. Windows runtime evidence was deliberately separated rather than left as Phase 5L work. | `crimson_windows_first_validation_guide.md`, `crimson_windows_trt10_cuda12.4_validation_record.md`, and `docs/reference/phase5l/` |
| Phase 5M polar extraction | `crimson_macos_phase5m_read_only_zarr_boundary_plan.md` and `crimson_macos_phase5m0_zarr_loader_inventory.md` through `crimson_macos_phase5m4_cross_platform_acceptance.md` | The pilot contract, both adapters, shared scene, and acceptance gate are complete. | Source contracts/tests and `docs/reference/phase5m/` |
| Phase 5N stimulus overlay parity | `crimson_macos_phase5n_remaining_stimulus_overlay_parity.md` | The remaining read-only stimulus overlays passed the maintained macOS and Linux/NVIDIA gate. | Source contracts/tests and `docs/reference/phase5n/` |

## Keep Active

These documents still describe current work and should remain in the active
documentation root:

- `crimson_macos_port_codex_goal.md`
- `crimson_app_architecture_refactor_todo.md`
- `crimson_macos_phase5o_bounded_data_access.md`
- `crimson_packaging_and_distribution_plan.md`
- the Windows validation and installation documents
- detection, keypoint, crop, subject-mask, and coordinate consumer contracts
- editing plans while Palette's storage/edit lifecycle remains unsettled

## Reconcile Before Archiving

These older documents are candidates for cleanup, but their remaining claims
need a focused code/runtime audit first:

- `crimson_seek_event_refactor_todo.md`: superseded in part by the shared seek
  transaction, but stimulus synchronization remains platform-owned.
- `crimson_decode_seek_artifact_todo.md`: several fixes landed, but the original
  artifact reproduction has not been formally closed.
- `crimson_live_playback_bidirectional_buffer_todo.md`,
  `crimson_contiguous_playback_window_design.md`, and
  `crimson_buffered_frame_resume_design.md`: recent pause/step behavior improved,
  but Linux and macOS still use different decoder-buffer implementations.
- `crimson_gui_smoke_testing_todo.md`: smoke tooling exists, but the document
  should be reconciled against the current macOS, Linux, and semantic harnesses.
- the main-camera late-conversion, zoom-aware, and playback-renderer plans:
  they contain useful low-end NVIDIA evidence and may still represent deferred
  performance work.

## Cleanup Procedure

1. Update inbound links to a current authority or the new archive path.
2. Move the closed bundle with `git mv`; do not copy it and leave two sources.
3. Preserve `docs/reference/` and `docs/diagnostics/` evidence paths unless an
   explicit evidence-retention decision says otherwise.
4. Run a repository-wide Markdown link check and `git diff --check`.
5. Keep one ledger entry here describing where the historical bundle moved.
