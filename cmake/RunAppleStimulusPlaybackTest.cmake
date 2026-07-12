if(NOT DEFINED FIXTURE_WRITER OR NOT DEFINED PLAYBACK_TEST OR
   NOT DEFINED VIDEO_FIXTURE)
    message(FATAL_ERROR "fixture writer, playback test, and video path are required")
endif()

execute_process(
    COMMAND "${FIXTURE_WRITER}" --write-fixture "${VIDEO_FIXTURE}"
    RESULT_VARIABLE fixture_result
    OUTPUT_VARIABLE fixture_output
    ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR
        "stimulus video fixture failed (${fixture_result})\n"
        "${fixture_output}${fixture_error}")
endif()

execute_process(
    COMMAND "${PLAYBACK_TEST}" "${VIDEO_FIXTURE}"
    RESULT_VARIABLE playback_result
    OUTPUT_VARIABLE playback_output
    ERROR_VARIABLE playback_error
)
file(REMOVE "${VIDEO_FIXTURE}")
if(NOT playback_result EQUAL 0)
    message(FATAL_ERROR
        "stimulus playback test failed (${playback_result})\n"
        "${playback_output}${playback_error}")
endif()

message(STATUS "${fixture_output}${playback_output}")
