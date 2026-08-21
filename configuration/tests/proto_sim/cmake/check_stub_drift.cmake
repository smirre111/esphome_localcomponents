# Schema-drift gate helper. Regenerates blinds.pb-c.{c,h} from the proto
# in PROTO_DIR and compares against the committed stubs in COMPARE_AGAINST.
# Run from CTest with PROTOC_C / PROTO_DIR / COMPARE_AGAINST / OUT_DIR set.
file(MAKE_DIRECTORY ${OUT_DIR})

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
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
                ${OUT_DIR}/${stub} ${COMPARE_AGAINST}/${stub}
        RESULT_VARIABLE diff_rc)
    if(NOT diff_rc EQUAL 0)
        message(FATAL_ERROR
            "${stub} regenerated from ${PROTO_DIR}/blinds.proto differs "
            "from ${COMPARE_AGAINST}/${stub}. Either the proto changed "
            "and stubs were not regenerated, or the two proto sources "
            "have semantically drifted.")
    endif()
endforeach()
