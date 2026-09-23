# Crimson Documentation Archive Ledger

Date: 2026-08-03

This directory holds documents removed from the active planning surface.
Archived does not mean disposable: implementation records, acceptance evidence,
and frozen contracts remain useful historical evidence. Inbound links point to
the archived location when the historical record is still relevant.

## Lifecycle Labels

- `active`: current requirements, roadmap, or unresolved work.
- `deferred`: valid work intentionally postponed; do not archive as complete.
- `archive-ready`: closed or superseded planning material with an identified
  replacement or completed acceptance record.
- `archived`: moved out of the active documentation root after inbound links
  were reconciled.
- `historical-reference`: evidence that should remain discoverable even after
  it moves out of the active documentation root.
- `reconcile`: an older TODO whose runtime outcome is not sufficiently proven
  to call complete or obsolete.

## Archived Bundles

| Bundle | Documents | Reason | Retained authority |
| --- | --- | --- | --- |
| Original CLI/modularization plan | `docs/archive/legacy-plans/crimson_cli_and_modularization_todo.md` | The CLI recording path and most mechanical splits exist; its unchecked boxes no longer describe repository state. | `docs/crimson_app_architecture_refactor_todo.md` and current source/tests |
| macOS Phase 0 | `docs/archive/macos-port/phase0/crimson_macos_phase0_inventory.md` | Frozen pre-port baseline; all later implementation decisions are recorded in completed phase checkpoints. | `docs/crimson_macos_port_codex_goal.md` and phase acceptance records |
| Phase 5L workspace parity | `docs/archive/macos-port/phase5l/` | The macOS and Linux/NVIDIA workspace gate is closed. Windows runtime evidence was deliberately separated rather than left as Phase 5L work. | `crimson_windows_first_validation_guide.md`, `crimson_windows_trt10_cuda12.4_validation_record.md`, and `docs/reference/phase5l/` |
| Phase 5M polar extraction | `docs/archive/macos-port/phase5m/` | The pilot contract, both adapters, shared scene, and acceptance gate are complete. | Source contracts/tests and `docs/reference/phase5m/` |
| Phase 5N stimulus overlay parity | `docs/archive/macos-port/phase5n/` | The remaining read-only stimulus overlays passed the maintained macOS and Linux/NVIDIA gate. | Source contracts/tests and `docs/reference/phase5n/` |
| Playback seek/buffer investigations | `docs/archive/playback/` | Shared transport, seek intent, and frame selection landed; the old plans mixed completed work with missing active/staging-window policy and an unclosed NVIDIA runtime gate. | `docs/crimson_playback_remaining_work.md`, `docs/crimson_shared_seek_transaction_checkpoint_2026-07-31.md`, and current source/tests |
| Main-camera render experiments | `docs/archive/playback/crimson_playback_preview_scale_plan.md`, `crimson_main_camera_late_conversion_plan.md`, and `crimson_main_camera_zoom_aware_render_plan.md` | Preview and late-conversion experiments are complete; zoom telemetry superseded ROI-only work. | `docs/crimson_main_camera_playback_renderer_plan.md` |
| Original GUI smoke TODO | `docs/archive/testing/crimson_gui_smoke_testing_todo.md` | Maintained Linux and macOS GUI harnesses plus semantic/headless coverage now exist. | `AGENTS.md`, `scripts/README.md`, `scripts/gui_smoke_playback.sh`, and the macOS smoke scripts |

## Keep Active

These documents still describe current work and should remain in the active
documentation root:

- `crimson_macos_port_codex_goal.md`
- `crimson_app_architecture_refactor_todo.md`
- `crimson_playback_remaining_work.md`
- `crimson_macos_phase5o_bounded_data_access.md`
- `crimson_packaging_and_distribution_plan.md`
- the Windows validation and installation documents
- detection, keypoint, crop, subject-mask, and coordinate consumer contracts
- editing plans while Palette's storage/edit lifecycle remains unsettled

## Active And Deferred TODO Register

The 2026-08-03 reconciliation leaves these actionable planning surfaces:

- `docs/crimson_app_architecture_refactor_todo.md`: active shared-module and
  composition-root work.
- `docs/crimson_playback_remaining_work.md`: active playback-window, stimulus
  settlement, and NVIDIA artifact-acceptance work.
- `docs/crimson_macos_phase5o_bounded_data_access.md`: active bounded-access and
  scheduling roadmap.
- `docs/crimson_bbox_editing_todo.md`: deferred until Palette's refined edit
  lifecycle is stable; legacy manual-subgroup instructions are not authority.
- `docs/crimson_eye_angle_exploratory_plotting_checklist.md`: deferred feature
  work requiring current coordinate/storage-contract reconciliation.
- `docs/crimson_frame_pacing_performance_todo.md`: deferred measurement-led
  frame-pacing work.
- `docs/crimson_main_camera_playback_renderer_plan.md`: deferred weak-GPU
  performance experiment.

Windows runtime checklists remain open validation records rather than stale
implementation TODOs.

## Cleanup Procedure

1. Update inbound links to a current authority or the new archive path.
2. Move the closed bundle with `git mv`; do not copy it and leave two sources.
3. Preserve `docs/reference/` and `docs/diagnostics/` evidence paths unless an
   explicit evidence-retention decision says otherwise.
4. Run a repository-wide Markdown link check and `git diff --check`.
5. Keep one ledger entry here describing where the historical bundle moved.
