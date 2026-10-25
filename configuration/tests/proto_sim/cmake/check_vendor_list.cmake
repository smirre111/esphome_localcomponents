# The vendored-header list is kept twice: the node's proto/regen_stubs.sh copies
# the files, this suite's cmake/vendored_shared_headers.txt gates them. Two
# hand-kept lists drift (review P2): a header added to one only is either
# never synced or never gated. This compares the two sets.
#
# Run from CTest with REGEN_SCRIPT (path to regen_stubs.sh) and HUB_LIST (path
# to vendored_shared_headers.txt).

foreach(f ${REGEN_SCRIPT} ${HUB_LIST})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "missing file: ${f}")
    endif()
endforeach()

file(READ ${REGEN_SCRIPT} regen)
# Entries look like "main/include/Foo.h:Foo.h"; FrameCrypto.h is synced by a
# block of its own (FC_SRC=.../FrameCrypto.h).
string(REGEX MATCHALL "\"main/[A-Za-z0-9_/.]+:[A-Za-z0-9_.]+\"" entries "${regen}")
set(regen_set "")
foreach(e ${entries})
    string(REGEX REPLACE "^.*:([A-Za-z0-9_.]+)\"$" "\\1" name "${e}")
    list(APPEND regen_set ${name})
endforeach()
if(regen MATCHES "FC_SRC=\"[^\"]*/FrameCrypto\\.h\"")
    list(APPEND regen_set FrameCrypto.h)
endif()
list(REMOVE_DUPLICATES regen_set)
list(SORT regen_set)

file(STRINGS ${HUB_LIST} hub_lines)
set(hub_set "")
foreach(l ${hub_lines})
    string(STRIP "${l}" l)
    if(l STREQUAL "" OR l MATCHES "^#")
        continue()
    endif()
    string(REGEX REPLACE "^.*/" "" name "${l}")
    list(APPEND hub_set ${name})
endforeach()
list(REMOVE_DUPLICATES hub_set)
list(SORT hub_set)

if(NOT regen_set STREQUAL hub_set)
    set(only_regen ${regen_set})
    list(REMOVE_ITEM only_regen ${hub_set})
    set(only_hub ${hub_set})
    list(REMOVE_ITEM only_hub ${regen_set})
    message(FATAL_ERROR
        "The vendored-file lists disagree.\n"
        "  synced by regen_stubs.sh but not gated here: ${only_regen}\n"
        "  gated here but not synced by regen_stubs.sh: ${only_hub}\n"
        "Add the file to both: ${REGEN_SCRIPT} and ${HUB_LIST}.")
endif()
list(LENGTH hub_set n)
message(STATUS "vendored-file lists agree (${n} files)")
