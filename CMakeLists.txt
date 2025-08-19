cmake_minimum_required(VERSION 3.18)
project(redgui LANGUAGES CXX CUDA)

# Set output directory for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/release)

# Set C++ standard and options
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast -mssse3 -ffast-math")

# Set CUDA standard and options
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CMAKE_CUDA_ARCHITECTURES "80;86")  # Support both sm_80 and sm_86

# Suppress deprecated FindCUDA warning
cmake_policy(SET CMP0146 OLD)

# --- Set Crimson installation paths ---
set(CRIMSON_ROOT /opt/crimson)
set(OpenCV_DIR ${CRIMSON_ROOT}/lib/opencv/lib/cmake/opencv4)
set(TENSORRT_ROOT /usr/local/TensorRT-10.0.1.6)
set(FFMPEG_ROOT /opt/orange/lib/ffmpeg-nvidia)

# --- Set RPATH for runtime library loading ---
set(CMAKE_INSTALL_RPATH "${CRIMSON_ROOT}/lib/opencv/lib;${TENSORRT_ROOT}/lib;${FFMPEG_ROOT}/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# --- Find All Dependencies ---
find_package(CUDA REQUIRED)
find_package(OpenGL REQUIRED)
find_package(OpenCV REQUIRED)  # Will use OpenCV_DIR set above
find_package(GLEW REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Threads REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS CXX HL)

message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")

# --- FFmpeg with NVIDIA support ---
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h HINTS "${FFMPEG_ROOT}/include")
find_library(AVFORMAT_LIBRARY avformat HINTS "${FFMPEG_ROOT}/lib")
find_library(AVCODEC_LIBRARY avcodec HINTS "${FFMPEG_ROOT}/lib")
find_library(AVUTIL_LIBRARY avutil HINTS "${FFMPEG_ROOT}/lib")
find_library(SWSCALE_LIBRARY swscale HINTS "${FFMPEG_ROOT}/lib")
find_library(SWRESAMPLE_LIBRARY swresample HINTS "${FFMPEG_ROOT}/lib")
set(FFMPEG_INCLUDE_DIRS ${AVFORMAT_INCLUDE_DIR})
set(FFMPEG_LIBRARIES ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})

# --- NVIDIA Libraries ---
find_library(NVENC_LIBRARY nvidia-encode HINTS /usr/lib/x86_64-linux-gnu /usr/lib)
find_path(NVML_INCLUDE_DIR nvml.h HINTS ${CUDA_TOOLKIT_ROOT_DIR}/include /usr/include)
find_library(NVML_LIBRARY NAMES nvidia-ml nvml HINTS /usr/lib/x86_64-linux-gnu /usr/lib64)

# --- CUDA Libraries ---
find_library(CUDA_DRIVER cuda HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)
find_library(NVCUVID_LIBRARY nvcuvid HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)

# NPP Libraries
find_library(NPPC_LIBRARY nppc HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)
find_library(NPPIG_LIBRARY nppig HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)
find_library(NPPICC_LIBRARY nppicc HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)
find_library(NPPIDEI_LIBRARY nppidei HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)
find_library(NPPIAL_LIBRARY nppial HINTS ${CUDA_TOOLKIT_ROOT_DIR}/lib64 /usr/local/cuda/lib64)

# --- TensorRT Libraries ---
if(EXISTS ${TENSORRT_ROOT})
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h HINTS "${TENSORRT_ROOT}/include")
    find_library(NVINFER_LIBRARY nvinfer HINTS "${TENSORRT_ROOT}/lib")
    find_library(NVINFER_PLUGIN_LIBRARY nvinfer_plugin HINTS "${TENSORRT_ROOT}/lib")
    if(NVINFER_LIBRARY)
        set(TENSORRT_FOUND TRUE)
        message(STATUS "TensorRT found at: ${TENSORRT_ROOT}")
    endif()
else()
    set(TENSORRT_FOUND FALSE)
    message(STATUS "TensorRT not found")
endif()

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

message(STATUS "TensorStore configured successfully")

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
file(GLOB CPP_SOURCES "src/*.cpp")

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
    /usr/local/cuda/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
    ${IMGUI_FILE_DIALOG_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${NVML_INCLUDE_DIR}
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
    tensorstore::tensorstore
    tensorstore::driver_zarr
    tensorstore::index_space
    tensorstore::kvstore_file

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

# Add NVML if found
if(NVML_LIBRARY)
    target_link_libraries(redgui PRIVATE ${NVML_LIBRARY})
    target_compile_definitions(redgui PRIVATE USE_NVML)
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

# --- Post-Build Commands ---
# Copy font files if they exist
set(FONT_FILES
    "src/Roboto-Regular.ttf"
    "src/fa-solid-900.ttf"
)

foreach(FONT_FILE ${FONT_FILES})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${FONT_FILE})
        add_custom_command(TARGET redgui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/${FONT_FILE}"
            "$<TARGET_FILE_DIR:redgui>/"
        )
    endif()
endforeach()

# --- Print Configuration Summary ---
message(STATUS "========================================")
message(STATUS "Configuration Summary:")
message(STATUS "  CUDA Version: ${CUDA_VERSION}")
message(STATUS "  CUDA Toolkit Root: ${CUDA_TOOLKIT_ROOT_DIR}")
message(STATUS "  CUDA Architecture: ${CMAKE_CUDA_ARCHITECTURES}")
message(STATUS "  OpenCV Version: ${OpenCV_VERSION}")
message(STATUS "  OpenCV Dir: ${OpenCV_DIR}")
message(STATUS "  FFmpeg Directory: ${FFMPEG_ROOT}")
if(TENSORRT_FOUND)
    message(STATUS "  TensorRT: FOUND at ${TENSORRT_ROOT}")
else()
    message(STATUS "  TensorRT: NOT FOUND")
endif()
if(NVML_LIBRARY)
    message(STATUS "  NVML: FOUND")
endif()
message(STATUS "  Output Directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "========================================")