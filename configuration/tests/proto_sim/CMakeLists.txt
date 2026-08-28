cmake_minimum_required(VERSION 3.16)
project(loradevices_proto_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Coverage gate (opt-in). Build with -DPROTO_SIM_COVERAGE=ON to instrument
# the proto_sim + real_* libraries; CI then runs scripts/run_coverage.sh
# which invokes gcovr and enforces the line-coverage threshold.
option(PROTO_SIM_COVERAGE "Instrument tests for line-coverage measurement" OFF)
if(PROTO_SIM_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "Coverage instrumentation requires gcc/clang")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Real protobuf-c stubs (the same generated C file the ESPHome firmware
# compiles). Compiled into our test library so the wire format under test
# is byte-identical to production.
set(BLINDSPROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../local_components/blindsproto)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF_C REQUIRED libprotobuf-c)

add_library(proto_sim STATIC
    sim/sim_clock.cpp
    sim/sim_radio.cpp
    sim/wire_codec.cpp
    sim/crypto.cpp
    sim/hub_model.cpp
    sim/node_model.cpp
    ${BLINDSPROTO_DIR}/blinds.pb-c.c
)
target_include_directories(proto_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${BLINDSPROTO_DIR}
    ${PROTOBUF_C_INCLUDE_DIRS}
)
# mbedcrypto carries AES-GCM and SHA-256.
target_link_libraries(proto_sim PUBLIC ${PROTOBUF_C_LIBRARIES} mbedcrypto)
target_link_directories(proto_sim PUBLIC ${PROTOBUF_C_LIBRARY_DIRS})

enable_testing()
include(GoogleTest)

function(add_scenario name)
    add_executable(${name} scenarios/${name}.cpp)
    target_link_libraries(${name} PRIVATE proto_sim GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name})
endfunction()

# Schema-drift gates. Whitespace/comment-tolerant: both gates work by
# regenerating .pb-c.{c,h} stubs and comparing the BINARY-compatible
# generated output, so cosmetic differences in the .proto sources don't
# trip the build.
find_program(PROTOC_C_EXECUTABLE protoc-c)
if(PROTOC_C_EXECUTABLE)
    set(ESPHOME_STUB_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/../../local_components/blindsproto)

    # Gate 1 (ESPHome-side .proto regenerates to its own stubs) was REMOVED:
    # the ESPHome-side copy local_components/lora_tracker/proto/blinds.proto was
    # deleted in v1.0.12, leaving BlindsESP/proto/blinds.proto as the single
    # source of truth.  The gate had been silently pointing at a non-existent
    # directory ever since, so it failed rather than guarding anything.  Gate 2
    # below now covers the same ground from the authoritative proto.

    # Gate 2: the hub-authoritative .proto must regenerate stubs identical to
    # ESPHome's committed ones. Catches BOTH "edited the proto but forgot to
    # regenerate" and semantic drift (field number, type, oneof case),
    # regardless of whitespace / comment formatting.
    set(HUB_PROTO_DIR /mnt/c/Development/PlatformIO/PlatformIO/BlindsESP/proto)
    if(NOT EXISTS ${HUB_PROTO_DIR}/blinds.proto)
        set(HUB_PROTO_DIR C:/Development/PlatformIO/PlatformIO/BlindsESP/proto)
    endif()
    if(EXISTS ${HUB_PROTO_DIR}/blinds.proto)
        add_test(NAME schema_drift_hub_vs_esphome
                 COMMAND ${CMAKE_COMMAND}
                         -DPROTOC_C=${PROTOC_C_EXECUTABLE}
                         -DPROTO_DIR=${HUB_PROTO_DIR}
                         -DCOMPARE_AGAINST=${ESPHOME_STUB_DIR}
                         -DOUT_DIR=${CMAKE_BINARY_DIR}/stub_regen_hub
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_stub_drift.cmake)
        set_tests_properties(schema_drift_hub_vs_esphome PROPERTIES
            LABELS "schema_drift")
    endif()
endif()

add_scenario(register_test)
add_scenario(login_test)
add_scenario(addressing_test)
add_scenario(sleep_wake_test)
add_scenario(sysop_test)
add_scenario(boundary_test)
add_scenario(cover_config_test)
add_scenario(reboot_test)
add_scenario(race_test)
add_scenario(crypto_test)
add_scenario(garbage_test)
add_scenario(recovery_test)
add_scenario(auto_mode_proto_test)   # P0: automatic-mode schema + frame budget

# ----------------------------------------------------------------------------
# Phase 3: compile the REAL production lora_client.cpp against host shims.
# The library produced here exposes esphome::lora_tracker::LORAListener as
# defined in the production headers — no parallel implementation.
# ----------------------------------------------------------------------------
set(LORA_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../local_components/lora_client)
set(SHIM_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/shims)

