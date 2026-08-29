# Schema-drift gate helper. Regenerates blinds.pb-c.{c,h} from the proto in
# PROTO_DIR and compares against the committed stubs.
#
# Two callers with different layouts:
#   COMPARE_AGAINST                     — both stubs in one dir (ESPHome)
#   COMPARE_C_AGAINST / COMPARE_H_AGAINST — split src/ and include/ (the node
#                                           component, IDF's layout)
file(MAKE_DIRECTORY ${OUT_DIR})

if(DEFINED COMPARE_AGAINST)
    set(COMPARE_C_AGAINST ${COMPARE_AGAINST})
    set(COMPARE_H_AGAINST ${COMPARE_AGAINST})
endif()

execute_process(
    COMMAND ${PROTOC_C} --c_out=${OUT_DIR} -I${PROTO_DIR} blinds.proto
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err)
# protoc-c on Ubuntu 24.04 emits a deprecation notice to stderr but still
# generates files successfully and returns 0. Treat rc==0 as success
# regardless of stderr noise.
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "protoc-c failed (${rc}): ${err}")
endif()

foreach(stub blinds.pb-c.h blinds.pb-c.c)
    if(stub STREQUAL blinds.pb-c.h)
        set(against ${COMPARE_H_AGAINST})
    else()
        set(against ${COMPARE_C_AGAINST})
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
                ${OUT_DIR}/${stub} ${against}/${stub}
        RESULT_VARIABLE diff_rc)
    if(NOT diff_rc EQUAL 0)
        message(FATAL_ERROR
            "${stub} regenerated from ${PROTO_DIR}/blinds.proto differs "
            "from ${against}/${stub}. Either the proto changed and stubs "
            "were not regenerated, or the two copies have drifted. "
            "Fix with: BlindsESP/proto/regen_stubs.sh")
    endif()
endforeach()
