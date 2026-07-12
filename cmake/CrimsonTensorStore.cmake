include_guard(GLOBAL)

include(FetchContent)

function(crimson_tensorstore_use_system_if_target option_name target_name)
    if(TARGET "${target_name}")
        set(${option_name} ON CACHE BOOL
            "Use an installed dependency when ${target_name} already exists"
            FORCE)
    endif()
endfunction()

function(crimson_define_tensorstore_zarr_target)
    if(TARGET crimson_tensorstore_zarr)
        return()
    endif()

    set(_required_targets
        tensorstore::tensorstore
        tensorstore::kvstore_file
        tensorstore::kvstore_memory
        tensorstore::driver_zarr_driver
        tensorstore::driver_zarr_zstd_compressor
        tensorstore::driver_zarr3_driver
        tensorstore::driver_zarr3_codec_bytes
        tensorstore::driver_zarr3_codec_zstd)
    foreach(_target IN LISTS _required_targets)
        if(NOT TARGET ${_target})
            return()
        endif()
    endforeach()

    add_library(crimson_tensorstore_zarr INTERFACE)
    add_library(crimson::tensorstore_zarr ALIAS crimson_tensorstore_zarr)
    target_link_libraries(crimson_tensorstore_zarr INTERFACE
        tensorstore::tensorstore
        tensorstore::kvstore_file
        tensorstore::kvstore_memory
        tensorstore::driver_zarr_driver
        tensorstore::driver_zarr_zstd_compressor
        tensorstore::driver_zarr3_driver
        tensorstore::driver_zarr3_codec_bytes
        tensorstore::driver_zarr3_codec_zstd)
endfunction()

function(crimson_fetch_tensorstore)
    if(TARGET tensorstore::all_drivers)
        crimson_define_tensorstore_zarr_target()
        return()
    endif()

    set(TENSORSTORE_BUILD_TESTS OFF CACHE BOOL "Build TensorStore tests" FORCE)
    set(TENSORSTORE_BUILD_EXAMPLES OFF CACHE BOOL "Build TensorStore examples" FORCE)
    set(TENSORSTORE_BUILD_PYTHON_BINDINGS OFF CACHE BOOL
        "Build TensorStore Python bindings" FORCE)
    set(TENSORSTORE_BUILD_BENCHMARKS OFF CACHE BOOL
        "Build TensorStore benchmarks" FORCE)
    set(TENSORSTORE_BUILD_DOC OFF CACHE BOOL "Build TensorStore documentation" FORCE)

    if(WIN32)
        # Prefer dependency targets already supplied by vcpkg, except for
        # nlohmann_json: TensorStore 0.1.64 requires its vendored 3.11 headers.
        set(TENSORSTORE_USE_SYSTEM_NLOHMANN_JSON OFF CACHE BOOL
            "Use TensorStore's vendored nlohmann_json on Windows" FORCE)
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_ZLIB "ZLIB::ZLIB")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_OPENSSL "OpenSSL::SSL")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_CURL "CURL::libcurl")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_PNG "PNG::PNG")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_JPEG "JPEG::JPEG")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_TIFF "TIFF::TIFF")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_WEBP "WebP::webp")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_LIBLZMA "LibLZMA::LibLZMA")
        crimson_tensorstore_use_system_if_target(
            TENSORSTORE_USE_SYSTEM_BZIP2 "BZip2::BZip2")
    endif()

    FetchContent_Declare(
        tensorstore
        GIT_REPOSITORY https://github.com/google/tensorstore.git
        GIT_TAG v0.1.64
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(tensorstore)

    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        # Abseil adds paired -Xarch selectors for universal Apple builds.
        # CMake de-duplicates the repeated selector and exposes an x86 SSE flag
        # to an arm64-only compile. Keep only the native hardware-AES option.
        foreach(_target IN ITEMS
                absl_random_internal_randen_hwaes
                absl_random_internal_randen_hwaes_impl)
            if(TARGET ${_target})
                get_target_property(_options ${_target} COMPILE_OPTIONS)
                if(_options)
                    list(FILTER _options EXCLUDE REGEX
                        "^-Xarch_|^-maes$|^-msse4\\.1$|^-march=armv8-a\\+crypto$")
                    list(APPEND _options "-march=armv8-a+crypto")
                    set_property(TARGET ${_target} PROPERTY
                        COMPILE_OPTIONS "${_options}")
                endif()
            endif()
        endforeach()
    endif()

    crimson_define_tensorstore_zarr_target()
endfunction()