# PSA Crypto headers ship with libmbedtls-dev on Ubuntu; mbedcrypto already
# linked via proto_sim.
add_library(real_lora_client STATIC
    ${LORA_CLIENT_DIR}/lora_client.cpp
    ${SHIM_DIR}/esphome/core/component.cpp
    ${SHIM_DIR}/esphome/components/lora_tracker/lora_tracker.cpp
    ${SHIM_DIR}/esp_random.c
)
target_include_directories(real_lora_client
    BEFORE PUBLIC
        ${SHIM_DIR}                    # esphome/core/*, esp_bt_defs.h, esp_random.h
        ${LORA_CLIENT_DIR}             # the production lora_client.h + lora_client_node.h
        ${BLINDSPROTO_DIR}             # blinds.pb-c.h
        ${CMAKE_CURRENT_SOURCE_DIR}    # sim/* used by the shims
        ${PROTOBUF_C_INCLUDE_DIRS}
)
target_link_libraries(real_lora_client PUBLIC proto_sim mbedcrypto)

# Phase 3 scenario that drives the real LORAListener through the same
# SimClock/SimRadio harness the model-based tests use.
add_executable(real_lora_client_test scenarios/real_lora_client_test.cpp)
target_link_libraries(real_lora_client_test
    PRIVATE real_lora_client GTest::gtest GTest::gtest_main)
gtest_discover_tests(real_lora_client_test)

# ----------------------------------------------------------------------------
# Phase 3 hub-cover: compile the REAL LoraCoverComponent against shims so
# the cover entity's protocol path (control() → CMD_OPERATION on the wire,
# set_response() handling CoverPosition replies) runs as production code.
# ----------------------------------------------------------------------------
set(LORACOVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../local_components/loracover)
add_library(real_lora_cover STATIC
    ${LORACOVER_DIR}/cover/lora_cover.cpp
    ${LORACOVER_DIR}/loracover_base.cpp
)
target_include_directories(real_lora_cover
    BEFORE PUBLIC
        ${SHIM_DIR}
        ${LORACOVER_DIR}/cover         # lora_cover.h
        ${LORACOVER_DIR}                # loracover_base.h
        ${LORA_CLIENT_DIR}              # lora_client.h, lora_client_node.h
        ${BLINDSPROTO_DIR}              # blinds.pb-c.h
        ${CMAKE_CURRENT_SOURCE_DIR}     # sim/*
        ${PROTOBUF_C_INCLUDE_DIRS}
)
target_link_libraries(real_lora_cover PUBLIC real_lora_client proto_sim)
target_compile_options(real_lora_cover PRIVATE
    -Wno-unused-parameter -Wno-sign-compare -Wno-format)

add_executable(real_lora_cover_test scenarios/real_lora_cover_test.cpp)
target_link_libraries(real_lora_cover_test
    PRIVATE real_lora_cover GTest::gtest GTest::gtest_main)
gtest_discover_tests(real_lora_cover_test)

# ----------------------------------------------------------------------------
# Phase 3 node side: compile the REAL production CmdDispatcher.cpp against
# host shims for FreeRTOS + ESP-IDF + sibling classes (MotorCtrl, SystemCtrl,
# LoraInterface). Tasks are NOT spawned; tests drive onReceiveNew() and pump
# the TX queue by hand.
# ----------------------------------------------------------------------------
set(BLINDS_ESP_MAIN /mnt/c/Development/PlatformIO/PlatformIO/BlindsESP/main)
if(NOT EXISTS ${BLINDS_ESP_MAIN})
    # Fallback for native Windows path lookups during config probing.
    set(BLINDS_ESP_MAIN C:/Development/PlatformIO/PlatformIO/BlindsESP/main)
endif()
set(NODE_SHIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shims_node)

