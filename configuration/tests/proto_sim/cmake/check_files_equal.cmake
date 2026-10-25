# Byte-compare pairs of committed files, line endings ignored. Needs no
# compiler: this is the protoc-independent half of the stub gate (review P2),
# so a half-applied regen is caught even where protoc-c is not installed or is
# a different version from the one that generated the committed stubs.
#
# Run from CTest with PAIRS = "a|b;c|d;..." (absolute paths).

foreach(pair ${PAIRS})
    string(REPLACE "|" ";" ab "${pair}")
    list(GET ab 0 a)
    list(GET ab 1 b)
    foreach(f ${a} ${b})
        if(NOT EXISTS ${f})
            message(FATAL_ERROR "missing file: ${f}")
        endif()
    endforeach()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol ${a} ${b}
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "${a} and ${b} differ. The committed stubs of the two repos must be "
            "identical. Fix with: BlindsESP/proto/regen_stubs.sh, and commit both repos.")
    endif()
endforeach()
