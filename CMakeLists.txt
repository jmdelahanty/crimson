cmake_minimum_required(VERSION 3.18)
project(redgui LANGUAGES CXX)

include(GNUInstallDirs)

if(APPLE)
    set(CRIMSON_BUILD_NVIDIA_APP_DEFAULT OFF)
    set(CRIMSON_BUILD_MACOS_APP_DEFAULT ON)
else()
    set(CRIMSON_BUILD_NVIDIA_APP_DEFAULT ON)
    set(CRIMSON_BUILD_MACOS_APP_DEFAULT OFF)
endif()

option(CRIMSON_BUILD_NVIDIA_APP
    "Build the maintained CUDA/NVDEC/OpenGL/TensorRT redgui application"
    ${CRIMSON_BUILD_NVIDIA_APP_DEFAULT})
option(CRIMSON_BUILD_MACOS_APP
    "Build the native Apple Silicon Metal application"
    ${CRIMSON_BUILD_MACOS_APP_DEFAULT})

if(CRIMSON_BUILD_NVIDIA_APP AND APPLE)
    message(FATAL_ERROR
        "CRIMSON_BUILD_NVIDIA_APP is not supported on macOS. "
        "Use the macOS preset or set CRIMSON_BUILD_NVIDIA_APP=OFF.")
endif()
if(CRIMSON_BUILD_MACOS_APP AND NOT APPLE)
    message(FATAL_ERROR
        "CRIMSON_BUILD_MACOS_APP requires native macOS.")
endif()
if(NOT CRIMSON_BUILD_NVIDIA_APP AND NOT CRIMSON_BUILD_MACOS_APP)
    message(WARNING "No Crimson application backend is enabled.")
endif()

# ----------------------- Build toggles -----------------------
option(ENABLE_NVML "Link against NVIDIA Management Library (NVML)" ON)
option(CRIMSON_INSTALL_DEV_TOOLS "Install diagnostic and developer helper executables" OFF)
option(CRIMSON_ENABLE_SFM "Enable OpenCV SFM-based triangulation workflows" ON)
option(CRIMSON_ENABLE_PARQUET "Enable Arrow/Parquet support for Palette clipped frame maps" OFF)
option(CRIMSON_ENABLE_NVENC "Require/link NVIDIA NVENC encode API for future annotated video export support" OFF)
option(ENABLE_TENSORSTORE_ZARR3_CHECK
    "Build and run a small runtime check to verify zarr3 driver support" ON)

# Set output directory for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)

# Set C++ standard and options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(Python3 QUIET COMPONENTS Interpreter)
include(cmake/CrimsonTensorStore.cmake)

