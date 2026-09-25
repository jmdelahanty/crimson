foreach(_required_var IN ITEMS CRIMSON_RPATH_FILE CRIMSON_OLD_RPATH CRIMSON_NEW_RPATH)
    if(NOT DEFINED ${_required_var})
        message(FATAL_ERROR "${_required_var} is required")
    endif()
endforeach()

if(NOT EXISTS "${CRIMSON_RPATH_FILE}")
    message(FATAL_ERROR "RUNPATH target does not exist: ${CRIMSON_RPATH_FILE}")
endif()

file(RPATH_CHANGE
    FILE "${CRIMSON_RPATH_FILE}"
    OLD_RPATH "${CRIMSON_OLD_RPATH}"
    NEW_RPATH "${CRIMSON_NEW_RPATH}")
