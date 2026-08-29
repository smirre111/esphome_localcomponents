# Vendored-header drift gate.
#
# Compares a vendored copy against its source of truth, ignoring everything
# before `#pragma once`. That lets the vendored copy carry a provenance banner
# ("DO NOT EDIT HERE") while still being byte-identical where it matters.
#
# Run from CTest with SOURCE_HEADER / VENDORED_HEADER set.

foreach(f ${SOURCE_HEADER} ${VENDORED_HEADER})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "missing header: ${f}")
    endif()
endforeach()

file(READ ${SOURCE_HEADER}   src)
file(READ ${VENDORED_HEADER} ven)

# Keep from `#pragma once` onward. Anything before it is provenance commentary.
string(FIND "${src}" "#pragma once" src_pos)
string(FIND "${ven}" "#pragma once" ven_pos)
if(src_pos EQUAL -1 OR ven_pos EQUAL -1)
    message(FATAL_ERROR "no '#pragma once' found — cannot align the two headers")
endif()
string(SUBSTRING "${src}" ${src_pos} -1 src_body)
string(SUBSTRING "${ven}" ${ven_pos} -1 ven_body)

# Normalise line endings; the two repos disagree about them and that is not drift.
string(REPLACE "\r\n" "\n" src_body "${src_body}")
string(REPLACE "\r\n" "\n" ven_body "${ven_body}")

if(NOT src_body STREQUAL ven_body)
    message(FATAL_ERROR
        "${VENDORED_HEADER} has drifted from ${SOURCE_HEADER}.\n"
        "The hub and the node must agree on the AEAD wire format byte for byte; "
        "a mismatch compiles and links cleanly on both sides and fails only on "
        "the air, as psa_aead_decrypt: -149.\n"
        "Fix with: BlindsESP/proto/regen_stubs.sh")
endif()
