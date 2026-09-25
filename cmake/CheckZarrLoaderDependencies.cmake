if(NOT DEFINED CRIMSON_SOURCE_ROOT)
    message(FATAL_ERROR "CRIMSON_SOURCE_ROOT is required")
endif()

include("${CRIMSON_SOURCE_ROOT}/cmake/CrimsonZarrLoaderDependencyPolicy.cmake")

file(GLOB_RECURSE crimson_source_files
    RELATIVE "${CRIMSON_SOURCE_ROOT}"
    "${CRIMSON_SOURCE_ROOT}/src/*.c"
    "${CRIMSON_SOURCE_ROOT}/src/*.cc"
    "${CRIMSON_SOURCE_ROOT}/src/*.cpp"
    "${CRIMSON_SOURCE_ROOT}/src/*.h"
    "${CRIMSON_SOURCE_ROOT}/src/*.hpp"
    "${CRIMSON_SOURCE_ROOT}/src/*.m"
    "${CRIMSON_SOURCE_ROOT}/src/*.mm"
)

set(direct_include_files)
set(violations)
foreach(relative_path IN LISTS crimson_source_files)
    file(READ "${CRIMSON_SOURCE_ROOT}/${relative_path}" source_text)
    if(source_text MATCHES "#[ \t]*include[ \t]*[<\"][^\r\n]*zarr_loader\\.h[>\"]")
        list(APPEND direct_include_files "${relative_path}")
        list(FIND CRIMSON_ZARR_LOADER_DIRECT_INCLUDE_ALLOWLIST
            "${relative_path}" allowed_index)
        if(allowed_index EQUAL -1)
            list(APPEND violations "${relative_path}")
        endif()
    endif()
endforeach()

list(LENGTH direct_include_files direct_include_count)
list(LENGTH CRIMSON_ZARR_LOADER_DIRECT_INCLUDE_ALLOWLIST baseline_count)
if(violations)
    list(JOIN violations "\n  " violation_text)
    message(FATAL_ERROR
        "New direct zarr_loader.h dependency detected. Add a feature-level "
        "contract instead of extending the monolith:\n  ${violation_text}")
endif()

message(STATUS
    "ZarrDetectionLoader direct-include baseline: "
    "${direct_include_count}/${baseline_count} allowed files")
