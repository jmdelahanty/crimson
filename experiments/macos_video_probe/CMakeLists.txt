cmake_minimum_required(VERSION 3.21)
project(crimson_macos_video_probe LANGUAGES CXX OBJCXX)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "This probe requires an Apple Silicon Mac.")
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET
        libavformat libavcodec libavutil)
endif()

find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)

configure_file(src/yuv_to_rgb.metal yuv_to_rgb.metal COPYONLY)

if(FFMPEG_FOUND)
    add_executable(crimson_macos_video_probe src/main.mm)
    target_compile_features(crimson_macos_video_probe PRIVATE cxx_std_17)
    target_compile_options(crimson_macos_video_probe PRIVATE
        -fobjc-arc -Wall -Wextra)
    target_compile_definitions(crimson_macos_video_probe PRIVATE
        CRIMSON_PROBE_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/yuv_to_rgb.metal")
    target_link_libraries(crimson_macos_video_probe PRIVATE
        PkgConfig::FFMPEG
        ${COCOA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${METALKIT_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${VIDEOTOOLBOX_FRAMEWORK})
else()
    message(STATUS "FFmpeg not found; building only the AVFoundation probe")
endif()

add_executable(crimson_avfoundation_metal_probe src/avfoundation_main.mm)
target_compile_features(crimson_avfoundation_metal_probe PRIVATE cxx_std_17)
target_compile_options(crimson_avfoundation_metal_probe PRIVATE
    -fobjc-arc -Wall -Wextra)
target_compile_definitions(crimson_avfoundation_metal_probe PRIVATE
    CRIMSON_PROBE_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/yuv_to_rgb.metal")
target_link_libraries(crimson_avfoundation_metal_probe PRIVATE
    ${AVFOUNDATION_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${COREMEDIA_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${VIDEOTOOLBOX_FRAMEWORK})

add_executable(crimson_avfoundation_feasibility src/avfoundation_feasibility.mm)
target_compile_features(crimson_avfoundation_feasibility PRIVATE cxx_std_17)
target_compile_options(crimson_avfoundation_feasibility PRIVATE
    -fobjc-arc -Wall -Wextra)
target_link_libraries(crimson_avfoundation_feasibility PRIVATE
    ${AVFOUNDATION_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
    ${COREMEDIA_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK})

add_executable(crimson_aligned_playback_probe src/aligned_playback_main.mm)
target_compile_features(crimson_aligned_playback_probe PRIVATE cxx_std_17)
target_compile_options(crimson_aligned_playback_probe PRIVATE
    -fobjc-arc -Wall -Wextra)
target_compile_definitions(crimson_aligned_playback_probe PRIVATE
    CRIMSON_PROBE_SHADER_PATH="${CMAKE_CURRENT_BINARY_DIR}/yuv_to_rgb.metal")
target_link_libraries(crimson_aligned_playback_probe PRIVATE
    ${AVFOUNDATION_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${COREMEDIA_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${VIDEOTOOLBOX_FRAMEWORK})
