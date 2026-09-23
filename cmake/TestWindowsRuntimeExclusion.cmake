# Exercise the actual generated-script regex without pretending to run Windows.
file(STRINGS "${CRIMSON_SOURCE_ROOT}/CMakeLists.txt" _lines
    REGEX "Ww.*indows.*System32.*SysWOW64")
list(LENGTH _lines _count)
if(NOT _count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one Windows system DLL exclusion regex")
endif()
list(GET _lines 0 _line)
cmake_language(EVAL CODE "set(_system_regex ${_line})")
foreach(_system_path IN ITEMS
        [[C:\Windows\System32\kernel32.dll]] [[C:\Windows\SysWOW64\kernel32.dll]]
        "C:/Windows/System32/kernel32.dll" "C:/windows/SysWOW64/kernel32.dll")
    if(NOT _system_path MATCHES "${_system_regex}")
        message(FATAL_ERROR "System DLL not excluded: ${_system_path}")
    endif()
endforeach()
foreach(_app_path IN ITEMS [[C:\Crimson\bin\VCRUNTIME140_1.dll]]
        "C:/Crimson/bin/kernel32.dll" "C:/Windows/not-system/nvcuvid.dll")
    if(_app_path MATCHES "${_system_regex}")
        message(FATAL_ERROR "App/driver DLL incorrectly excluded: ${_app_path}")
    endif()
endforeach()
message(STATUS "Windows runtime exclusion path contract passed (not native validation)")
