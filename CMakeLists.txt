cmake_minimum_required(VERSION 3.18)
project(redgui LANGUAGES CXX CUDA)

include(GNUInstallDirs)

# ----------------------- Build toggles -----------------------
option(ENABLE_NVML "Link against NVIDIA Management Library (NVML)" ON)
option(CRIMSON_INSTALL_DEV_TOOLS "Install diagnostic and developer helper executables" OFF)
option(CRIMSON_ENABLE_SFM "Enable OpenCV SFM-based triangulation workflows" ON)

# Set output directory for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)

# Set C++ standard and options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CRIMSON_STRICT_NAN_COMPILE_OPTION "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(CRIMSON_CXX_FAST_FLAGS -Ofast -ffast-math)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|AMD64|amd64|i[3-6]86)$")
        list(APPEND CRIMSON_CXX_FAST_FLAGS -mssse3)
    endif()
    foreach(CRIMSON_CXX_FAST_FLAG IN LISTS CRIMSON_CXX_FAST_FLAGS)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${CRIMSON_CXX_FAST_FLAG}>)
    endforeach()
    set(CRIMSON_STRICT_NAN_COMPILE_OPTION "-fno-fast-math")
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
find_package(Threads REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS CXX HL)
find_package(nlohmann_json CONFIG REQUIRED)

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

# --- FFmpeg with NVIDIA support ---
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h HINTS ${CRIMSON_FFMPEG_INCLUDE_HINTS})
find_library(AVFORMAT_LIBRARY avformat HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(AVCODEC_LIBRARY avcodec HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(AVUTIL_LIBRARY avutil HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(SWSCALE_LIBRARY swscale HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
find_library(SWRESAMPLE_LIBRARY swresample HINTS ${CRIMSON_FFMPEG_LIBRARY_HINTS})
set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
set(FFMPEG_LIBRARIES ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})

# --- NVIDIA Libraries ---
# NVENC (Video Codec SDK runtime)
find_library(NVENC_LIBRARY
    NAMES nvidia-encode nvencodeapi
    HINTS ${CRIMSON_NV_CODEC_LIBRARY_HINTS}
)

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
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)
set(IMGUI_FILE_DIALOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ImGuiFileDialog)

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

include(FetchContent)

# Set TensorStore options before declaring
set(TENSORSTORE_BUILD_TESTS OFF CACHE BOOL "Build TensorStore tests" FORCE)
set(TENSORSTORE_BUILD_EXAMPLES OFF CACHE BOOL "Build TensorStore examples" FORCE)
set(TENSORSTORE_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "Build Python bindings" FORCE)
set(TENSORSTORE_BUILD_BENCHMARKS OFF CACHE BOOL "Build benchmarks" FORCE)
set(TENSORSTORE_BUILD_DOC OFF CACHE BOOL "Build documentation" FORCE)

# Use a specific release version for stability
FetchContent_Declare(
    tensorstore
    GIT_REPOSITORY https://github.com/google/tensorstore.git
    GIT_TAG        v0.1.64  # Latest stable release as of Jan 2025
    GIT_SHALLOW    TRUE      # Only fetch the specific tag, not full history
    GIT_PROGRESS   TRUE      # Show download progress
)

# This will download and configure TensorStore
FetchContent_MakeAvailable(tensorstore)

# ============================================================================
# TensorStore Zarr3 build/test-time verification
# ============================================================================
option(ENABLE_TENSORSTORE_ZARR3_CHECK "Build and run a small runtime check to verify zarr3 driver support" ON)

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
file(GLOB CPP_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# keypoint write path requires strict IEEE NaN semantics for contract-idempotent
# NaN-aware comparisons and writes.
if(CRIMSON_STRICT_NAN_COMPILE_OPTION)
    set_source_files_properties(
        src/zarr_loader_keypoint_write.cpp
        PROPERTIES COMPILE_OPTIONS "${CRIMSON_STRICT_NAN_COMPILE_OPTION}"
    )
endif()

# --- Create the Executable ---
add_executable(redgui
    ${CPP_SOURCES}
    ${CUDA_SOURCES}
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${IMGUI_FILE_DIALOG_SOURCES}
)

# --- Set Include Directories ---
target_include_directories(redgui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/IconFontCppHeaders
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
    # CUDA and NVIDIA Libraries
    ${CUDA_LIBRARIES}
    ${CUDA_DRIVER}
    ${NVCUVID_LIBRARY}
    ${NVENC_LIBRARY}
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
    ${HDF5_HL_LIBRARIES}

    # System Libraries
    Threads::Threads
)

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

# --- Compile Definitions ---
target_compile_definitions(redgui PRIVATE
    GLEW_STATIC
)

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
install(TARGETS redgui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT core
)

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
    )
    target_include_directories(keypoint_fish_present_contract_check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(keypoint_fish_present_contract_check PRIVATE
        tensorstore::all_drivers
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${OpenCV_LIBS}
        ${HDF5_CXX_LIBRARIES}
        ${HDF5_HL_LIBRARIES}
    )
    target_compile_features(keypoint_fish_present_contract_check PRIVATE cxx_std_17)
    if(CRIMSON_STRICT_NAN_COMPILE_OPTION)
        target_compile_options(keypoint_fish_present_contract_check PRIVATE
            ${CRIMSON_STRICT_NAN_COMPILE_OPTION})
    endif()
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
    )
    target_include_directories(keypoint_plot_debug_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(keypoint_plot_debug_dump PRIVATE
        tensorstore::all_drivers
        nlohmann_json::nlohmann_json
        Threads::Threads
        ${OpenCV_LIBS}
        ${HDF5_CXX_LIBRARIES}
        ${HDF5_HL_LIBRARIES}
    )
    target_compile_features(keypoint_plot_debug_dump PRIVATE cxx_std_17)
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