add_library(crimson_frame_contracts STATIC
    src/clipped_media_handoff.cpp
    src/frame_types.cpp
    src/frame_selection.cpp
    src/frame_slot.cpp
    src/playback_clock.cpp
    src/playback_diagnostics.cpp
    src/playback_seek.cpp
    src/playback_presentation_lifecycle.cpp
)
target_include_directories(crimson_frame_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(crimson_frame_contracts PUBLIC
    Threads::Threads
    nlohmann_json::nlohmann_json
)
target_compile_features(crimson_frame_contracts PUBLIC cxx_std_17)

add_library(crimson_runtime_contracts STATIC
    src/session_lifecycle.cpp
    src/loading_progress.cpp
    src/frame_presentation.cpp
    src/diagnostic_report.cpp
    src/process_memory.cpp
    src/endurance_workload.cpp
    src/session_readiness.cpp
    src/session_open_transaction.cpp
    src/recording_open_workflow.cpp
    src/session_loading_presentation.cpp
)
target_include_directories(crimson_runtime_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(crimson_runtime_contracts PUBLIC Threads::Threads)
target_compile_features(crimson_runtime_contracts PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(crimson_runtime_contracts PUBLIC Psapi)
endif()

add_library(crimson_recording_contracts STATIC
    src/recording_clip_index.cpp
    src/recording_path_resolution.cpp
)
target_include_directories(crimson_recording_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_recording_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_recording_contracts PUBLIC
    nlohmann_json::nlohmann_json
)

add_executable(recording_path_resolution_tests
    tools/recording_path_resolution_tests.cpp
)
target_link_libraries(recording_path_resolution_tests PRIVATE
    crimson_recording_contracts
)
target_compile_features(recording_path_resolution_tests PRIVATE cxx_std_17)
set_target_properties(recording_path_resolution_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME recording_path_resolution_tests
    COMMAND $<TARGET_FILE:recording_path_resolution_tests>
)
set_tests_properties(recording_path_resolution_tests PROPERTIES
    LABELS "headless;media;portable;runtime"
)

add_executable(recording_clip_index_tests
    tools/recording_clip_index_tests.cpp
)
target_link_libraries(recording_clip_index_tests PRIVATE
    crimson_recording_contracts
)
target_compile_features(recording_clip_index_tests PRIVATE cxx_std_17)
set_target_properties(recording_clip_index_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME recording_clip_index_tests
    COMMAND $<TARGET_FILE:recording_clip_index_tests>
)
set_tests_properties(recording_clip_index_tests PROPERTIES
    LABELS "clips;headless;media;portable;runtime"
)

add_library(crimson_data_access_contracts STATIC
    src/data_access.cpp
    src/data_access_cache.cpp
    src/data_access_diagnostics.cpp
    src/data_access_scheduler.cpp
)
target_include_directories(crimson_data_access_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(crimson_data_access_contracts PUBLIC Threads::Threads)
target_compile_features(crimson_data_access_contracts PUBLIC cxx_std_17)

add_library(crimson_coordinate_contracts STATIC
    src/coordinate_contract.cpp
)
target_include_directories(crimson_coordinate_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_coordinate_contracts PUBLIC cxx_std_17)

add_library(crimson_repository_contracts STATIC
    src/analysis_series_timeline.cpp
    src/analysis_series_timeline_buffer.cpp
    src/canonical_detection_buffer.cpp
    src/detection_quality_timeline.cpp
    src/detection_quality_timeline_buffer.cpp
    src/keypoint_quality_timeline.cpp
    src/keypoint_quality_timeline_buffer.cpp
    src/chaser_distance_polar.cpp
    src/chaser_distance_polar_buffer.cpp
    src/chaser_distance_polar_scene.cpp
    src/eye_angle_timeline.cpp
    src/eye_angle_timeline_buffer.cpp
    src/eye_geometry_overlay_buffer.cpp
    src/keypoint_overlay_buffer.cpp
    src/stimulus_camera_overlay_scene.cpp
    src/stimulus_context_timeline.cpp
    src/swim_bout_timeline.cpp
    src/swim_bout_timeline_buffer.cpp
    src/subject_mask_overlay_buffer.cpp
    src/subject_shape_overlay_buffer.cpp
    src/zarr/keypoint_overlay_repository.cpp
    src/zarr/eye_geometry_overlay_repository.cpp
    src/zarr/stimulus_repository.cpp
    src/zarr/subject_mask_overlay_repository.cpp
    src/zarr/subject_shape_overlay_repository.cpp
)
target_include_directories(crimson_repository_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_repository_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_repository_contracts PUBLIC
    crimson_data_access_contracts
    Threads::Threads
)

add_library(crimson_crop_contracts STATIC
    src/crop_source_contract.cpp
    src/crop_presentation_coordinator.cpp
    src/zarr/analysis_crop_geometry_repository.cpp
    src/zarr/acquisition_crop_repository.cpp
)
target_include_directories(crimson_crop_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_crop_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_crop_contracts PUBLIC
    crimson_coordinate_contracts
)

add_library(crimson_overlay_contracts STATIC
    src/overlay_scene_contract.cpp
    src/read_only_overlay_controls.cpp
    src/read_only_overlay_frame_coordinator.cpp
    src/read_only_overlay_scene.cpp
    src/subject_mask_presentation_coordinator.cpp
    src/zarr/canonical_detection_overlay_scene_adapter.cpp
    src/zarr/keypoint_overlay_scene_adapter.cpp
    src/zarr/eye_geometry_overlay_scene_adapter.cpp
    src/zarr/subject_mask_overlay_scene_adapter.cpp
    src/zarr/subject_shape_overlay_scene_adapter.cpp
)
target_include_directories(crimson_overlay_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_overlay_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_overlay_contracts PUBLIC
    crimson_coordinate_contracts
    crimson_crop_contracts
    crimson_repository_contracts
)

add_library(crimson_stimulus_playback_contracts STATIC
    src/stimulus_playback_coordinator.cpp
    src/stimulus_presentation_coordinator.cpp
)
target_include_directories(crimson_stimulus_playback_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_stimulus_playback_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_stimulus_playback_contracts PUBLIC
    crimson_repository_contracts
)

add_library(crimson_workspace_contracts STATIC
    src/app/frame_inspect_controller.cpp
    src/workspace_state.cpp
)
target_include_directories(crimson_workspace_contracts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(crimson_workspace_contracts PUBLIC cxx_std_17)
target_link_libraries(crimson_workspace_contracts PUBLIC
    crimson_crop_contracts
    crimson_overlay_contracts
)

function(crimson_add_tensorstore_repositories tensorstore_target)
    if(TARGET crimson_tensorstore_repositories)
        return()
    endif()

    add_library(crimson_tensorstore_repositories OBJECT
        src/zarr/tensorstore_archive_context.cpp
        src/zarr/tensorstore_affiliated_video_repository.cpp
        src/zarr/tensorstore_analysis_series_timeline_repository.cpp
        src/zarr/tensorstore_chaser_distance_polar_repository.cpp
        src/zarr/detection_repository_selection.cpp
        src/zarr/canonical_json.cpp
        src/zarr/canonical_detection_contract.cpp
        src/zarr/coordinate_catalog_contract.cpp
        src/zarr/crop_geometry_contract.cpp
        src/zarr/keypoint_v2_contract.cpp
        src/zarr/tensorstore_keypoint_v2_repository.cpp
        src/zarr/refined_detection_contract.cpp
        src/zarr/subject_mask_bundle_v2_contract.cpp
        src/zarr/subject_mask_sampled_contour_v1_contract.cpp
        src/zarr/subject_mask_v1_contract.cpp
        src/zarr/tensorstore_canonical_detection_repository.cpp
        src/zarr/tensorstore_detection_quality_timeline_repository.cpp
        src/zarr/tensorstore_refined_detection_repository.cpp
        src/zarr/tensorstore_eye_angle_timeline_repository.cpp
        src/zarr/tensorstore_eye_geometry_overlay_repository.cpp
        src/zarr/tensorstore_analysis_crop_geometry_repository.cpp
        src/zarr/tensorstore_acquisition_crop_repository.cpp
        src/zarr/tensorstore_keypoint_overlay_repository.cpp
        src/zarr/tensorstore_stimulus_context_timeline_repository.cpp
        src/zarr/tensorstore_swim_bout_timeline_repository.cpp
        src/zarr/tensorstore_stimulus_repository.cpp
        src/zarr/tensorstore_subject_mask_overlay_repository.cpp
        src/zarr/tensorstore_subject_shape_overlay_repository.cpp
        src/zarr/zarr_metadata_equivalence.cpp
    )
    target_include_directories(crimson_tensorstore_repositories PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(crimson_tensorstore_repositories PUBLIC cxx_std_17)
    set(_crimson_tensorstore_digest_target "")
    if(TARGET tensorstore_internal_digest_sha256)
        set(_crimson_tensorstore_digest_target
            tensorstore_internal_digest_sha256)
    endif()
    target_link_libraries(crimson_tensorstore_repositories
        PUBLIC
            crimson_crop_contracts
            crimson_recording_contracts
            crimson_repository_contracts
        PRIVATE ${tensorstore_target} ${_crimson_tensorstore_digest_target}
                Threads::Threads
    )

    add_executable(archive_context_tests
        tools/archive_context_tests.cpp
    )
    target_compile_features(archive_context_tests PRIVATE cxx_std_17)
    target_link_libraries(archive_context_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(archive_context_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME archive_context_tests
        COMMAND $<TARGET_FILE:archive_context_tests>)

    add_executable(canonical_detection_repository_tests
        tools/canonical_detection_repository_tests.cpp
    )
    target_compile_features(canonical_detection_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(canonical_detection_repository_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(canonical_detection_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME canonical_detection_repository_tests
        COMMAND $<TARGET_FILE:canonical_detection_repository_tests>)
    set_tests_properties(canonical_detection_repository_tests PROPERTIES
        LABELS "headless;detection;overlay;repository;tensorstore;zarr"
        TIMEOUT 30
    )

    add_executable(detection_quality_timeline_gate
        tools/detection_quality_timeline_gate.cpp
    )
    target_compile_features(detection_quality_timeline_gate PRIVATE cxx_std_17)
    target_link_libraries(detection_quality_timeline_gate PRIVATE
        crimson_tensorstore_repositories
        crimson_repository_contracts
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(detection_quality_timeline_gate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(detection_quality_timeline_tests
        tools/detection_quality_timeline_tests.cpp
    )
    target_compile_features(detection_quality_timeline_tests PRIVATE cxx_std_17)
    target_link_libraries(detection_quality_timeline_tests PRIVATE
        crimson_repository_contracts
        Threads::Threads
    )
    set_target_properties(detection_quality_timeline_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME detection_quality_timeline_tests
        COMMAND $<TARGET_FILE:detection_quality_timeline_tests>)
    set_tests_properties(detection_quality_timeline_tests PROPERTIES
        LABELS "headless;detection;timeline;repository"
        TIMEOUT 30
    )

    add_executable(keypoint_quality_timeline_tests
        tools/keypoint_quality_timeline_tests.cpp
    )
    target_compile_features(keypoint_quality_timeline_tests PRIVATE cxx_std_17)
    target_link_libraries(keypoint_quality_timeline_tests PRIVATE
        crimson_repository_contracts
        Threads::Threads
    )
    set_target_properties(keypoint_quality_timeline_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME keypoint_quality_timeline_tests
        COMMAND $<TARGET_FILE:keypoint_quality_timeline_tests>)
    set_tests_properties(keypoint_quality_timeline_tests PROPERTIES
        LABELS "headless;keypoints;quality;timeline;repository"
        TIMEOUT 30
    )

    add_executable(quality_timeline_session_tests
        tools/quality_timeline_session_tests.cpp
        src/gui/quality_timeline_session.cpp
    )
    target_compile_features(quality_timeline_session_tests PRIVATE cxx_std_17)
    target_link_libraries(quality_timeline_session_tests PRIVATE
        crimson_tensorstore_repositories
        crimson_repository_contracts
        ${tensorstore_target}
        Threads::Threads
    )
    set_target_properties(quality_timeline_session_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME quality_timeline_session_tests
        COMMAND $<TARGET_FILE:quality_timeline_session_tests>)
    set_tests_properties(quality_timeline_session_tests PROPERTIES
        LABELS "headless;portable;quality;session;timeline"
        TIMEOUT 30
    )

    add_executable(coordinate_catalog_contract_tests
        tools/coordinate_catalog_contract_tests.cpp
    )
    target_compile_features(coordinate_catalog_contract_tests PRIVATE cxx_std_17)
    target_link_libraries(coordinate_catalog_contract_tests PRIVATE
        crimson_coordinate_contracts
        crimson_tensorstore_repositories
        nlohmann_json::nlohmann_json
    )
    target_compile_definitions(coordinate_catalog_contract_tests PRIVATE
        CRIMSON_COORDINATE_FIXTURE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/tools/fixtures/palette_coordinate_catalogs_154d7888.json\"
    )
    set_target_properties(coordinate_catalog_contract_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME coordinate_catalog_contract_tests
        COMMAND $<TARGET_FILE:coordinate_catalog_contract_tests>)
    set_tests_properties(coordinate_catalog_contract_tests PROPERTIES
        LABELS "coordinates;headless;portable;tensorstore;zarr"
        TIMEOUT 30
    )

    add_executable(refined_detection_shadow_gate
        tools/refined_detection_shadow_gate.cpp
    )
    target_compile_features(refined_detection_shadow_gate PRIVATE cxx_std_17)
    target_link_libraries(refined_detection_shadow_gate PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CRIMSON_REFINED_GATE_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CRIMSON_REFINED_GATE_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT CRIMSON_REFINED_GATE_GIT_COMMIT)
        set(CRIMSON_REFINED_GATE_GIT_COMMIT "unknown")
    endif()
    if(CRIMSON_REFINED_GATE_GIT_STATUS)
        set(CRIMSON_REFINED_GATE_WORKTREE_DIRTY 1)
    else()
        set(CRIMSON_REFINED_GATE_WORKTREE_DIRTY 0)
    endif()
    target_compile_definitions(refined_detection_shadow_gate PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(refined_detection_shadow_gate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(coordinate_catalog_canary_gate
        tools/coordinate_catalog_canary_gate.cpp
    )
    target_compile_features(coordinate_catalog_canary_gate PRIVATE cxx_std_17)
    target_link_libraries(coordinate_catalog_canary_gate PRIVATE
        crimson_tensorstore_repositories
        nlohmann_json::nlohmann_json
    )
    target_compile_definitions(coordinate_catalog_canary_gate PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(coordinate_catalog_canary_gate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(keypoint_v2_canary_gate
        tools/keypoint_v2_canary_gate.cpp
    )
    target_compile_features(keypoint_v2_canary_gate PRIVATE cxx_std_17)
    target_link_libraries(keypoint_v2_canary_gate PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
    )
    set_target_properties(keypoint_v2_canary_gate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(keypoint_quality_timeline_gate
        tools/keypoint_quality_timeline_gate.cpp
    )
    target_compile_features(keypoint_quality_timeline_gate PRIVATE cxx_std_17)
    target_link_libraries(keypoint_quality_timeline_gate PRIVATE
        crimson_tensorstore_repositories
        crimson_repository_contracts
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(keypoint_quality_timeline_gate PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(keypoint_v2_contract_tests
        tools/keypoint_v2_contract_tests.cpp
    )
    target_compile_features(keypoint_v2_contract_tests PRIVATE cxx_std_17)
    target_link_libraries(keypoint_v2_contract_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        nlohmann_json::nlohmann_json
    )
    set_target_properties(keypoint_v2_contract_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME keypoint_v2_contract_tests
        COMMAND $<TARGET_FILE:keypoint_v2_contract_tests>)
    set_tests_properties(keypoint_v2_contract_tests PROPERTIES
        LABELS "headless;keypoint;overlay;portable;tensorstore;zarr"
        TIMEOUT 30
    )

    add_executable(refined_detection_physical_profile_benchmark
        tools/refined_detection_physical_profile_benchmark.cpp
    )
    target_compile_features(refined_detection_physical_profile_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(refined_detection_physical_profile_benchmark PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    target_compile_definitions(
        refined_detection_physical_profile_benchmark PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(refined_detection_physical_profile_benchmark
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(archive_context_tests PROPERTIES
        LABELS "cache;headless;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(tensorstore_array_access_benchmark
        tools/tensorstore_array_access_benchmark.cpp
    )
    target_compile_features(tensorstore_array_access_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(tensorstore_array_access_benchmark PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(tensorstore_array_access_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(canonical_detection_storage_benchmark
        tools/canonical_detection_storage_benchmark.cpp
    )
    target_compile_features(canonical_detection_storage_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(canonical_detection_storage_benchmark PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(canonical_detection_storage_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(crop_geometry_v2_read_benchmark
        tools/crop_geometry_v2_read_benchmark.cpp
    )
    target_compile_features(crop_geometry_v2_read_benchmark PRIVATE cxx_std_17)
    target_link_libraries(crop_geometry_v2_read_benchmark PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    target_compile_definitions(crop_geometry_v2_read_benchmark PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(crop_geometry_v2_read_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME crop_geometry_v2_read_benchmark_self_test
        COMMAND $<TARGET_FILE:crop_geometry_v2_read_benchmark> --self-test)
    set_tests_properties(crop_geometry_v2_read_benchmark_self_test PROPERTIES
        LABELS "portable;headless;tensorstore;zarr;crop;benchmark"
    )

    add_executable(keypoint_v2_long_duration_benchmark
        tools/keypoint_v2_long_duration_benchmark.cpp
    )
    target_compile_features(keypoint_v2_long_duration_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(keypoint_v2_long_duration_benchmark PRIVATE
        crimson_repository_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    target_compile_definitions(keypoint_v2_long_duration_benchmark PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(keypoint_v2_long_duration_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME keypoint_v2_long_duration_benchmark_self_test
        COMMAND $<TARGET_FILE:keypoint_v2_long_duration_benchmark> --self-test)
    set_tests_properties(keypoint_v2_long_duration_benchmark_self_test
        PROPERTIES
        LABELS "portable;headless;tensorstore;zarr;keypoints;benchmark"
    )

    add_executable(subject_mask_v1_long_duration_benchmark
        tools/subject_mask_v1_long_duration_benchmark.cpp
    )
    target_compile_features(subject_mask_v1_long_duration_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_v1_long_duration_benchmark PRIVATE
        crimson_repository_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    target_compile_definitions(subject_mask_v1_long_duration_benchmark PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_REFINED_GATE_GIT_COMMIT}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_REFINED_GATE_WORKTREE_DIRTY}
    )
    set_target_properties(subject_mask_v1_long_duration_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME subject_mask_v1_long_duration_benchmark_self_test
        COMMAND $<TARGET_FILE:subject_mask_v1_long_duration_benchmark>
            --self-test)
    set_tests_properties(subject_mask_v1_long_duration_benchmark_self_test
        PROPERTIES
        LABELS "portable;headless;tensorstore;zarr;masks;benchmark"
    )

    add_executable(subject_mask_sampled_contour_v1_contract_gate
        tools/subject_mask_sampled_contour_v1_contract_gate.cpp
    )
    target_compile_features(subject_mask_sampled_contour_v1_contract_gate
        PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_sampled_contour_v1_contract_gate PRIVATE
        crimson_repository_contracts
        crimson_tensorstore_repositories
        nlohmann_json::nlohmann_json
    )
    set_target_properties(subject_mask_sampled_contour_v1_contract_gate
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(subject_mask_bundle_v2_probe
        tools/subject_mask_bundle_v2_probe.cpp
    )
    target_compile_features(subject_mask_bundle_v2_probe PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_bundle_v2_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
    )
    set_target_properties(subject_mask_bundle_v2_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(subject_mask_crop_join_probe
        tools/subject_mask_crop_join_probe.cpp
    )
    target_compile_features(subject_mask_crop_join_probe PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_crop_join_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
    )
    set_target_properties(subject_mask_crop_join_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    if(Python3_Interpreter_FOUND)
        add_test(NAME keypoint_v2_experiment_runner_self_test
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_keypoint_v2_long_duration_benchmark.py
                --self-test)
        set_tests_properties(keypoint_v2_experiment_runner_self_test
            PROPERTIES
            LABELS "portable;headless;keypoints;benchmark"
        )
    endif()

    add_executable(canonical_detection_residency_benchmark
        tools/canonical_detection_residency_benchmark.cpp
    )
    target_compile_features(canonical_detection_residency_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(canonical_detection_residency_benchmark PRIVATE
        crimson_repository_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(canonical_detection_residency_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(stimulus_repository_probe
        tools/stimulus_repository_probe.cpp
    )
    target_compile_features(stimulus_repository_probe PRIVATE cxx_std_17)
    target_link_libraries(stimulus_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(stimulus_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(stimulus_context_timeline_repository_probe
        tools/stimulus_context_timeline_repository_probe.cpp
    )
    target_compile_features(stimulus_context_timeline_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(stimulus_context_timeline_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(stimulus_context_timeline_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(swim_bout_timeline_repository_probe
        tools/swim_bout_timeline_repository_probe.cpp
    )
    target_compile_features(swim_bout_timeline_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(swim_bout_timeline_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(swim_bout_timeline_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(stimulus_context_timeline_repository_tests
        tools/stimulus_context_timeline_repository_tests.cpp
    )
    target_compile_features(stimulus_context_timeline_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(stimulus_context_timeline_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(stimulus_context_timeline_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME stimulus_context_timeline_repository_tests
        COMMAND $<TARGET_FILE:stimulus_context_timeline_repository_tests>)
    set_tests_properties(stimulus_context_timeline_repository_tests PROPERTIES
        LABELS "headless;portable;repository;stimulus;tensorstore;timeline;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(swim_bout_timeline_repository_tests
        tools/swim_bout_timeline_repository_tests.cpp
    )
    target_compile_features(swim_bout_timeline_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(swim_bout_timeline_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(swim_bout_timeline_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME swim_bout_timeline_repository_tests
        COMMAND $<TARGET_FILE:swim_bout_timeline_repository_tests>)
    set_tests_properties(swim_bout_timeline_repository_tests PROPERTIES
        LABELS "headless;portable;repository;swim_bout;tensorstore;timeline;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(chaser_distance_polar_tensorstore_repository_tests
        tools/chaser_distance_polar_tensorstore_repository_tests.cpp
    )
    target_compile_features(
        chaser_distance_polar_tensorstore_repository_tests PRIVATE cxx_std_17)
    target_link_libraries(
        chaser_distance_polar_tensorstore_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(
        chaser_distance_polar_tensorstore_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME chaser_distance_polar_tensorstore_repository_tests
        COMMAND $<TARGET_FILE:chaser_distance_polar_tensorstore_repository_tests>)
    set_tests_properties(
        chaser_distance_polar_tensorstore_repository_tests PROPERTIES
        LABELS "headless;polar;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(acquisition_crop_repository_tests
        tools/acquisition_crop_repository_tests.cpp
    )
    target_compile_features(acquisition_crop_repository_tests PRIVATE cxx_std_17)
    target_link_libraries(acquisition_crop_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(acquisition_crop_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME acquisition_crop_repository_tests
        COMMAND $<TARGET_FILE:acquisition_crop_repository_tests>)
    set_tests_properties(acquisition_crop_repository_tests PROPERTIES
        LABELS "crop;headless;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(affiliated_video_repository_tests
        tools/affiliated_video_repository_tests.cpp
    )
    target_compile_features(affiliated_video_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(affiliated_video_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(affiliated_video_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME affiliated_video_repository_tests
        COMMAND $<TARGET_FILE:affiliated_video_repository_tests>)
    set_tests_properties(affiliated_video_repository_tests PROPERTIES
        LABELS "headless;media;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(affiliated_video_repository_probe
        tools/affiliated_video_repository_probe.cpp
    )
    target_compile_features(affiliated_video_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(affiliated_video_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(affiliated_video_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(analysis_crop_geometry_repository_tests
        tools/analysis_crop_geometry_repository_tests.cpp
    )
    target_compile_features(analysis_crop_geometry_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(analysis_crop_geometry_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(analysis_crop_geometry_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME analysis_crop_geometry_repository_tests
        COMMAND $<TARGET_FILE:analysis_crop_geometry_repository_tests>)
    set_tests_properties(analysis_crop_geometry_repository_tests PROPERTIES
        LABELS "crop;geometry;headless;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(keypoint_overlay_repository_tests
        tools/keypoint_overlay_repository_tests.cpp
    )
    target_compile_features(keypoint_overlay_repository_tests PRIVATE cxx_std_17)
    target_link_libraries(keypoint_overlay_repository_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(keypoint_overlay_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME keypoint_overlay_repository_tests
        COMMAND $<TARGET_FILE:keypoint_overlay_repository_tests>)
    set_tests_properties(keypoint_overlay_repository_tests PROPERTIES
        LABELS "headless;keypoints;overlay;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(subject_mask_overlay_repository_tests
        tools/subject_mask_overlay_repository_tests.cpp
    )
    target_compile_features(subject_mask_overlay_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_overlay_repository_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(subject_mask_overlay_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME subject_mask_overlay_repository_tests
        COMMAND $<TARGET_FILE:subject_mask_overlay_repository_tests>)
    set_tests_properties(subject_mask_overlay_repository_tests PROPERTIES
        LABELS "headless;masks;overlay;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(subject_mask_v1_contract_tests
        tools/subject_mask_v1_contract_tests.cpp
    )
    target_compile_features(subject_mask_v1_contract_tests PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_v1_contract_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(subject_mask_v1_contract_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME subject_mask_v1_contract_tests
        COMMAND $<TARGET_FILE:subject_mask_v1_contract_tests>)
    set_tests_properties(subject_mask_v1_contract_tests PROPERTIES
        LABELS "contract;headless;masks;portable;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(subject_shape_overlay_repository_tests
        tools/subject_shape_overlay_repository_tests.cpp
    )
    target_compile_features(subject_shape_overlay_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(subject_shape_overlay_repository_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(subject_shape_overlay_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME subject_shape_overlay_repository_tests
        COMMAND $<TARGET_FILE:subject_shape_overlay_repository_tests>)
    set_tests_properties(subject_shape_overlay_repository_tests PROPERTIES
        LABELS "headless;overlay;portable;repository;subject-shape;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(eye_geometry_overlay_repository_probe
        tools/eye_geometry_overlay_repository_probe.cpp
    )
    target_compile_features(eye_geometry_overlay_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(eye_geometry_overlay_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(eye_geometry_overlay_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(eye_angle_timeline_repository_probe
        tools/eye_angle_timeline_repository_probe.cpp
    )
    target_compile_features(eye_angle_timeline_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(eye_angle_timeline_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(eye_angle_timeline_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(eye_angle_timeline_repository_tests
        tools/eye_angle_timeline_repository_tests.cpp
    )
    target_compile_features(eye_angle_timeline_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(eye_angle_timeline_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(eye_angle_timeline_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME eye_angle_timeline_repository_tests
        COMMAND $<TARGET_FILE:eye_angle_timeline_repository_tests>)
    set_tests_properties(eye_angle_timeline_repository_tests PROPERTIES
        LABELS "headless;portable;repository;tensorstore;timeline;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(analysis_series_timeline_repository_tests
        tools/analysis_series_timeline_repository_tests.cpp
    )
    target_compile_features(analysis_series_timeline_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(analysis_series_timeline_repository_tests PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(analysis_series_timeline_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME analysis_series_timeline_repository_tests
        COMMAND $<TARGET_FILE:analysis_series_timeline_repository_tests>)
    set_tests_properties(analysis_series_timeline_repository_tests PROPERTIES
        LABELS "headless;portable;repository;tensorstore;timeline;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(analysis_series_timeline_repository_probe
        tools/analysis_series_timeline_repository_probe.cpp
    )
    target_compile_features(analysis_series_timeline_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(analysis_series_timeline_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(analysis_series_timeline_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(eye_geometry_overlay_repository_tests
        tools/eye_geometry_overlay_repository_tests.cpp
    )
    target_compile_features(eye_geometry_overlay_repository_tests
        PRIVATE cxx_std_17)
    target_link_libraries(eye_geometry_overlay_repository_tests PRIVATE
        crimson_overlay_contracts
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(eye_geometry_overlay_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME eye_geometry_overlay_repository_tests
        COMMAND $<TARGET_FILE:eye_geometry_overlay_repository_tests>)
    set_tests_properties(eye_geometry_overlay_repository_tests PROPERTIES
        LABELS "eye-geometry;headless;overlay;portable;repository;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(acquisition_crop_repository_probe
        tools/acquisition_crop_repository_probe.cpp
    )
    target_compile_features(acquisition_crop_repository_probe PRIVATE cxx_std_17)
    target_link_libraries(acquisition_crop_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(acquisition_crop_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(keypoint_overlay_repository_probe
        tools/keypoint_overlay_repository_probe.cpp
    )
    target_compile_features(keypoint_overlay_repository_probe PRIVATE cxx_std_17)
    target_link_libraries(keypoint_overlay_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(keypoint_overlay_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(subject_mask_overlay_repository_probe
        tools/subject_mask_overlay_repository_probe.cpp
    )
    target_compile_features(subject_mask_overlay_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(subject_mask_overlay_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(subject_mask_overlay_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(subject_shape_overlay_repository_probe
        tools/subject_shape_overlay_repository_probe.cpp
    )
    target_compile_features(subject_shape_overlay_repository_probe
        PRIVATE cxx_std_17)
    target_link_libraries(subject_shape_overlay_repository_probe PRIVATE
        crimson_tensorstore_repositories
        ${tensorstore_target}
    )
    set_target_properties(subject_shape_overlay_repository_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endfunction()

add_executable(frame_slot_tests tools/frame_slot_tests.cpp)
target_link_libraries(frame_slot_tests PRIVATE crimson_frame_contracts)
target_compile_features(frame_slot_tests PRIVATE cxx_std_17)
set_target_properties(frame_slot_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
enable_testing()

if(UNIX AND NOT APPLE)
    add_test(NAME crimson_linux_launcher_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/crimson_linux_launcher_tests.sh)
    set_tests_properties(crimson_linux_launcher_tests PROPERTIES
        LABELS "headless;linux;packaging;portable"
        TIMEOUT 30
    )
endif()

add_custom_target(crimson_zarr_loader_dependency_check ALL
    COMMAND ${CMAKE_COMMAND}
        -DCRIMSON_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckZarrLoaderDependencies.cmake
    COMMENT "Checking ZarrDetectionLoader dependency baseline"
    VERBATIM
)
add_test(NAME zarr_loader_dependency_check
    COMMAND ${CMAKE_COMMAND}
        -DCRIMSON_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckZarrLoaderDependencies.cmake
)
set_tests_properties(zarr_loader_dependency_check PROPERTIES
    LABELS "architecture;headless;portable;zarr"
)

add_test(NAME frame_slot_tests COMMAND $<TARGET_FILE:frame_slot_tests>)
set_tests_properties(frame_slot_tests PROPERTIES LABELS "frame;portable")

add_executable(playback_transport_tests
    tools/playback_transport_tests.cpp
)
target_link_libraries(playback_transport_tests PRIVATE crimson_frame_contracts)
target_compile_features(playback_transport_tests PRIVATE cxx_std_17)
set_target_properties(playback_transport_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME playback_transport_tests
    COMMAND $<TARGET_FILE:playback_transport_tests>
)
set_tests_properties(playback_transport_tests PROPERTIES
    LABELS "headless;playback;portable;transport"
)

add_executable(clipped_media_handoff_tests
    tools/clipped_media_handoff_tests.cpp
)
target_link_libraries(clipped_media_handoff_tests PRIVATE crimson_frame_contracts)
target_compile_features(clipped_media_handoff_tests PRIVATE cxx_std_17)
set_target_properties(clipped_media_handoff_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME clipped_media_handoff_tests
    COMMAND $<TARGET_FILE:clipped_media_handoff_tests>
)
set_tests_properties(clipped_media_handoff_tests PROPERTIES
    LABELS "headless;playback;portable;clipped"
)

add_executable(playback_diagnostics_tests
    tools/playback_diagnostics_tests.cpp
)
target_link_libraries(playback_diagnostics_tests PRIVATE crimson_frame_contracts)
target_compile_features(playback_diagnostics_tests PRIVATE cxx_std_17)
set_target_properties(playback_diagnostics_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME playback_diagnostics_tests
    COMMAND $<TARGET_FILE:playback_diagnostics_tests>
)
set_tests_properties(playback_diagnostics_tests PROPERTIES
    LABELS "headless;playback;portable;diagnostics"
)

foreach(runtime_test
        session_lifecycle_tests
        loading_progress_tests
        frame_presentation_tests
        diagnostic_report_tests
        process_memory_tests
        endurance_workload_tests
        session_readiness_tests
        session_open_transaction_tests
        recording_open_workflow_tests
        session_loading_presentation_tests)
    add_executable(${runtime_test} tools/${runtime_test}.cpp)
    target_link_libraries(${runtime_test} PRIVATE crimson_runtime_contracts)
    target_compile_features(${runtime_test} PRIVATE cxx_std_17)
    set_target_properties(${runtime_test} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ${runtime_test} COMMAND $<TARGET_FILE:${runtime_test}>)
    set_tests_properties(${runtime_test} PROPERTIES
        LABELS "headless;portable;runtime")
endforeach()

add_executable(data_access_contract_tests
    tools/data_access_contract_tests.cpp
)
target_link_libraries(data_access_contract_tests PRIVATE
    crimson_data_access_contracts
)
target_compile_features(data_access_contract_tests PRIVATE cxx_std_17)
set_target_properties(data_access_contract_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME data_access_contract_tests
    COMMAND $<TARGET_FILE:data_access_contract_tests>)
set_tests_properties(data_access_contract_tests PROPERTIES
    LABELS "cache;headless;portable;scheduler"
)

add_executable(coordinate_contract_tests
    tools/coordinate_contract_tests.cpp
)
target_link_libraries(coordinate_contract_tests PRIVATE
    crimson_coordinate_contracts
)
target_compile_features(coordinate_contract_tests PRIVATE cxx_std_17)
set_target_properties(coordinate_contract_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME coordinate_contract_tests
    COMMAND $<TARGET_FILE:coordinate_contract_tests>)
set_tests_properties(coordinate_contract_tests PROPERTIES
    LABELS "coordinates;headless;portable"
)

add_executable(chaser_distance_polar_contract_tests
    tools/chaser_distance_polar_contract_tests.cpp
)
target_link_libraries(chaser_distance_polar_contract_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(chaser_distance_polar_contract_tests PRIVATE
    cxx_std_17)
set_target_properties(chaser_distance_polar_contract_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME chaser_distance_polar_contract_tests
    COMMAND $<TARGET_FILE:chaser_distance_polar_contract_tests>)
set_tests_properties(chaser_distance_polar_contract_tests PROPERTIES
    LABELS "headless;polar;portable;repository"
)

add_executable(chaser_distance_polar_buffer_tests
    tools/chaser_distance_polar_buffer_tests.cpp
)
target_link_libraries(chaser_distance_polar_buffer_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(chaser_distance_polar_buffer_tests PRIVATE cxx_std_17)
set_target_properties(chaser_distance_polar_buffer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME chaser_distance_polar_buffer_tests
    COMMAND $<TARGET_FILE:chaser_distance_polar_buffer_tests>)
set_tests_properties(chaser_distance_polar_buffer_tests PROPERTIES
    LABELS "buffer;headless;polar;portable;repository"
    TIMEOUT 30
)

add_executable(chaser_distance_polar_scene_tests
    tools/chaser_distance_polar_scene_tests.cpp
)
target_link_libraries(chaser_distance_polar_scene_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(chaser_distance_polar_scene_tests PRIVATE cxx_std_17)
set_target_properties(chaser_distance_polar_scene_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME chaser_distance_polar_scene_tests
    COMMAND $<TARGET_FILE:chaser_distance_polar_scene_tests>)
set_tests_properties(chaser_distance_polar_scene_tests PROPERTIES
    LABELS "headless;polar;portable;presentation;scene"
)

add_executable(workspace_state_tests tools/workspace_state_tests.cpp)
target_link_libraries(workspace_state_tests PRIVATE
    crimson_workspace_contracts
)
target_compile_features(workspace_state_tests PRIVATE cxx_std_17)
set_target_properties(workspace_state_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME workspace_state_tests
    COMMAND $<TARGET_FILE:workspace_state_tests>)
set_tests_properties(workspace_state_tests PROPERTIES
    LABELS "headless;portable;workspace"
)

add_executable(workspace_workflow_tests
    tools/workspace_workflow_tests.cpp
)
target_link_libraries(workspace_workflow_tests PRIVATE
    crimson_workspace_contracts
)
target_compile_features(workspace_workflow_tests PRIVATE cxx_std_17)
set_target_properties(workspace_workflow_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME workspace_workflow_tests
    COMMAND $<TARGET_FILE:workspace_workflow_tests>)
set_tests_properties(workspace_workflow_tests PROPERTIES
    LABELS "headless;portable;workspace;workflow"
)

add_executable(frame_inspect_workflow_tests
    tools/frame_inspect_workflow_tests.cpp
)
target_link_libraries(frame_inspect_workflow_tests PRIVATE
    crimson_workspace_contracts
)
target_compile_features(frame_inspect_workflow_tests PRIVATE cxx_std_17)
set_target_properties(frame_inspect_workflow_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME frame_inspect_workflow_tests
    COMMAND $<TARGET_FILE:frame_inspect_workflow_tests>)
set_tests_properties(frame_inspect_workflow_tests PROPERTIES
    LABELS "headless;portable;workspace;workflow"
)

add_executable(crop_source_contract_tests
    tools/crop_source_contract_tests.cpp
)
target_link_libraries(crop_source_contract_tests PRIVATE
    crimson_crop_contracts
)
target_compile_features(crop_source_contract_tests PRIVATE cxx_std_17)
set_target_properties(crop_source_contract_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME crop_source_contract_tests
    COMMAND $<TARGET_FILE:crop_source_contract_tests>)
set_tests_properties(crop_source_contract_tests PROPERTIES
    LABELS "crop;playback;portable"
)

add_executable(crop_presentation_coordinator_tests
    tools/crop_presentation_coordinator_tests.cpp
)
target_link_libraries(crop_presentation_coordinator_tests PRIVATE
    crimson_crop_contracts
)
target_compile_features(crop_presentation_coordinator_tests PRIVATE cxx_std_17)
set_target_properties(crop_presentation_coordinator_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME crop_presentation_coordinator_tests
    COMMAND $<TARGET_FILE:crop_presentation_coordinator_tests>)
set_tests_properties(crop_presentation_coordinator_tests PROPERTIES
    LABELS "crop;playback;portable;presentation"
)

add_executable(roi_inset_presentation_tests
    tools/roi_inset_presentation_tests.cpp
)
target_link_libraries(roi_inset_presentation_tests PRIVATE
    crimson_crop_contracts
)
target_compile_features(roi_inset_presentation_tests PRIVATE cxx_std_17)
set_target_properties(roi_inset_presentation_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME roi_inset_presentation_tests
    COMMAND $<TARGET_FILE:roi_inset_presentation_tests>)
set_tests_properties(roi_inset_presentation_tests PROPERTIES
    LABELS "crop;headless;portable;presentation"
)

add_executable(overlay_scene_contract_tests
    tools/overlay_scene_contract_tests.cpp
)
target_link_libraries(overlay_scene_contract_tests PRIVATE
    crimson_overlay_contracts
)
target_include_directories(overlay_scene_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(overlay_scene_contract_tests PRIVATE cxx_std_17)
set_target_properties(overlay_scene_contract_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME overlay_scene_contract_tests
    COMMAND $<TARGET_FILE:overlay_scene_contract_tests>)
set_tests_properties(overlay_scene_contract_tests PROPERTIES
    LABELS "headless;overlay;portable"
)

add_executable(read_only_overlay_scene_tests
    tools/read_only_overlay_scene_tests.cpp
)
target_link_libraries(read_only_overlay_scene_tests PRIVATE
    crimson_overlay_contracts
)
target_include_directories(read_only_overlay_scene_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(read_only_overlay_scene_tests PRIVATE cxx_std_17)
set_target_properties(read_only_overlay_scene_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME read_only_overlay_scene_tests
    COMMAND $<TARGET_FILE:read_only_overlay_scene_tests>)
set_tests_properties(read_only_overlay_scene_tests PROPERTIES
    LABELS "headless;overlay;portable"
)

add_executable(read_only_overlay_controls_tests
    tools/read_only_overlay_controls_tests.cpp
)
target_link_libraries(read_only_overlay_controls_tests PRIVATE
    crimson_overlay_contracts
)
target_include_directories(read_only_overlay_controls_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(read_only_overlay_controls_tests PRIVATE cxx_std_17)
set_target_properties(read_only_overlay_controls_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME read_only_overlay_controls_tests
    COMMAND $<TARGET_FILE:read_only_overlay_controls_tests>)
set_tests_properties(read_only_overlay_controls_tests PROPERTIES
    LABELS "controls;headless;overlay;portable"
)

add_executable(read_only_overlay_frame_coordinator_tests
    tools/read_only_overlay_frame_coordinator_tests.cpp
)
target_link_libraries(read_only_overlay_frame_coordinator_tests PRIVATE
    crimson_overlay_contracts
)
target_compile_features(read_only_overlay_frame_coordinator_tests
    PRIVATE cxx_std_17)
set_target_properties(read_only_overlay_frame_coordinator_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME read_only_overlay_frame_coordinator_tests
    COMMAND $<TARGET_FILE:read_only_overlay_frame_coordinator_tests>)
set_tests_properties(read_only_overlay_frame_coordinator_tests PROPERTIES
    LABELS "headless;overlay;portable;presentation"
)

add_executable(subject_mask_presentation_coordinator_tests
    tools/subject_mask_presentation_coordinator_tests.cpp
)
target_link_libraries(subject_mask_presentation_coordinator_tests PRIVATE
    crimson_overlay_contracts
)
target_compile_features(subject_mask_presentation_coordinator_tests
    PRIVATE cxx_std_17)
set_target_properties(subject_mask_presentation_coordinator_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME subject_mask_presentation_coordinator_tests
    COMMAND $<TARGET_FILE:subject_mask_presentation_coordinator_tests>)
set_tests_properties(subject_mask_presentation_coordinator_tests PROPERTIES
    LABELS "headless;masks;overlay;portable;presentation"
)

add_executable(eye_angle_timeline_tests
    tools/eye_angle_timeline_tests.cpp
)
target_link_libraries(eye_angle_timeline_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(eye_angle_timeline_tests PRIVATE cxx_std_17)
set_target_properties(eye_angle_timeline_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME eye_angle_timeline_tests
    COMMAND $<TARGET_FILE:eye_angle_timeline_tests>)
set_tests_properties(eye_angle_timeline_tests PROPERTIES
    LABELS "headless;portable;repository;timeline"
)

add_executable(analysis_series_timeline_tests
    tools/analysis_series_timeline_tests.cpp
)
target_link_libraries(analysis_series_timeline_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(analysis_series_timeline_tests PRIVATE cxx_std_17)
set_target_properties(analysis_series_timeline_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME analysis_series_timeline_tests
    COMMAND $<TARGET_FILE:analysis_series_timeline_tests>)
set_tests_properties(analysis_series_timeline_tests PROPERTIES
    LABELS "headless;portable;repository;timeline"
)

add_executable(stimulus_context_timeline_tests
    tools/stimulus_context_timeline_tests.cpp
)
target_link_libraries(stimulus_context_timeline_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(stimulus_context_timeline_tests PRIVATE cxx_std_17)
set_target_properties(stimulus_context_timeline_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME stimulus_context_timeline_tests
    COMMAND $<TARGET_FILE:stimulus_context_timeline_tests>)
set_tests_properties(stimulus_context_timeline_tests PROPERTIES
    LABELS "headless;portable;repository;stimulus;timeline"
)

add_executable(stimulus_camera_overlay_scene_tests
    tools/stimulus_camera_overlay_scene_tests.cpp
)
target_link_libraries(stimulus_camera_overlay_scene_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(stimulus_camera_overlay_scene_tests PRIVATE cxx_std_17)
set_target_properties(stimulus_camera_overlay_scene_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME stimulus_camera_overlay_scene_tests
    COMMAND $<TARGET_FILE:stimulus_camera_overlay_scene_tests>)
set_tests_properties(stimulus_camera_overlay_scene_tests PROPERTIES
    LABELS "headless;overlay;portable;presentation;scene;stimulus"
)

add_executable(swim_bout_timeline_tests
    tools/swim_bout_timeline_tests.cpp
)
target_link_libraries(swim_bout_timeline_tests PRIVATE
    crimson_repository_contracts
)
target_compile_features(swim_bout_timeline_tests PRIVATE cxx_std_17)
set_target_properties(swim_bout_timeline_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME swim_bout_timeline_tests
    COMMAND $<TARGET_FILE:swim_bout_timeline_tests>)
set_tests_properties(swim_bout_timeline_tests PROPERTIES
    LABELS "headless;portable;repository;swim_bout;timeline"
)

add_executable(stimulus_playback_coordinator_tests
    tools/stimulus_playback_coordinator_tests.cpp
)
target_link_libraries(stimulus_playback_coordinator_tests PRIVATE
    crimson_stimulus_playback_contracts
)
target_compile_features(stimulus_playback_coordinator_tests PRIVATE cxx_std_17)
set_target_properties(stimulus_playback_coordinator_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME stimulus_playback_coordinator_tests
    COMMAND $<TARGET_FILE:stimulus_playback_coordinator_tests>)
set_tests_properties(stimulus_playback_coordinator_tests PROPERTIES
    LABELS "playback;portable;stimulus"
)

add_executable(stimulus_presentation_coordinator_tests
    tools/stimulus_presentation_coordinator_tests.cpp
)
target_link_libraries(stimulus_presentation_coordinator_tests PRIVATE
    crimson_stimulus_playback_contracts
)
target_compile_features(stimulus_presentation_coordinator_tests PRIVATE cxx_std_17)
set_target_properties(stimulus_presentation_coordinator_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_test(NAME stimulus_presentation_coordinator_tests
    COMMAND $<TARGET_FILE:stimulus_presentation_coordinator_tests>)
set_tests_properties(stimulus_presentation_coordinator_tests PROPERTIES
    LABELS "playback;portable;stimulus"
)

if(CRIMSON_BUILD_NVIDIA_APP)
    enable_language(CUDA)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|AMD64|amd64|i[3-6]86)$")
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-mssse3>)
    endif()
endif()

# Set CUDA standard and options
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CMAKE_CUDA_ARCHITECTURES "80;86")  # Support both sm_80 and sm_86

# Suppress deprecated FindCUDA warning
cmake_policy(SET CMP0146 OLD)

function(crimson_require_exact_version package_name actual_version expected_version)
    if(NOT expected_version)
        return()
    endif()

    if(NOT actual_version)
        message(FATAL_ERROR
            "${package_name} version check failed.\n"
            "  Expected: ${expected_version}\n"
            "  Found:    <not detected>")
    endif()

    if(NOT "${actual_version}" STREQUAL "${expected_version}")
        message(FATAL_ERROR
            "${package_name} version mismatch.\n"
            "  Expected: ${expected_version}\n"
            "  Found:    ${actual_version}")
    endif()

    message(STATUS "${package_name} version matches expected ${expected_version}")
endfunction()

function(crimson_read_tensorrt_version include_dir out_var)
    set(tensorrt_version "")

    if(NOT include_dir)
        set(${out_var} "${tensorrt_version}" PARENT_SCOPE)
        return()
    endif()

    set(version_header "${include_dir}/NvInferVersion.h")
    if(NOT EXISTS "${version_header}")
        set(${out_var} "${tensorrt_version}" PARENT_SCOPE)
        return()
    endif()

    file(STRINGS "${version_header}" tensorrt_major_line
        REGEX "^#define NV_TENSORRT_MAJOR [0-9]+")
    file(STRINGS "${version_header}" tensorrt_minor_line
        REGEX "^#define NV_TENSORRT_MINOR [0-9]+")
    file(STRINGS "${version_header}" tensorrt_patch_line
        REGEX "^#define NV_TENSORRT_PATCH [0-9]+")
    file(STRINGS "${version_header}" tensorrt_build_line
        REGEX "^#define NV_TENSORRT_BUILD [0-9]+")

    if(tensorrt_major_line AND tensorrt_minor_line AND tensorrt_patch_line
       AND tensorrt_build_line)
        string(REGEX MATCH "[0-9]+" tensorrt_major "${tensorrt_major_line}")
        string(REGEX MATCH "[0-9]+" tensorrt_minor "${tensorrt_minor_line}")
        string(REGEX MATCH "[0-9]+" tensorrt_patch "${tensorrt_patch_line}")
        string(REGEX MATCH "[0-9]+" tensorrt_build "${tensorrt_build_line}")
        set(tensorrt_version
            "${tensorrt_major}.${tensorrt_minor}.${tensorrt_patch}.${tensorrt_build}")
    endif()

    set(${out_var} "${tensorrt_version}" PARENT_SCOPE)
endfunction()

function(crimson_require_found variable_name description)
    set(value "${${variable_name}}")
    if(NOT value OR value MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "${description} was not found.\n"
            "  ${variable_name}=${value}\n"
            "  FFMPEG_ROOT=${FFMPEG_ROOT}\n"
            "  VIDEO_CODEC_SDK_ROOT=${VIDEO_CODEC_SDK_ROOT}\n"
            "  Hint: On Windows, FFmpeg must provide development headers and "
            "MSVC import libraries, not only runtime DLLs. If using Media "
            "Autobuild Suite, run tools/stage_windows_ffmpeg_nvidia.ps1.")
    endif()
endfunction()

# --- Dependency discovery roots ---
if(NOT OpenCV_DIR AND DEFINED ENV{OpenCV_DIR} AND NOT "$ENV{OpenCV_DIR}" STREQUAL "")
    set(OpenCV_DIR "$ENV{OpenCV_DIR}")
endif()
set(OpenCV_DIR "${OpenCV_DIR}" CACHE PATH
    "Path to the OpenCV CMake package directory (the folder containing OpenCVConfig.cmake)")

if(NOT TENSORRT_ROOT AND DEFINED ENV{TENSORRT_ROOT} AND NOT "$ENV{TENSORRT_ROOT}" STREQUAL "")
    set(TENSORRT_ROOT "$ENV{TENSORRT_ROOT}")
endif()
set(TENSORRT_ROOT "${TENSORRT_ROOT}" CACHE PATH "Path to the TensorRT installation root")

if(NOT CRIMSON_VCPKG_BIN_DIR AND DEFINED ENV{CRIMSON_VCPKG_BIN_DIR} AND NOT "$ENV{CRIMSON_VCPKG_BIN_DIR}" STREQUAL "")
    set(CRIMSON_VCPKG_BIN_DIR "$ENV{CRIMSON_VCPKG_BIN_DIR}")
endif()
set(CRIMSON_VCPKG_BIN_DIR "${CRIMSON_VCPKG_BIN_DIR}" CACHE PATH
    "Optional vcpkg runtime DLL directory for Windows packaging")

if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT} AND NOT "$ENV{FFMPEG_ROOT}" STREQUAL "")
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()
set(FFMPEG_ROOT "${FFMPEG_ROOT}" CACHE PATH "Path to a custom FFmpeg installation root")

if(NOT VIDEO_CODEC_SDK_ROOT AND DEFINED ENV{VIDEO_CODEC_SDK_ROOT} AND NOT "$ENV{VIDEO_CODEC_SDK_ROOT}" STREQUAL "")
    set(VIDEO_CODEC_SDK_ROOT "$ENV{VIDEO_CODEC_SDK_ROOT}")
endif()
if(NOT VIDEO_CODEC_SDK_ROOT AND DEFINED ENV{CRIMSON_VIDEO_CODEC_SDK_ROOT} AND NOT "$ENV{CRIMSON_VIDEO_CODEC_SDK_ROOT}" STREQUAL "")
    set(VIDEO_CODEC_SDK_ROOT "$ENV{CRIMSON_VIDEO_CODEC_SDK_ROOT}")
endif()
set(VIDEO_CODEC_SDK_ROOT "${VIDEO_CODEC_SDK_ROOT}" CACHE PATH
    "Path to the NVIDIA Video Codec SDK installation root")

set(CRIMSON_EXPECT_CUDA_VERSION "${CRIMSON_EXPECT_CUDA_VERSION}" CACHE STRING
    "Expected CUDA toolkit version (optional exact match)")
set(CRIMSON_EXPECT_OPENCV_VERSION "${CRIMSON_EXPECT_OPENCV_VERSION}" CACHE STRING
    "Expected OpenCV version (optional exact match)")
set(CRIMSON_EXPECT_TENSORRT_VERSION "${CRIMSON_EXPECT_TENSORRT_VERSION}" CACHE STRING
    "Expected TensorRT version (optional exact match)")

set(CRIMSON_FFMPEG_INCLUDE_HINTS "")
set(CRIMSON_FFMPEG_LIBRARY_HINTS "")
if(FFMPEG_ROOT)
    list(APPEND CRIMSON_FFMPEG_INCLUDE_HINTS "${FFMPEG_ROOT}/include")
    list(APPEND CRIMSON_FFMPEG_LIBRARY_HINTS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib64"
        "${FFMPEG_ROOT}/lib/x64")
endif()

set(CRIMSON_CUDA_INCLUDE_HINTS "")
set(CRIMSON_CUDA_LIBRARY_HINTS "")
if(CUDA_TOOLKIT_ROOT_DIR)
    list(APPEND CRIMSON_CUDA_INCLUDE_HINTS "${CUDA_TOOLKIT_ROOT_DIR}/include")
    list(APPEND CRIMSON_CUDA_LIBRARY_HINTS
        "${CUDA_TOOLKIT_ROOT_DIR}/lib64"
        "${CUDA_TOOLKIT_ROOT_DIR}/lib/x64")
endif()
if(UNIX)
    list(APPEND CRIMSON_CUDA_INCLUDE_HINTS "/usr/local/cuda/include")
    list(APPEND CRIMSON_CUDA_LIBRARY_HINTS "/usr/local/cuda/lib64")
endif()

set(CRIMSON_VIDEO_CODEC_SDK_INCLUDE_HINTS "")
set(CRIMSON_VIDEO_CODEC_SDK_LIBRARY_HINTS "")
if(VIDEO_CODEC_SDK_ROOT)
    list(APPEND CRIMSON_VIDEO_CODEC_SDK_INCLUDE_HINTS
        "${VIDEO_CODEC_SDK_ROOT}/Interface"
        "${VIDEO_CODEC_SDK_ROOT}/include")
    list(APPEND CRIMSON_VIDEO_CODEC_SDK_LIBRARY_HINTS
        "${VIDEO_CODEC_SDK_ROOT}/Lib/win/x64"
        "${VIDEO_CODEC_SDK_ROOT}/lib/win/x64"
        "${VIDEO_CODEC_SDK_ROOT}/Lib/x64"
        "${VIDEO_CODEC_SDK_ROOT}/lib/x64"
        "${VIDEO_CODEC_SDK_ROOT}/Lib"
        "${VIDEO_CODEC_SDK_ROOT}/lib")
endif()
if(WIN32)
    list(APPEND CRIMSON_VIDEO_CODEC_SDK_LIBRARY_HINTS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec/x64")
endif()

set(CRIMSON_NV_CODEC_LIBRARY_HINTS
    ${CRIMSON_VIDEO_CODEC_SDK_LIBRARY_HINTS}
    ${CRIMSON_CUDA_LIBRARY_HINTS})

set(CRIMSON_TENSORRT_INCLUDE_HINTS "")
set(CRIMSON_TENSORRT_LIBRARY_HINTS "")
if(TENSORRT_ROOT)
    list(APPEND CRIMSON_TENSORRT_INCLUDE_HINTS "${TENSORRT_ROOT}/include")
    list(APPEND CRIMSON_TENSORRT_LIBRARY_HINTS
        "${TENSORRT_ROOT}/lib"
        "${TENSORRT_ROOT}/lib64"
        "${TENSORRT_ROOT}/lib/x64")
endif()

function(crimson_append_existing_dir list_var dir_path)
    if(NOT dir_path)
        return()
    endif()
    if(EXISTS "${dir_path}")
        list(APPEND ${list_var} "${dir_path}")
        list(REMOVE_DUPLICATES ${list_var})
        set(${list_var} "${${list_var}}" PARENT_SCOPE)
    endif()
endfunction()

set(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "")
if(WIN32)
    if(CUDA_TOOLKIT_ROOT_DIR)
        crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CUDA_TOOLKIT_ROOT_DIR}/bin")
    endif()
    if(CUDAToolkit_ROOT)
        crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CUDAToolkit_ROOT}/bin")
    endif()
    crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${FFMPEG_ROOT}/bin")
    crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${TENSORRT_ROOT}/bin")
    crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${TENSORRT_ROOT}/lib")
    crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CRIMSON_VCPKG_BIN_DIR}")

    if(OpenCV_DIR)
        crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${OpenCV_DIR}/x64/vc17/bin")
        crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${OpenCV_DIR}/bin")
        get_filename_component(CRIMSON_OPENCV_DIR_LEAF "${OpenCV_DIR}" NAME)
        if(CRIMSON_OPENCV_DIR_LEAF STREQUAL "lib")
            get_filename_component(CRIMSON_OPENCV_DIR_PARENT "${OpenCV_DIR}" DIRECTORY)
            crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CRIMSON_OPENCV_DIR_PARENT}/bin")
        endif()
        set(CRIMSON_OPENCV_RUNTIME_CURSOR "${OpenCV_DIR}")
        foreach(_crimson_opencv_parent_index RANGE 1 4)
            get_filename_component(CRIMSON_OPENCV_RUNTIME_CURSOR "${CRIMSON_OPENCV_RUNTIME_CURSOR}" DIRECTORY)
            if(NOT CRIMSON_OPENCV_RUNTIME_CURSOR)
                break()
            endif()
            crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CRIMSON_OPENCV_RUNTIME_CURSOR}/x64/vc17/bin")
            crimson_append_existing_dir(CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS "${CRIMSON_OPENCV_RUNTIME_CURSOR}/bin")
        endforeach()
    endif()
endif()

# --- Set RPATH for relocatable installs ---
if(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
    set(CMAKE_INSTALL_RPATH
        "$ORIGIN"
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/crimson/private")
endif()

# --- Find All Dependencies ---
find_package(CUDA REQUIRED)
find_package(OpenGL REQUIRED)
find_package(OpenCV REQUIRED)  # Will use OpenCV_DIR set above
find_package(GLEW REQUIRED)
find_package(glfw3 REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS CXX)
if(CRIMSON_ENABLE_PARQUET)
    find_package(Arrow CONFIG QUIET)
    find_package(Parquet CONFIG QUIET)
    if(NOT Arrow_FOUND OR NOT Parquet_FOUND)
        message(FATAL_ERROR
            "CRIMSON_ENABLE_PARQUET=ON requires Arrow and Parquet CMake packages.\n"
            "Provide a package prefix, for example:\n"
            "  -DCMAKE_PREFIX_PATH=/path/to/arrow-prefix\n"
            "or explicit package dirs:\n"
            "  -DArrow_DIR=/path/to/lib/cmake/Arrow\n"
            "  -DParquet_DIR=/path/to/lib/cmake/Parquet\n"
            "Disable clipped Parquet support with -DCRIMSON_ENABLE_PARQUET=OFF.")
    endif()
endif()

crimson_require_exact_version("CUDA" "${CUDA_VERSION}" "${CRIMSON_EXPECT_CUDA_VERSION}")
crimson_require_exact_version("OpenCV" "${OpenCV_VERSION}" "${CRIMSON_EXPECT_OPENCV_VERSION}")

if(CUDA_TOOLKIT_ROOT_DIR)
    list(APPEND CRIMSON_CUDA_INCLUDE_HINTS "${CUDA_TOOLKIT_ROOT_DIR}/include")
    list(APPEND CRIMSON_CUDA_LIBRARY_HINTS
        "${CUDA_TOOLKIT_ROOT_DIR}/lib64"
        "${CUDA_TOOLKIT_ROOT_DIR}/lib/x64")
endif()

message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")

set(CRIMSON_ARROW_TARGET "")
set(CRIMSON_PARQUET_TARGET "")
if(CRIMSON_ENABLE_PARQUET)
    if(TARGET Arrow::arrow_shared)
        set(CRIMSON_ARROW_TARGET Arrow::arrow_shared)
    elseif(TARGET Arrow::arrow_static)
        set(CRIMSON_ARROW_TARGET Arrow::arrow_static)
    else()
        message(FATAL_ERROR
            "Arrow package found, but no Arrow::arrow_shared or "
            "Arrow::arrow_static target is available")
    endif()

    if(TARGET Parquet::parquet_shared)
        set(CRIMSON_PARQUET_TARGET Parquet::parquet_shared)
    elseif(TARGET Parquet::parquet_static)
        set(CRIMSON_PARQUET_TARGET Parquet::parquet_static)
    else()
        message(FATAL_ERROR
            "Parquet package found, but no Parquet::parquet_shared or "
            "Parquet::parquet_static target is available")
    endif()
endif()

function(crimson_configure_parquet_support target_name)
    if(CRIMSON_ENABLE_PARQUET)
        target_compile_definitions(${target_name} PRIVATE CRIMSON_ENABLE_PARQUET=1)
        target_link_libraries(${target_name} PRIVATE
            ${CRIMSON_ARROW_TARGET}
            ${CRIMSON_PARQUET_TARGET}
        )
    else()
        target_compile_definitions(${target_name} PRIVATE CRIMSON_ENABLE_PARQUET=0)
    endif()
endfunction()

# --- FFmpeg with NVIDIA support ---
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h HINTS ${CRIMSON_FFMPEG_INCLUDE_HINTS})
find_library(AVFORMAT_LIBRARY avformat HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(AVCODEC_LIBRARY avcodec HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(AVUTIL_LIBRARY avutil HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(SWSCALE_LIBRARY swscale HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(SWRESAMPLE_LIBRARY swresample HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
crimson_require_found(AVFORMAT_INCLUDE_DIR "FFmpeg headers")
crimson_require_found(AVFORMAT_LIBRARY "FFmpeg avformat import library")
crimson_require_found(AVCODEC_LIBRARY "FFmpeg avcodec import library")
crimson_require_found(AVUTIL_LIBRARY "FFmpeg avutil import library")
crimson_require_found(SWSCALE_LIBRARY "FFmpeg swscale import library")
crimson_require_found(SWRESAMPLE_LIBRARY "FFmpeg swresample import library")
set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
set(FFMPEG_LIBRARIES ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})

# NVML headers (from libnvidia-ml-dev OR CUDA Toolkit)
find_path(NVML_INCLUDE_DIR nvml.h
    HINTS ${CRIMSON_CUDA_INCLUDE_HINTS}
)

# NVML library: accept both unversioned .so and SONAME .so.1
if(ENABLE_NVML)
    find_library(NVML_LIBRARY
        NAMES nvidia-ml nvml libnvidia-ml.so libnvidia-ml.so.1 nvidia-ml.so nvidia-ml.so.1
        HINTS ${CRIMSON_CUDA_LIBRARY_HINTS}
    )
endif()

# --- CUDA Libraries ---
find_library(CUDA_DRIVER NAMES cuda nvcuda HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})
find_library(NVCUVID_LIBRARY NAMES nvcuvid HINTS ${CRIMSON_NV_CODEC_LIBRARY_HINTS})
crimson_require_found(NVCUVID_LIBRARY "NVIDIA nvcuvid import library")

# NVENC is only needed by future video encode/export paths, not GUI playback.
if(CRIMSON_ENABLE_NVENC)
    find_library(NVENC_LIBRARY
        NAMES nvidia-encode nvencodeapi
        HINTS ${CRIMSON_NV_CODEC_LIBRARY_HINTS}
    )
    crimson_require_found(NVENC_LIBRARY "NVIDIA nvencodeapi import library")
else()
    set(NVENC_LIBRARY "")
endif()

# NPP Libraries
find_library(NPPC_LIBRARY NAMES nppc HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})
find_library(NPPIG_LIBRARY NAMES nppig HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})
find_library(NPPICC_LIBRARY NAMES nppicc HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})
find_library(NPPIDEI_LIBRARY NAMES nppidei HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})
find_library(NPPIAL_LIBRARY NAMES nppial HINTS ${CRIMSON_CUDA_LIBRARY_HINTS})

# --- TensorRT Libraries ---
find_path(TENSORRT_INCLUDE_DIR NvInfer.h HINTS ${CRIMSON_TENSORRT_INCLUDE_HINTS})
find_library(NVINFER_LIBRARY
    NAMES nvinfer nvinfer_10
    HINTS ${CRIMSON_TENSORRT_LIBRARY_HINTS})
find_library(NVINFER_PLUGIN_LIBRARY
    NAMES nvinfer_plugin nvinfer_plugin_10
    HINTS ${CRIMSON_TENSORRT_LIBRARY_HINTS})
crimson_read_tensorrt_version("${TENSORRT_INCLUDE_DIR}" TENSORRT_VERSION)
if(TENSORRT_INCLUDE_DIR AND NVINFER_LIBRARY)
    set(TENSORRT_FOUND TRUE)
    if(TENSORRT_ROOT)
        message(STATUS "TensorRT found at: ${TENSORRT_ROOT}")
    else()
        message(STATUS "TensorRT found via default search paths")
    endif()
else()
    set(TENSORRT_FOUND FALSE)
    message(STATUS "TensorRT not found")
endif()
crimson_require_exact_version("TensorRT" "${TENSORRT_VERSION}" "${CRIMSON_EXPECT_TENSORRT_VERSION}")

# --- Third-Party Source Definitions (ImGui/ImPlot) ---
function(crimson_resolve_vendored_dir out_var relative_dir required_file)
    set(_local_dir "${CMAKE_CURRENT_SOURCE_DIR}/${relative_dir}")
    if(EXISTS "${_local_dir}/${required_file}")
        set(${out_var} "${_local_dir}" PARENT_SCOPE)
        return()
    endif()

    get_filename_component(_repo_parent "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
    set(_fallback_dir "${_repo_parent}/crimson/${relative_dir}")
    if(EXISTS "${_fallback_dir}/${required_file}")
        message(STATUS "Using vendored dependency from sibling checkout: ${_fallback_dir}")
        set(${out_var} "${_fallback_dir}" PARENT_SCOPE)
        return()
    endif()

    set(${out_var} "${_local_dir}" PARENT_SCOPE)
endfunction()

crimson_resolve_vendored_dir(IMGUI_DIR "third_party/imgui" "imgui.cpp")
crimson_resolve_vendored_dir(IMPLOT_DIR "third_party/implot" "implot.cpp")
crimson_resolve_vendored_dir(IMGUI_FILE_DIALOG_DIR "third_party/ImGuiFileDialog" "ImGuiFileDialog.cpp")
crimson_resolve_vendored_dir(ICON_FONT_CPP_HEADERS_DIR "third_party/IconFontCppHeaders" "IconsFontAwesome6.h")

# ImGui sources
if(EXISTS ${IMGUI_DIR})
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
else()
    message(FATAL_ERROR "ImGui not found at ${IMGUI_DIR}")
endif()

# ImPlot sources
if(EXISTS ${IMPLOT_DIR})
    set(IMPLOT_SOURCES
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
        ${IMPLOT_DIR}/implot_demo.cpp
    )
else()
    message(WARNING "ImPlot not found at ${IMPLOT_DIR}")
endif()

# ImGuiFileDialog sources (optional)
if(EXISTS ${IMGUI_FILE_DIALOG_DIR})
    set(IMGUI_FILE_DIALOG_SOURCES
        ${IMGUI_FILE_DIALOG_DIR}/ImGuiFileDialog.cpp
    )
endif()

# ============================================================================
# TensorStore for Zarr support
# ============================================================================
message(STATUS "Configuring TensorStore for Zarr support...")

set(CRIMSON_TENSORSTORE_IMPORTED_FROM_PREBUILT OFF)

function(crimson_import_prebuilt_tensorstore prebuilt_build_dir)
    find_program(CRIMSON_JQ_EXECUTABLE jq)
    if(NOT CRIMSON_JQ_EXECUTABLE)
        message(STATUS "jq not found; cannot import prebuilt TensorStore metadata")
        return()
    endif()

    set(_compile_commands "${prebuilt_build_dir}/compile_commands.json")
    set(_link_txt "${prebuilt_build_dir}/CMakeFiles/ts_zarr3_check.dir/link.txt")
    set(_nlohmann_include "${prebuilt_build_dir}/_deps/nlohmann_json-src/include")

    if(NOT EXISTS "${_compile_commands}" OR NOT EXISTS "${_link_txt}")
        return()
    endif()

    execute_process(
        COMMAND ${CRIMSON_JQ_EXECUTABLE} -r
                ".[] | select(.file|endswith(\"tools/test_zarr3_read.cpp\")) | .command"
                "${_compile_commands}"
        RESULT_VARIABLE _compile_result
        OUTPUT_VARIABLE _compile_command
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _compile_result EQUAL 0 OR _compile_command STREQUAL "")
        message(STATUS "Could not extract prebuilt TensorStore compile metadata from ${_compile_commands}")
        return()
    endif()

    separate_arguments(_compile_args UNIX_COMMAND "${_compile_command}")
    set(_tensorstore_include_dirs "")
    set(_tensorstore_compile_defs "")
    foreach(_arg IN LISTS _compile_args)
        if(_arg MATCHES "^-I(.+)")
            list(APPEND _tensorstore_include_dirs "${CMAKE_MATCH_1}")
        elseif(_arg MATCHES "^-D(.+)")
            list(APPEND _tensorstore_compile_defs "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    file(READ "${_link_txt}" _link_line)
    string(STRIP "${_link_line}" _link_line)
    string(REGEX REPLACE "^.* -o [^ ]+ +" "" _link_tail "${_link_line}")
    if(_link_tail STREQUAL "${_link_line}" OR _link_tail STREQUAL "")
        message(STATUS "Could not parse prebuilt TensorStore link metadata from ${_link_txt}")
        return()
    endif()

    separate_arguments(_link_args UNIX_COMMAND "${_link_tail}")
    set(_tensorstore_link_args "")
    foreach(_arg IN LISTS _link_args)
        if(_arg MATCHES "^-Wl,-rpath,")
            continue()
        elseif(_arg MATCHES "^_deps/")
            list(APPEND _tensorstore_link_args "${prebuilt_build_dir}/${_arg}")
        elseif(_arg MATCHES "^-L(_deps/.+)")
            list(APPEND _tensorstore_link_args "-L${prebuilt_build_dir}/${CMAKE_MATCH_1}")
        else()
            list(APPEND _tensorstore_link_args "${_arg}")
        endif()
    endforeach()

    set(_tensorstore_rpath_dirs "")
    if(OpenCV_DIR)
        get_filename_component(_opencv_config_parent "${OpenCV_DIR}" DIRECTORY)
        get_filename_component(_opencv_lib_dir "${_opencv_config_parent}" DIRECTORY)
        crimson_append_existing_dir(_tensorstore_rpath_dirs "${_opencv_lib_dir}")
    endif()
    crimson_append_existing_dir(_tensorstore_rpath_dirs "${TENSORRT_ROOT}/lib")
    crimson_append_existing_dir(_tensorstore_rpath_dirs "${FFMPEG_ROOT}/lib")
    if(_tensorstore_rpath_dirs)
        list(JOIN _tensorstore_rpath_dirs ":" _tensorstore_rpath)
        list(PREPEND _tensorstore_link_args "-Wl,-rpath,${_tensorstore_rpath}")
    endif()

    list(JOIN _tensorstore_link_args "\n" _tensorstore_link_rsp_content)
    set(_tensorstore_link_rsp "${CMAKE_CURRENT_BINARY_DIR}/tensorstore-prebuilt-link.rsp")
    file(WRITE "${_tensorstore_link_rsp}" "${_tensorstore_link_rsp_content}\n")

    if(NOT TARGET nlohmann_json)
        add_library(nlohmann_json INTERFACE)
        target_include_directories(nlohmann_json INTERFACE "${_nlohmann_include}")
    endif()
    if(NOT TARGET nlohmann_json::nlohmann_json)
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    endif()

    add_library(crimson_prebuilt_tensorstore INTERFACE)
    target_include_directories(crimson_prebuilt_tensorstore INTERFACE ${_tensorstore_include_dirs})
    target_compile_definitions(crimson_prebuilt_tensorstore INTERFACE ${_tensorstore_compile_defs})
    target_compile_features(crimson_prebuilt_tensorstore INTERFACE cxx_std_17)

    if(NOT TARGET tensorstore::all_drivers)
        add_library(tensorstore::all_drivers ALIAS crimson_prebuilt_tensorstore)
    endif()

    set(CMAKE_CXX_LINK_EXECUTABLE
        "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> @${_tensorstore_link_rsp} -o <TARGET> <LINK_LIBRARIES>"
        PARENT_SCOPE)
    set(CRIMSON_TENSORSTORE_IMPORTED_FROM_PREBUILT ON PARENT_SCOPE)
    set(CRIMSON_TENSORSTORE_IMPORTED_FROM_PREBUILT ON CACHE INTERNAL "Imported TensorStore from a sibling build")
    message(STATUS "Reusing prebuilt TensorStore from ${prebuilt_build_dir}")
endfunction()

get_filename_component(CRIMSON_WORKTREE_PARENT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
set(CRIMSON_PREBUILT_TENSORSTORE_BUILD_DIR
    "${CRIMSON_WORKTREE_PARENT}/crimson/build"
    CACHE PATH "Existing Crimson build directory to reuse for TensorStore imports")

if(EXISTS "${CRIMSON_PREBUILT_TENSORSTORE_BUILD_DIR}")
    crimson_import_prebuilt_tensorstore("${CRIMSON_PREBUILT_TENSORSTORE_BUILD_DIR}")
endif()

if(NOT CRIMSON_TENSORSTORE_IMPORTED_FROM_PREBUILT)
    crimson_fetch_tensorstore()
endif()
crimson_add_tensorstore_repositories(tensorstore::all_drivers)

# ============================================================================
# TensorStore Zarr3 build/test-time verification
# ============================================================================
if(ENABLE_TENSORSTORE_ZARR3_CHECK)
  # --------------------------------------------------------------------------
  # Create a tiny runtime checker to confirm that TensorStore's zarr3 driver is
  # actually registered in this build. The checker will attempt to open an
  # in-memory zarr3 array; if successful, it prints "Zarr3: OK".
  # --------------------------------------------------------------------------

  # Write source file to tools/ if it doesn't exist already
  set(TS_ZARR3_CHECK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tools/ts_zarr3_check.cpp)
  if(NOT EXISTS ${TS_ZARR3_CHECK_SRC})
    file(WRITE ${TS_ZARR3_CHECK_SRC}
"#include <iostream>
#include <tensorstore/tensorstore.h>
#include <tensorstore/context.h>

int main() {
  auto ctx = tensorstore::Context::Default();
  auto spec_result = tensorstore::Spec::FromJson(R\"json(
    {
      \"driver\": \"zarr3\",
      \"kvstore\": {\"driver\": \"memory\"},
      \"dtype\": \"uint8\",
      \"shape\": [1],
      \"create\": true,
      \"open\": true
    }
  )json\");
  if (!spec_result.ok()) {
    std::cerr << \"Spec parse failed: \" << spec_result.status() << \"\\n\";
    return 2;
  }
  auto open_result = tensorstore::Open(*spec_result, ctx).result();
  if (!open_result.ok()) {
    std::cerr << \"Open failed: \" << open_result.status() << \"\\n\";
    return 3;
  }
  std::cout << \"Zarr3: OK\\n\";
  return 0;
}
")
  endif()

  # --------------------------------------------------------------------------
  # Build and register the test
  # --------------------------------------------------------------------------
  add_executable(ts_zarr3_check ${TS_ZARR3_CHECK_SRC})
  target_link_libraries(ts_zarr3_check PRIVATE
    tensorstore::all_drivers
    Threads::Threads
  )
  target_compile_features(ts_zarr3_check PRIVATE cxx_std_17)

  # Enable testing and add a ctest entry
  enable_testing()
  add_test(NAME tensorstore_zarr3_check COMMAND ts_zarr3_check)

  # Convenience target: run the test automatically during build
  add_custom_target(probe_zarr3 ALL
    COMMAND ${CMAKE_CTEST_COMMAND} -R tensorstore_zarr3_check --output-on-failure
    DEPENDS ts_zarr3_check
    COMMENT "Probing TensorStore for zarr3 driver support..."
  )
endif()

message(STATUS "TensorStore configured successfully")

# Tiny Zarr v3 (Palette) smoke test
add_executable(test_zarr3_read tools/test_zarr3_read.cpp)
target_link_libraries(test_zarr3_read PRIVATE
    tensorstore::all_drivers
    Threads::Threads
    nlohmann_json::nlohmann_json
)
target_compile_features(test_zarr3_read PRIVATE cxx_std_17)

add_executable(zarr_calibration_probe tools/zarr_calibration_probe.cpp)
target_link_libraries(zarr_calibration_probe PRIVATE
    tensorstore::all_drivers
    Threads::Threads
    nlohmann_json::nlohmann_json
)
target_compile_features(zarr_calibration_probe PRIVATE cxx_std_17)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE CRIMSON_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CRIMSON_GIT_COMMIT)
    set(CRIMSON_GIT_COMMIT "unknown")
endif()

add_executable(crimson_decode_smoke
    tools/crimson_decode_smoke.cpp
    src/FFmpegDemuxer.cpp
    src/NvDecoder.cpp
)
target_include_directories(crimson_decode_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec
    ${CUDA_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(crimson_decode_smoke PRIVATE
    ${CUDA_LIBRARIES}
    ${CUDA_DRIVER}
    ${NVCUVID_LIBRARY}
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)
target_compile_features(crimson_decode_smoke PRIVATE cxx_std_17)
target_compile_definitions(crimson_decode_smoke PRIVATE
    CRIMSON_GIT_COMMIT="${CRIMSON_GIT_COMMIT}"
)

add_executable(ui_path_config_tests
    tools/ui_path_config_tests.cpp
    src/ui_path_config.cpp
)
target_include_directories(ui_path_config_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(ui_path_config_tests PRIVATE
    crimson_recording_contracts
    nlohmann_json::nlohmann_json
)
target_compile_features(ui_path_config_tests PRIVATE cxx_std_17)
add_test(NAME ui_path_config_tests COMMAND ui_path_config_tests)
# --- Source Files ---
# CUDA sources
set(CUDA_SOURCES "")
set(POSSIBLE_CUDA_SOURCES
    src/create_image_cuda.cu
    src/ColorSpace.cu
    src/kernel.cu
)

foreach(CUDA_FILE ${POSSIBLE_CUDA_SOURCES})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${CUDA_FILE})
        list(APPEND CUDA_SOURCES ${CUDA_FILE})
        message(STATUS "Found CUDA source: ${CUDA_FILE}")
    endif()
endforeach()

# C++ sources
file(GLOB CPP_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/gui/*.cpp"
    "src/zarr/*.cpp")
list(APPEND CPP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/nvidia/nvidia_frame_inspect_adapter.cpp"
)
list(REMOVE_ITEM CPP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/coordinate_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/data_access.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/data_access_cache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/data_access_diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/data_access_scheduler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/chaser_distance_polar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/chaser_distance_polar_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/chaser_distance_polar_scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/canonical_detection_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detection_quality_timeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/detection_quality_timeline_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/keypoint_quality_timeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/keypoint_quality_timeline_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stimulus_camera_overlay_scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/swim_bout_timeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/swim_bout_timeline_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/crop_presentation_coordinator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/crop_source_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_types.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_selection.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_slot.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/session_lifecycle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/loading_progress.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/frame_presentation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostic_report.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/session_readiness.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/session_open_transaction.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recording_open_workflow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/session_loading_presentation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/workspace_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/overlay_scene_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/read_only_overlay_scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/canonical_detection_overlay_scene_adapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/canonical_json.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/canonical_detection_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/coordinate_catalog_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/crop_geometry_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/keypoint_v2_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/detection_repository_selection.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/refined_detection_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/subject_mask_bundle_v2_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/subject_mask_sampled_contour_v1_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/subject_mask_v1_contract.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/analysis_crop_geometry_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/stimulus_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/acquisition_crop_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_archive_context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_affiliated_video_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_analysis_series_timeline_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_chaser_distance_polar_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_canonical_detection_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_detection_quality_timeline_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_refined_detection_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_eye_angle_timeline_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_eye_geometry_overlay_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_analysis_crop_geometry_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_acquisition_crop_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_keypoint_overlay_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_keypoint_v2_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_stimulus_context_timeline_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_swim_bout_timeline_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_stimulus_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_subject_mask_overlay_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/tensorstore_subject_shape_overlay_repository.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zarr/zarr_metadata_equivalence.cpp"
)

# --- Create the Executable ---
add_executable(redgui
    ${CPP_SOURCES}
    ${CUDA_SOURCES}
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${IMGUI_FILE_DIALOG_SOURCES}
)
target_compile_definitions(redgui PRIVATE IMGUI_ENABLE_TEST_ENGINE)

# --- Set Include Directories ---
target_include_directories(redgui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec
    ${ICON_FONT_CPP_HEADERS_DIR}
    ${CUDA_INCLUDE_DIRS}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
    ${IMGUI_FILE_DIALOG_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${TENSORRT_INCLUDE_DIR}
)

# --- Link Libraries ---
target_link_libraries(redgui PRIVATE
    crimson_frame_contracts
    crimson_runtime_contracts
    crimson_overlay_contracts
    crimson_repository_contracts
    crimson_tensorstore_repositories
    crimson_workspace_contracts
    # CUDA and NVIDIA Libraries
    ${CUDA_LIBRARIES}
    ${CUDA_DRIVER}
    ${NVCUVID_LIBRARY}
    ${NPPC_LIBRARY}
    ${NPPIG_LIBRARY}
    ${NPPICC_LIBRARY}
    ${NPPIDEI_LIBRARY}
    ${NPPIAL_LIBRARY}

    # OpenGL Libraries
    OpenGL::GL
    OpenGL::GLU
    GLEW::GLEW
    glfw

    # TensorStore for Zarr support
    tensorstore::all_drivers

    # FFmpeg Libraries
    ${FFMPEG_LIBRARIES}

    # OpenCV Libraries
    ${OpenCV_LIBS}

    # HDF5 Libraries
    ${HDF5_CXX_LIBRARIES}

    # System Libraries
    Threads::Threads
)
crimson_configure_parquet_support(redgui)

if(CRIMSON_ENABLE_NVENC)
    target_link_libraries(redgui PRIVATE ${NVENC_LIBRARY})
    target_compile_definitions(redgui PRIVATE CRIMSON_ENABLE_NVENC=1)
else()
    target_compile_definitions(redgui PRIVATE CRIMSON_ENABLE_NVENC=0)
endif()

if(WIN32)
    target_link_libraries(redgui PRIVATE Dbghelp)
endif()

# Add NVML if enabled
if(ENABLE_NVML)
    if(NVML_LIBRARY)
        target_link_libraries(redgui PRIVATE ${NVML_LIBRARY})
        target_compile_definitions(redgui PRIVATE USE_NVML)
        if(NVML_INCLUDE_DIR)
            target_include_directories(redgui PRIVATE ${NVML_INCLUDE_DIR})
        endif()
    else()
        message(WARNING
            "NVML requested but not found. "
            "Set -DENABLE_NVML=OFF to build without it, "
            "or install headers (libnvidia-ml-dev) and ensure libnvidia-ml.so[.1] is present.")
    endif()
endif()

# Add TensorRT if found
if(TENSORRT_FOUND)
    target_link_libraries(redgui PRIVATE ${NVINFER_LIBRARY} ${NVINFER_PLUGIN_LIBRARY})
    target_compile_definitions(redgui PRIVATE USE_TENSORRT)
endif()

if(CRIMSON_ENABLE_SFM)
    target_compile_definitions(redgui PRIVATE CRIMSON_ENABLE_SFM=1)
else()
    target_compile_definitions(redgui PRIVATE CRIMSON_ENABLE_SFM=0)
endif()

# --- Post-Build Commands ---
# Copy font files if they exist
set(FONT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fonts")
set(FONT_FILES
    "Roboto-Regular.ttf"
    "fa-solid-900.ttf"
    "fontawesome-webfont.ttf"
    "forkawesome-webfont.ttf"
)

foreach(FONT_FILE ${FONT_FILES})
    if(EXISTS ${FONT_SOURCE_DIR}/${FONT_FILE})
        add_custom_command(TARGET redgui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:redgui>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${FONT_SOURCE_DIR}/${FONT_FILE}"
                "$<TARGET_FILE_DIR:redgui>/fonts/${FONT_FILE}"
        )
    endif()
endforeach()

# --- Install Rules ---
set(CRIMSON_INSTALL_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    install(TARGETS redgui
        RUNTIME DESTINATION ${CRIMSON_INSTALL_RUNTIME_DESTINATION}
        COMPONENT core
    )

    install(PROGRAMS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/install_crimson.ps1
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/install_crimson.cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_crimson_runtime.ps1
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_crimson_runtime.cmd
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/set_crimson_cuda_device.ps1
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/set_crimson_cuda_device.cmd
        DESTINATION .
        COMPONENT core
    )
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/windows_app_README.txt
        DESTINATION .
        RENAME README.txt
        COMPONENT core
    )

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CRIMSON_INSTALL_RUNTIME_DESTINATION})
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT core)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            DESTINATION ${CRIMSON_INSTALL_RUNTIME_DESTINATION}
            COMPONENT core
        )
    endif()

    set(CRIMSON_WINDOWS_IGNORED_UNRESOLVED_RUNTIME_DLLS
        "AzureAttestManager.dll"
        "AzureAttestNormal.dll"
        "DeclaredConfiguration.dll"
        "dmenterprisediagnostics.dll"
        "EDPCSP.dll"
        "EFSCORE.dll"
        "FVESKYBACKUP.dll"
        "HwReqChk.dll"
        "HvsiFileTrust.dll"
        "LSASRV.dll"
        "ngcrecovery.dll"
        "PdmUtilities.dll"
        "policymanagerprecheck.dll"
        "SAMSRV.dll"
        "VCRUNTIME140_1.dll"
        "wpaxholder.dll"
    )
    set(_crimson_windows_runtime_dep_script
        "${CMAKE_CURRENT_BINARY_DIR}/install_windows_runtime_deps_$<CONFIG>.cmake")
    set(_crimson_windows_runtime_dep_script_content [=[
set(_crimson_runtime_executable "$<TARGET_FILE:redgui>")
set(_crimson_runtime_destination "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/@CRIMSON_INSTALL_RUNTIME_DESTINATION@")
set(_crimson_runtime_dependency_search_dirs "@CRIMSON_RUNTIME_DEPENDENCY_SEARCH_DIRS@")
set(_crimson_ignored_unresolved_runtime_dlls "@CRIMSON_WINDOWS_IGNORED_UNRESOLVED_RUNTIME_DLLS@")

file(GET_RUNTIME_DEPENDENCIES
    RESOLVED_DEPENDENCIES_VAR _crimson_resolved_runtime_deps
    UNRESOLVED_DEPENDENCIES_VAR _crimson_unresolved_runtime_deps
    EXECUTABLES "${_crimson_runtime_executable}"
    DIRECTORIES ${_crimson_runtime_dependency_search_dirs}
    PRE_EXCLUDE_REGEXES
        "api-ms-win-.*"
        "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*[Ww]indows[\\/](System32|SysWOW64)[\\/].*"
)

set(_crimson_remaining_unresolved_runtime_deps "")
foreach(_crimson_runtime_dep IN LISTS _crimson_unresolved_runtime_deps)
    get_filename_component(_crimson_runtime_dep_name "${_crimson_runtime_dep}" NAME)
    list(FIND _crimson_ignored_unresolved_runtime_dlls "${_crimson_runtime_dep_name}" _crimson_ignored_runtime_dep_index)
    if(_crimson_ignored_runtime_dep_index EQUAL -1)
        list(APPEND _crimson_remaining_unresolved_runtime_deps "${_crimson_runtime_dep_name}")
    endif()
endforeach()

if(_crimson_remaining_unresolved_runtime_deps)
    list(REMOVE_DUPLICATES _crimson_remaining_unresolved_runtime_deps)
    string(REPLACE ";" "\n  " _crimson_unresolved_runtime_dep_message "${_crimson_remaining_unresolved_runtime_deps}")
    message(FATAL_ERROR "Could not resolve runtime dependencies:\n  ${_crimson_unresolved_runtime_dep_message}")
endif()

if(_crimson_resolved_runtime_deps)
    list(REMOVE_DUPLICATES _crimson_resolved_runtime_deps)
    file(INSTALL
        DESTINATION "${_crimson_runtime_destination}"
        TYPE FILE
        FOLLOW_SYMLINK_CHAIN
        FILES ${_crimson_resolved_runtime_deps}
    )
endif()
]=])
    string(CONFIGURE "${_crimson_windows_runtime_dep_script_content}"
        _crimson_windows_runtime_dep_script_content @ONLY)
    file(GENERATE
        OUTPUT "${_crimson_windows_runtime_dep_script}"
        CONTENT "${_crimson_windows_runtime_dep_script_content}"
    )
    install(SCRIPT "${_crimson_windows_runtime_dep_script}" COMPONENT core)
else()
    install(TARGETS redgui
        RUNTIME DESTINATION ${CRIMSON_INSTALL_RUNTIME_DESTINATION}
        COMPONENT core
    )

    if(UNIX AND NOT APPLE)
        install(PROGRAMS
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_crimson_runtime.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/install_crimson.sh
            DESTINATION .
            COMPONENT core
        )
        install(PROGRAMS
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/crimson_linux_launcher.sh
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME crimson
            COMPONENT core
        )
        install(FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/linux_app_README.txt
            DESTINATION .
            RENAME README.txt
            COMPONENT core
        )
    endif()
endif()

install(DIRECTORY ${FONT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/crimson/fonts
    COMPONENT core
    FILES_MATCHING PATTERN "*.ttf"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/crimson/config
    COMPONENT core
    FILES_MATCHING PATTERN "*.json"
)

# --- Print Configuration Summary ---
message(STATUS "========================================")
message(STATUS "Configuration Summary:")
message(STATUS "  CUDA Version: ${CUDA_VERSION}")
message(STATUS "  CUDA Toolkit Root: ${CUDA_TOOLKIT_ROOT_DIR}")
message(STATUS "  CUDA Architecture: ${CMAKE_CUDA_ARCHITECTURES}")
message(STATUS "  OpenCV Version: ${OpenCV_VERSION}")
message(STATUS "  OpenCV Dir: ${OpenCV_DIR}")
message(STATUS "  Expected OpenCV Version: ${CRIMSON_EXPECT_OPENCV_VERSION}")
message(STATUS "  OpenCV SFM Triangulation: ${CRIMSON_ENABLE_SFM}")
message(STATUS "  Arrow/Parquet Clipped Maps: ${CRIMSON_ENABLE_PARQUET}")
message(STATUS "  NVENC Video Export Support: ${CRIMSON_ENABLE_NVENC}")
message(STATUS "  FFmpeg Directory: ${FFMPEG_ROOT}")
message(STATUS "  Video Codec SDK Root: ${VIDEO_CODEC_SDK_ROOT}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Install Data Dir: ${CMAKE_INSTALL_FULL_DATADIR}/crimson")
if(TENSORRT_FOUND)
    message(STATUS "  TensorRT Version: ${TENSORRT_VERSION}")
    if(TENSORRT_ROOT)
        message(STATUS "  TensorRT: FOUND at ${TENSORRT_ROOT}")
    else()
        message(STATUS "  TensorRT: FOUND via default search paths")
    endif()
else()
    message(STATUS "  TensorRT: NOT FOUND")
endif()
message(STATUS "  Expected CUDA Version: ${CRIMSON_EXPECT_CUDA_VERSION}")
message(STATUS "  Expected TensorRT Version: ${CRIMSON_EXPECT_TENSORRT_VERSION}")
if(ENABLE_NVML)
    if(NVML_LIBRARY)
        message(STATUS "  NVML: FOUND -> ${NVML_LIBRARY}")
    else()
        message(STATUS "  NVML: NOT FOUND (set -DENABLE_NVML=OFF to skip)")
    endif()
endif()
message(STATUS "  Output Directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "========================================")

# ============================================================================
# Diagnostics and tooling
# ============================================================================

add_executable(debug_detect_run tools/debug_detect_run.cpp)
target_link_libraries(debug_detect_run PRIVATE tensorstore::all_drivers)

add_executable(palette_clipped_loader_probe
    tools/palette_clipped_loader_probe.cpp
    src/zarr_loader.cpp
    src/zarr_loader_detections.cpp
    src/zarr_loader_eye_keypoint.cpp
    src/zarr_loader_movement.cpp
    src/zarr_loader_stimulus.cpp
    src/zarr_loader_subject_shape.cpp
    src/zarr_loader_tail_kinematics.cpp
    src/zarr_loader_write.cpp
    src/zarr_loader_internal.cpp
    src/keypoint_heading_utils.cpp
    src/zarr/clipped_detection_repository.cpp
    src/zarr/palette_clipped_resolver.cpp
)
target_include_directories(palette_clipped_loader_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
)
target_link_libraries(palette_clipped_loader_probe PRIVATE
    crimson_repository_contracts
    crimson_tensorstore_repositories
    tensorstore::all_drivers
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${OpenCV_LIBS}
    ${HDF5_CXX_LIBRARIES}
)
target_compile_features(palette_clipped_loader_probe PRIVATE cxx_std_17)
crimson_configure_parquet_support(palette_clipped_loader_probe)

add_executable(chaser_distance_polar_legacy_probe
    tools/chaser_distance_polar_legacy_probe.cpp
    src/zarr_loader.cpp
    src/zarr_loader_detections.cpp
    src/zarr_loader_eye_keypoint.cpp
    src/zarr_loader_movement.cpp
    src/zarr_loader_stimulus.cpp
    src/zarr_loader_subject_shape.cpp
    src/zarr_loader_tail_kinematics.cpp
    src/zarr_loader_write.cpp
    src/zarr_loader_internal.cpp
    src/keypoint_heading_utils.cpp
    src/zarr/clipped_detection_repository.cpp
    src/zarr/palette_clipped_resolver.cpp
)
target_include_directories(chaser_distance_polar_legacy_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
)
target_link_libraries(chaser_distance_polar_legacy_probe PRIVATE
    crimson_repository_contracts
    crimson_tensorstore_repositories
    tensorstore::all_drivers
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${OpenCV_LIBS}
    ${HDF5_CXX_LIBRARIES}
)
target_compile_features(chaser_distance_polar_legacy_probe PRIVATE cxx_std_17)
crimson_configure_parquet_support(chaser_distance_polar_legacy_probe)
add_test(NAME chaser_distance_polar_legacy_characterization
    COMMAND $<TARGET_FILE:chaser_distance_polar_legacy_probe> --self-test
)
set_tests_properties(chaser_distance_polar_legacy_characterization PROPERTIES
    LABELS "characterization;headless;legacy;polar;zarr"
    TIMEOUT 120
)

add_executable(stimulus_context_timeline_legacy_probe
    tools/stimulus_context_timeline_legacy_probe.cpp
    src/zarr_loader.cpp
    src/zarr_loader_detections.cpp
    src/zarr_loader_eye_keypoint.cpp
    src/zarr_loader_movement.cpp
    src/zarr_loader_stimulus.cpp
    src/zarr_loader_subject_shape.cpp
    src/zarr_loader_tail_kinematics.cpp
    src/zarr_loader_write.cpp
    src/zarr_loader_internal.cpp
    src/keypoint_heading_utils.cpp
    src/zarr/clipped_detection_repository.cpp
    src/zarr/palette_clipped_resolver.cpp
)
target_include_directories(stimulus_context_timeline_legacy_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
)
target_link_libraries(stimulus_context_timeline_legacy_probe PRIVATE
    crimson_repository_contracts
    crimson_tensorstore_repositories
    tensorstore::all_drivers
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${OpenCV_LIBS}
    ${HDF5_CXX_LIBRARIES}
)
target_compile_features(stimulus_context_timeline_legacy_probe PRIVATE
    cxx_std_17)
crimson_configure_parquet_support(stimulus_context_timeline_legacy_probe)
add_test(NAME stimulus_context_timeline_legacy_characterization
    COMMAND $<TARGET_FILE:stimulus_context_timeline_legacy_probe> --self-test
)
set_tests_properties(stimulus_context_timeline_legacy_characterization
    PROPERTIES
        LABELS "characterization;headless;legacy;stimulus;timeline;zarr"
        TIMEOUT 120
)

add_executable(subject_mask_contour_probe EXCLUDE_FROM_ALL
    tools/subject_mask_contour_probe.cpp
    src/zarr_loader.cpp
    src/zarr_loader_detections.cpp
    src/zarr_loader_eye_keypoint.cpp
    src/zarr_loader_movement.cpp
    src/zarr_loader_stimulus.cpp
    src/zarr_loader_subject_shape.cpp
    src/zarr_loader_tail_kinematics.cpp
    src/zarr_loader_write.cpp
    src/zarr_loader_internal.cpp
    src/keypoint_heading_utils.cpp
    src/zarr/clipped_detection_repository.cpp
    src/zarr/palette_clipped_resolver.cpp
)
target_include_directories(subject_mask_contour_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
)
target_link_libraries(subject_mask_contour_probe PRIVATE
    crimson_repository_contracts
    tensorstore::all_drivers
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${OpenCV_LIBS}
    ${HDF5_CXX_LIBRARIES}
)
target_compile_features(subject_mask_contour_probe PRIVATE cxx_std_17)
crimson_configure_parquet_support(subject_mask_contour_probe)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/keypoint_fish_present_contract_check.cpp")
    add_executable(keypoint_fish_present_contract_check
        tools/keypoint_fish_present_contract_check.cpp
        src/zarr_loader.cpp
        src/zarr_loader_detections.cpp
        src/zarr_loader_eye_masks.cpp
        src/zarr_loader_eye_keypoint.cpp
        src/zarr_loader_movement.cpp
        src/zarr_loader_stimulus.cpp
        src/zarr_loader_write.cpp
        src/zarr_loader_keypoint_write.cpp
        src/zarr_loader_internal.cpp
        src/zarr/clipped_detection_repository.cpp
        src/zarr/palette_clipped_resolver.cpp
    )
    target_include_directories(keypoint_fish_present_contract_check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(keypoint_fish_present_contract_check PRIVATE
        crimson_repository_contracts
        tensorstore::all_drivers
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${OpenCV_LIBS}
        ${HDF5_CXX_LIBRARIES}
    )
    target_compile_features(keypoint_fish_present_contract_check PRIVATE cxx_std_17)
    crimson_configure_parquet_support(keypoint_fish_present_contract_check)
else()
    message(STATUS
        "Skipping keypoint_fish_present_contract_check: "
        "tools/keypoint_fish_present_contract_check.cpp is not present")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/keypoint_plot_debug_dump.cpp")
    add_executable(keypoint_plot_debug_dump
        tools/keypoint_plot_debug_dump.cpp
        src/zarr_loader.cpp
        src/zarr_loader_detections.cpp
        src/zarr_loader_eye_masks.cpp
        src/zarr_loader_eye_keypoint.cpp
        src/zarr_loader_movement.cpp
        src/zarr_loader_stimulus.cpp
        src/zarr_loader_write.cpp
        src/zarr_loader_internal.cpp
        src/zarr/clipped_detection_repository.cpp
        src/zarr/palette_clipped_resolver.cpp
    )
    target_include_directories(keypoint_plot_debug_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(keypoint_plot_debug_dump PRIVATE
        crimson_repository_contracts
        tensorstore::all_drivers
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${OpenCV_LIBS}
        ${HDF5_CXX_LIBRARIES}
    )
    target_compile_features(keypoint_plot_debug_dump PRIVATE cxx_std_17)
    crimson_configure_parquet_support(keypoint_plot_debug_dump)
else()
    message(STATUS
        "Skipping keypoint_plot_debug_dump: "
        "tools/keypoint_plot_debug_dump.cpp is not present")
endif()

if(CRIMSON_INSTALL_DEV_TOOLS)
    install(TARGETS
        test_zarr3_read
        zarr_calibration_probe
        debug_detect_run
        keypoint_fish_present_contract_check
        keypoint_plot_debug_dump
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/crimson
        COMPONENT dev_tools
    )

    if(TARGET ts_zarr3_check)
        install(TARGETS ts_zarr3_check
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/crimson
            COMPONENT dev_tools
        )
    endif()
endif()

endif() # CRIMSON_BUILD_NVIDIA_APP

if(CRIMSON_BUILD_MACOS_APP)
    enable_language(OBJCXX)
    message(STATUS "Configuring TensorStore for macOS Zarr support...")
    crimson_fetch_tensorstore()
    crimson_add_tensorstore_repositories(crimson::tensorstore_zarr)

    add_library(crimson_apple_analysis_loading STATIC
        src/platform/macos/apple_analysis_repository_loader.cpp
    )
    target_compile_features(crimson_apple_analysis_loading PUBLIC cxx_std_17)
    target_include_directories(crimson_apple_analysis_loading PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(crimson_apple_analysis_loading PUBLIC
        crimson_runtime_contracts
        crimson_tensorstore_repositories
        crimson::tensorstore_zarr
        Threads::Threads
    )

    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR
            "The maintained macOS backend requires native Apple Silicon arm64.")
    endif()

    set(CRIMSON_MACOS_IMGUI_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    set(CRIMSON_MACOS_IMPLOT_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot")
    if(NOT EXISTS "${CRIMSON_MACOS_IMGUI_DIR}/imgui.cpp")
        message(FATAL_ERROR
            "Dear ImGui is not initialized at ${CRIMSON_MACOS_IMGUI_DIR}. "
            "Run: git submodule update --init --recursive")
    endif()
    if(NOT EXISTS "${CRIMSON_MACOS_IMPLOT_DIR}/implot.cpp")
        message(FATAL_ERROR
            "ImPlot is not initialized at ${CRIMSON_MACOS_IMPLOT_DIR}. "
            "Run: git submodule update --init --recursive")
    endif()

    find_package(glfw3 CONFIG REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
    find_library(COREIMAGE_FRAMEWORK CoreImage REQUIRED)
    find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)

    add_library(crimson_apple_workspace STATIC
        src/platform/macos/apple_workspace_layout.cpp
    )
    target_compile_features(crimson_apple_workspace PUBLIC cxx_std_17)
    target_include_directories(crimson_apple_workspace PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    add_library(crimson_apple_video STATIC
        src/platform/macos/apple_video_frame_provider.mm
        src/platform/macos/apple_video_playback_buffer.cpp
        src/platform/macos/apple_video_metal_renderer.mm
    )
    target_compile_features(crimson_apple_video PRIVATE cxx_std_17)
    target_compile_options(crimson_apple_video PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_include_directories(crimson_apple_video PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(crimson_apple_video PUBLIC
        crimson_frame_contracts
        crimson_recording_contracts
        ${AVFOUNDATION_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${FOUNDATION_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${VIDEOTOOLBOX_FRAMEWORK}
    )

    add_library(crimson_apple_overlay STATIC
        src/platform/macos/apple_overlay_metal_renderer.mm
    )
    target_compile_features(crimson_apple_overlay PRIVATE cxx_std_17)
    target_compile_options(crimson_apple_overlay PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_include_directories(crimson_apple_overlay PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(crimson_apple_overlay PUBLIC
        crimson_overlay_contracts
        crimson_repository_contracts
        ${FOUNDATION_FRAMEWORK}
        ${METAL_FRAMEWORK}
    )

    add_library(crimson_apple_stimulus_playback STATIC
        src/platform/macos/apple_stimulus_playback_session.cpp
    )
    target_compile_features(crimson_apple_stimulus_playback PRIVATE cxx_std_17)
    target_include_directories(crimson_apple_stimulus_playback PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(crimson_apple_stimulus_playback PUBLIC
        crimson_apple_video
        crimson_stimulus_playback_contracts
    )

    add_library(crimson_apple_acquisition_crop_playback STATIC
        src/platform/macos/apple_acquisition_crop_playback_session.cpp
    )
    target_compile_features(crimson_apple_acquisition_crop_playback
        PRIVATE cxx_std_17)
    target_include_directories(crimson_apple_acquisition_crop_playback PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(crimson_apple_acquisition_crop_playback PUBLIC
        crimson_apple_video
        crimson_crop_contracts
    )

    add_executable(apple_stimulus_playback_probe
        tools/apple_stimulus_playback_probe.cpp
    )
    target_compile_features(apple_stimulus_playback_probe PRIVATE cxx_std_17)
    target_link_libraries(apple_stimulus_playback_probe PRIVATE
        crimson_tensorstore_repositories
        crimson_apple_stimulus_playback
        crimson::tensorstore_zarr
    )
    set_target_properties(apple_stimulus_playback_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(apple_acquisition_crop_playback_probe
        tools/apple_acquisition_crop_playback_probe.cpp
    )
    target_compile_features(apple_acquisition_crop_playback_probe
        PRIVATE cxx_std_17)
    target_link_libraries(apple_acquisition_crop_playback_probe PRIVATE
        crimson_apple_acquisition_crop_playback
        crimson_tensorstore_repositories
        crimson::tensorstore_zarr
    )
    set_target_properties(apple_acquisition_crop_playback_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CRIMSON_MACOS_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT CRIMSON_MACOS_GIT_COMMIT)
        set(CRIMSON_MACOS_GIT_COMMIT "unknown")
    endif()
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CRIMSON_MACOS_GIT_COMMIT_FULL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE CRIMSON_MACOS_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT CRIMSON_MACOS_GIT_COMMIT_FULL)
        set(CRIMSON_MACOS_GIT_COMMIT_FULL "unknown")
    endif()
    if(CRIMSON_MACOS_GIT_STATUS)
        set(CRIMSON_MACOS_WORKTREE_DIRTY 1)
    else()
        set(CRIMSON_MACOS_WORKTREE_DIRTY 0)
    endif()

    set(CRIMSON_MACOS_APP_SOURCES
        src/platform/macos/crimson_macos_main.mm
        src/platform/macos/apple_video_viewer_ui.mm
        src/gui/camera_view_transport_controls.cpp
        src/gui/canonical_detection_inspect_adapter.cpp
        src/gui/eye_geometry_overlay_controls.cpp
        src/gui/eye_geometry_overlay_inspect_adapter.cpp
        src/gui/frame_inspect_detection_module.cpp
        src/gui/frame_inspect_eye_angle_module.cpp
        src/gui/frame_inspect_keypoint_module.cpp
        src/gui/frame_inspect_subject_mask_module.cpp
        src/gui/frame_inspect_subject_shape_module.cpp
        src/gui/frame_inspect_window.cpp
        src/gui/keypoint_overlay_inspect_adapter.cpp
        src/gui/quality_timeline_session.cpp
        src/gui/quality_timeline_window.cpp
        src/gui/read_only_eye_geometry_controls_adapter.cpp
        src/gui/read_only_subject_mask_controls_adapter.cpp
        src/gui/read_only_subject_shape_controls_adapter.cpp
        src/gui/session_loading_modal.cpp
        src/gui/subject_mask_overlay_inspect_adapter.cpp
        src/gui/subject_mask_overlay_controls.cpp
        src/gui/subject_shape_overlay_controls.cpp
        src/gui/subject_shape_overlay_inspect_adapter.cpp
        src/imgui_semantic_snapshot.cpp
        src/ui_path_config.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_draw.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_tables.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_widgets.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/backends/imgui_impl_metal.mm
        ${CRIMSON_MACOS_IMPLOT_DIR}/implot.cpp
        ${CRIMSON_MACOS_IMPLOT_DIR}/implot_items.cpp
    )
    set(CRIMSON_MACOS_APP_RESOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/Roboto-Regular.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/forkawesome-webfont.ttf
    )
    set_source_files_properties(${CRIMSON_MACOS_APP_RESOURCES}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fonts")

    add_executable(crimson_macos_shell MACOSX_BUNDLE
        ${CRIMSON_MACOS_APP_SOURCES}
        ${CRIMSON_MACOS_APP_RESOURCES}
    )
    target_compile_features(crimson_macos_shell PRIVATE cxx_std_17)
    target_compile_options(crimson_macos_shell PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(crimson_macos_shell PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_MACOS_GIT_COMMIT}"
        IMGUI_ENABLE_TEST_ENGINE
    )
    target_include_directories(crimson_macos_shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CRIMSON_MACOS_IMGUI_DIR}
        ${CRIMSON_MACOS_IMGUI_DIR}/backends
        ${CRIMSON_MACOS_IMPLOT_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/IconFontCppHeaders
    )
    target_link_libraries(crimson_macos_shell PRIVATE
        crimson_apple_analysis_loading
        crimson_apple_workspace
        crimson_apple_overlay
        crimson_apple_acquisition_crop_playback
        crimson_apple_stimulus_playback
        crimson_tensorstore_repositories
        crimson_workspace_contracts
        crimson::tensorstore_zarr
        nlohmann_json::nlohmann_json
        glfw
        ${COCOA_FRAMEWORK}
        ${COREIMAGE_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
    )

    add_executable(imgui_semantic_snapshot_tests
        tools/imgui_semantic_snapshot_tests.cpp
        src/gui/camera_view_transport_controls.cpp
        src/gui/canonical_detection_inspect_adapter.cpp
        src/gui/eye_geometry_overlay_controls.cpp
        src/gui/eye_geometry_overlay_inspect_adapter.cpp
        src/gui/frame_inspect_detection_module.cpp
        src/gui/frame_inspect_eye_angle_module.cpp
        src/gui/frame_inspect_keypoint_module.cpp
        src/gui/frame_inspect_subject_mask_module.cpp
        src/gui/frame_inspect_subject_shape_module.cpp
        src/gui/frame_inspect_window.cpp
        src/gui/keypoint_overlay_inspect_adapter.cpp
        src/gui/read_only_eye_geometry_controls_adapter.cpp
        src/gui/read_only_subject_mask_controls_adapter.cpp
        src/gui/read_only_subject_shape_controls_adapter.cpp
        src/gui/session_loading_modal.cpp
        src/gui/subject_mask_overlay_inspect_adapter.cpp
        src/gui/subject_mask_overlay_controls.cpp
        src/gui/subject_shape_overlay_controls.cpp
        src/gui/subject_shape_overlay_inspect_adapter.cpp
        src/imgui_semantic_snapshot.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_draw.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_tables.cpp
        ${CRIMSON_MACOS_IMGUI_DIR}/imgui_widgets.cpp
    )
    target_compile_features(imgui_semantic_snapshot_tests PRIVATE cxx_std_17)
    target_compile_definitions(imgui_semantic_snapshot_tests PRIVATE
        IMGUI_ENABLE_TEST_ENGINE)
    target_include_directories(imgui_semantic_snapshot_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CRIMSON_MACOS_IMGUI_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/IconFontCppHeaders
    )
    target_link_libraries(imgui_semantic_snapshot_tests PRIVATE
        crimson_frame_contracts
        crimson_runtime_contracts
        crimson_workspace_contracts
        nlohmann_json::nlohmann_json)
    add_test(NAME imgui_semantic_snapshot_tests
        COMMAND imgui_semantic_snapshot_tests)
    set_tests_properties(imgui_semantic_snapshot_tests PROPERTIES
        LABELS "headless;macos;ui"
    )
    set_target_properties(crimson_macos_shell PROPERTIES
        OUTPUT_NAME "Crimson"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        MACOSX_BUNDLE_BUNDLE_NAME "Crimson"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.hhmi.crimson"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    )

    enable_testing()
    add_executable(apple_workspace_layout_tests
        tools/apple_workspace_layout_tests.cpp
    )
    target_compile_features(apple_workspace_layout_tests PRIVATE cxx_std_17)
    target_link_libraries(apple_workspace_layout_tests PRIVATE
        crimson_apple_workspace
    )
    set_target_properties(apple_workspace_layout_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_workspace_layout_tests
        COMMAND $<TARGET_FILE:apple_workspace_layout_tests>)
    set_tests_properties(apple_workspace_layout_tests PROPERTIES
        LABELS "headless;macos;workspace"
    )

    add_executable(apple_analysis_repository_loader_tests
        tools/apple_analysis_repository_loader_tests.cpp
    )
    target_compile_features(apple_analysis_repository_loader_tests
        PRIVATE cxx_std_17)
    target_link_libraries(apple_analysis_repository_loader_tests PRIVATE
        crimson_apple_analysis_loading
    )
    set_target_properties(apple_analysis_repository_loader_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_analysis_repository_loader_tests
        COMMAND $<TARGET_FILE:apple_analysis_repository_loader_tests>)
    set_tests_properties(apple_analysis_repository_loader_tests PROPERTIES
        LABELS "headless;macos;repository;startup;tensorstore;zarr"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(canonical_detection_full_archive_integration_gate
        tools/canonical_detection_full_archive_integration_gate.cpp
    )
    target_compile_features(canonical_detection_full_archive_integration_gate
        PRIVATE cxx_std_17)
    target_link_libraries(canonical_detection_full_archive_integration_gate
        PRIVATE
            crimson_apple_analysis_loading
            crimson_overlay_contracts
            nlohmann_json::nlohmann_json
    )
    set_target_properties(canonical_detection_full_archive_integration_gate
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(canonical_detection_full_archive_stage1_benchmark
        tools/canonical_detection_full_archive_stage1_benchmark.cpp
    )
    target_compile_features(canonical_detection_full_archive_stage1_benchmark
        PRIVATE cxx_std_17)
    target_link_libraries(canonical_detection_full_archive_stage1_benchmark
        PRIVATE
            crimson_apple_analysis_loading
            crimson_overlay_contracts
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(
        canonical_detection_full_archive_stage1_benchmark PRIVATE
        CRIMSON_GIT_COMMIT="${CRIMSON_MACOS_GIT_COMMIT_FULL}"
        CRIMSON_WORKTREE_DIRTY=${CRIMSON_MACOS_WORKTREE_DIRTY}
    )
    set_target_properties(canonical_detection_full_archive_stage1_benchmark
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(apple_tensorstore_zarr_tests
        tools/apple_tensorstore_zarr_tests.cpp
    )
    target_compile_features(apple_tensorstore_zarr_tests PRIVATE cxx_std_17)
    target_link_libraries(apple_tensorstore_zarr_tests PRIVATE
        crimson::tensorstore_zarr
        Threads::Threads
    )
    set_target_properties(apple_tensorstore_zarr_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_tensorstore_zarr_tests
        COMMAND $<TARGET_FILE:apple_tensorstore_zarr_tests>)
    set_tests_properties(apple_tensorstore_zarr_tests PROPERTIES
        LABELS "headless;macos;tensorstore;zarr"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(stimulus_repository_tests
        tools/stimulus_repository_tests.cpp
    )
    target_compile_features(stimulus_repository_tests PRIVATE cxx_std_17)
    target_compile_definitions(stimulus_repository_tests PRIVATE
        CRIMSON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(stimulus_repository_tests PRIVATE
        crimson_tensorstore_repositories
        crimson::tensorstore_zarr
    )
    set_target_properties(stimulus_repository_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME stimulus_repository_tests
        COMMAND $<TARGET_FILE:stimulus_repository_tests>)
    set_tests_properties(stimulus_repository_tests PROPERTIES
        LABELS "headless;macos;tensorstore;zarr;repository;stimulus"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_video_provider_tests
        tools/apple_video_provider_tests.mm
    )
    target_compile_features(apple_video_provider_tests PRIVATE cxx_std_17)
    target_compile_options(apple_video_provider_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(apple_video_provider_tests PRIVATE
        CRIMSON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(apple_video_provider_tests PRIVATE
        crimson_apple_video
        nlohmann_json::nlohmann_json
    )
    set_target_properties(apple_video_provider_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_video_provider_tests
        COMMAND $<TARGET_FILE:apple_video_provider_tests>)
    set_tests_properties(apple_video_provider_tests PROPERTIES
        LABELS "headless;macos;video"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_stimulus_playback_tests
        tools/apple_stimulus_playback_tests.mm
    )
    target_compile_features(apple_stimulus_playback_tests PRIVATE cxx_std_17)
    target_compile_options(apple_stimulus_playback_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_stimulus_playback_tests PRIVATE
        crimson_apple_stimulus_playback
    )
    set_target_properties(apple_stimulus_playback_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(apple_stimulus_playback_tests apple_video_provider_tests)
    set(CRIMSON_STIMULUS_VIDEO_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/apple_stimulus_playback_fixture.mov")
    add_test(NAME apple_stimulus_playback_tests
        COMMAND ${CMAKE_COMMAND}
            -DFIXTURE_WRITER=$<TARGET_FILE:apple_video_provider_tests>
            -DPLAYBACK_TEST=$<TARGET_FILE:apple_stimulus_playback_tests>
            -DVIDEO_FIXTURE=${CRIMSON_STIMULUS_VIDEO_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAppleStimulusPlaybackTest.cmake)
    set_tests_properties(apple_stimulus_playback_tests PROPERTIES
        LABELS "headless;macos;playback;repository;stimulus;video"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_acquisition_crop_playback_tests
        tools/apple_acquisition_crop_playback_tests.mm
    )
    target_compile_features(apple_acquisition_crop_playback_tests
        PRIVATE cxx_std_17)
    target_compile_options(apple_acquisition_crop_playback_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_acquisition_crop_playback_tests PRIVATE
        crimson_apple_acquisition_crop_playback
    )
    set_target_properties(apple_acquisition_crop_playback_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(apple_acquisition_crop_playback_tests
        apple_video_provider_tests)
    set(CRIMSON_ACQUISITION_CROP_VIDEO_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/apple_acquisition_crop_playback_fixture.mov")
    add_test(NAME apple_acquisition_crop_playback_tests
        COMMAND ${CMAKE_COMMAND}
            -DFIXTURE_WRITER=$<TARGET_FILE:apple_video_provider_tests>
            -DPLAYBACK_TEST=$<TARGET_FILE:apple_acquisition_crop_playback_tests>
            -DVIDEO_FIXTURE=${CRIMSON_ACQUISITION_CROP_VIDEO_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAppleStimulusPlaybackTest.cmake)
    set_tests_properties(apple_acquisition_crop_playback_tests PROPERTIES
        LABELS "crop;headless;macos;playback;repository;video"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_stimulus_composite_metal_tests
        tools/apple_stimulus_composite_metal_tests.mm
    )
    target_compile_features(apple_stimulus_composite_metal_tests PRIVATE cxx_std_17)
    target_compile_options(apple_stimulus_composite_metal_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_stimulus_composite_metal_tests PRIVATE
        crimson_apple_stimulus_playback
    )
    set_target_properties(apple_stimulus_composite_metal_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(apple_stimulus_composite_metal_tests
        apple_video_provider_tests)
    set(CRIMSON_STIMULUS_COMPOSITE_VIDEO_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/apple_stimulus_composite_fixture.mov")
    add_test(NAME apple_stimulus_composite_metal_tests
        COMMAND ${CMAKE_COMMAND}
            -DFIXTURE_WRITER=$<TARGET_FILE:apple_video_provider_tests>
            -DPLAYBACK_TEST=$<TARGET_FILE:apple_stimulus_composite_metal_tests>
            -DVIDEO_FIXTURE=${CRIMSON_STIMULUS_COMPOSITE_VIDEO_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAppleStimulusPlaybackTest.cmake)
    set_tests_properties(apple_stimulus_composite_metal_tests PROPERTIES
        LABELS "composite;headless;macos;metal;playback;stimulus;video"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_crop_presentation_metal_tests
        tools/apple_crop_presentation_metal_tests.mm
    )
    target_compile_features(apple_crop_presentation_metal_tests PRIVATE cxx_std_17)
    target_compile_options(apple_crop_presentation_metal_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_crop_presentation_metal_tests PRIVATE
        crimson_apple_acquisition_crop_playback
    )
    set_target_properties(apple_crop_presentation_metal_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(apple_crop_presentation_metal_tests
        apple_video_provider_tests)
    set(CRIMSON_CROP_PRESENTATION_VIDEO_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/apple_crop_presentation_fixture.mov")
    add_test(NAME apple_crop_presentation_metal_tests
        COMMAND ${CMAKE_COMMAND}
            -DFIXTURE_WRITER=$<TARGET_FILE:apple_video_provider_tests>
            -DPLAYBACK_TEST=$<TARGET_FILE:apple_crop_presentation_metal_tests>
            -DVIDEO_FIXTURE=${CRIMSON_CROP_PRESENTATION_VIDEO_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAppleStimulusPlaybackTest.cmake)
    set_tests_properties(apple_crop_presentation_metal_tests PROPERTIES
        LABELS "composite;crop;headless;macos;metal;playback;video"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_read_only_overlay_metal_tests
        tools/apple_read_only_overlay_metal_tests.mm
    )
    target_compile_features(apple_read_only_overlay_metal_tests
        PRIVATE cxx_std_17)
    target_compile_options(apple_read_only_overlay_metal_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_include_directories(apple_read_only_overlay_metal_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(apple_read_only_overlay_metal_tests PRIVATE
        crimson_apple_overlay
    )
    set_target_properties(apple_read_only_overlay_metal_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_read_only_overlay_metal_tests
        COMMAND $<TARGET_FILE:apple_read_only_overlay_metal_tests>)
    set_tests_properties(apple_read_only_overlay_metal_tests PROPERTIES
        LABELS "headless;macos;metal;overlay;polar;presentation"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_multistream_discontinuity_tests
        tools/apple_multistream_discontinuity_tests.mm
    )
    target_compile_features(apple_multistream_discontinuity_tests
        PRIVATE cxx_std_17)
    target_compile_options(apple_multistream_discontinuity_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_multistream_discontinuity_tests PRIVATE
        crimson_apple_acquisition_crop_playback
        crimson_apple_stimulus_playback
    )
    set_target_properties(apple_multistream_discontinuity_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_dependencies(apple_multistream_discontinuity_tests
        apple_video_provider_tests)
    set(CRIMSON_MULTISTREAM_DISCONTINUITY_VIDEO_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/apple_multistream_discontinuity_fixture.mov")
    add_test(NAME apple_multistream_discontinuity_tests
        COMMAND ${CMAKE_COMMAND}
            -DFIXTURE_WRITER=$<TARGET_FILE:apple_video_provider_tests>
            -DPLAYBACK_TEST=$<TARGET_FILE:apple_multistream_discontinuity_tests>
            -DVIDEO_FIXTURE=${CRIMSON_MULTISTREAM_DISCONTINUITY_VIDEO_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAppleStimulusPlaybackTest.cmake)
    set_tests_properties(apple_multistream_discontinuity_tests PROPERTIES
        LABELS "composite;crop;discontinuity;headless;macos;metal;playback;stimulus;video"
        TIMEOUT 90
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_executable(apple_video_metal_tests
        tools/apple_video_metal_tests.mm
    )
    target_compile_features(apple_video_metal_tests PRIVATE cxx_std_17)
    target_compile_options(apple_video_metal_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(apple_video_metal_tests PRIVATE
        crimson_apple_video
    )
    set_target_properties(apple_video_metal_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(NAME apple_video_metal_tests
        COMMAND $<TARGET_FILE:apple_video_metal_tests>)
    set_tests_properties(apple_video_metal_tests PROPERTIES
        LABELS "headless;macos;metal;video"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME crimson_macos_metal_headless
        COMMAND $<TARGET_FILE:crimson_macos_shell> --validate-metal)
    set_tests_properties(crimson_macos_metal_headless PROPERTIES
        LABELS "headless;macos;metal"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME crimson_macos_shell_smoke
        COMMAND $<TARGET_FILE:crimson_macos_shell> --smoke)
    set_tests_properties(crimson_macos_shell_smoke PROPERTIES
        LABELS "gui;macos;metal"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    install(TARGETS crimson_macos_shell
        BUNDLE DESTINATION .
        COMPONENT core
    )

    message(STATUS "========================================")
    message(STATUS "Crimson macOS Phase 4 Configuration")
    message(STATUS "  Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "  Renderer: Metal")
    message(STATUS "  Window backend: GLFW/Cocoa")
    message(STATUS "  Git commit: ${CRIMSON_MACOS_GIT_COMMIT}")
    message(STATUS "========================================")
endif()