# P3: the schedule arithmetic is dependency-free by design (only <stdint.h> and
# <time.h>), so it compiles directly from the production source with no shims
# and no staging — the calendar logic under test is byte-for-byte what the node
# runs.
if(EXISTS ${BLINDS_ESP_MAIN}/Scheduler.cpp)
    add_executable(scheduler_test
        scenarios/scheduler_test.cpp
        ${BLINDS_ESP_MAIN}/Scheduler.cpp)
    target_include_directories(scheduler_test PRIVATE ${BLINDS_ESP_MAIN}/include)
    target_link_libraries(scheduler_test PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(scheduler_test)
endif()

# Same rule for the boot policy: BootPolicy.h is header-only and
# dependency-free precisely so the reset classification and the
# reference-move decision are covered here rather than only on hardware.
if(EXISTS ${BLINDS_ESP_MAIN}/include/BootPolicy.h)
    add_executable(boot_policy_test scenarios/boot_policy_test.cpp)
    target_include_directories(boot_policy_test PRIVATE ${BLINDS_ESP_MAIN}/include)
    target_link_libraries(boot_policy_test PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(boot_policy_test)
endif()

# AutoModePolicy.h is header-only and dependency-free for the same reason
# BootPolicy.h is: the arithmetic that decides when a battery node wakes and
# whether it acts is verified here rather than on a roof.
if(EXISTS ${BLINDS_ESP_MAIN}/include/AutoModePolicy.h)
    add_executable(auto_mode_policy_test
        scenarios/auto_mode_policy_test.cpp
        ${BLINDS_ESP_MAIN}/Scheduler.cpp)
    target_include_directories(auto_mode_policy_test PRIVATE ${BLINDS_ESP_MAIN}/include)
    target_link_libraries(auto_mode_policy_test PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(auto_mode_policy_test)
endif()

if(EXISTS ${BLINDS_ESP_MAIN}/CmdDispatcher.cpp)
    # Quoted-include searches the dir of the FILE doing the include first
    # (no compiler flag overrides that). We copy the production source +
    # headers we need into a staging dir, and place sibling-class shims
    # (MotorCtrl/SystemCtrl/LoraInterface/Packet/myinterrupts) at the same
    # path so `#include "MotorCtrl.h"` from CmdDispatcher.h finds the shim
    # instead of the real ESP-IDF-laden file.
    set(NODE_STAGE ${CMAKE_BINARY_DIR}/node_stage)
    file(MAKE_DIRECTORY ${NODE_STAGE})
    configure_file(${BLINDS_ESP_MAIN}/CmdDispatcher.cpp ${NODE_STAGE}/CmdDispatcher.cpp COPYONLY)
    # SessionManager: the session state extracted out of CmdDispatcher. Staged
    # alongside so the harness compiles the REAL class, not a shim.
    configure_file(${BLINDS_ESP_MAIN}/SessionManager.cpp ${NODE_STAGE}/SessionManager.cpp COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/include/SessionManager.h ${NODE_STAGE}/SessionManager.h COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/comm_utils.c      ${NODE_STAGE}/comm_utils.c      COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/include/comm_utils.h ${NODE_STAGE}/comm_utils.h   COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/include/CmdDispatcher.h ${NODE_STAGE}/CmdDispatcher.h COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/include/common.h         ${NODE_STAGE}/common.h         COPYONLY)
    configure_file(${BLINDS_ESP_MAIN}/include/Packet.h         ${NODE_STAGE}/Packet.h         COPYONLY)
    # Drop the sibling shims into the same staging directory.
    foreach(shim MotorCtrl.h SystemCtrl.h LoraInterface.h myinterrupts.h)
        configure_file(${NODE_SHIM_DIR}/${shim} ${NODE_STAGE}/${shim} COPYONLY)
    endforeach()

    add_library(real_cmd_dispatcher STATIC
        ${NODE_STAGE}/CmdDispatcher.cpp
        ${NODE_STAGE}/SessionManager.cpp
        ${NODE_STAGE}/comm_utils.c
        ${BLINDS_ESP_MAIN}/Scheduler.cpp
        ${NODE_SHIM_DIR}/esp_idf_stubs.c
        ${NODE_SHIM_DIR}/extern_stubs.cpp
    )
    target_include_directories(real_cmd_dispatcher
        BEFORE PUBLIC
            ${NODE_STAGE}                 # CmdDispatcher.h + sibling shims
            ${NODE_SHIM_DIR}              # FreeRTOS, ESP-IDF
            ${BLINDS_ESP_MAIN}/include    # Scheduler.h (dependency-free)
            ${BLINDSPROTO_DIR}            # blinds.pb-c.h
            ${PROTOBUF_C_INCLUDE_DIRS}
    )
    target_link_libraries(real_cmd_dispatcher PUBLIC ${PROTOBUF_C_LIBRARIES} mbedcrypto)
    target_compile_options(real_cmd_dispatcher PRIVATE
        -Wno-sign-compare -Wno-format -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -Wno-format-extra-args -Wno-unused-parameter)

    # SessionManager is deliberately constructible on its own — no motor, no
    # radio, no mutexes — so its tests link only the class and the NVS shim.
    add_executable(session_manager_test
        scenarios/session_manager_test.cpp
        ${NODE_STAGE}/SessionManager.cpp
        ${NODE_SHIM_DIR}/esp_idf_stubs.c
    )
    target_include_directories(session_manager_test
        BEFORE PRIVATE ${NODE_STAGE} ${NODE_SHIM_DIR})
    target_link_libraries(session_manager_test PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(session_manager_test)

    add_executable(real_cmd_dispatcher_test scenarios/real_cmd_dispatcher_test.cpp)
    target_link_libraries(real_cmd_dispatcher_test
        PRIVATE real_cmd_dispatcher proto_sim GTest::gtest GTest::gtest_main)
    gtest_discover_tests(real_cmd_dispatcher_test)
else()
    message(STATUS "BlindsESP node source not found at ${BLINDS_ESP_MAIN} — "
                   "skipping real_cmd_dispatcher target")
endif()
