# NBA 2K18 recompiled - runtime + recompiled game code.
#
# The recompiled code (../ppc) is generated on your machine from your own
# default.xex by XenonRecomp and is never committed.
#
#   cmake -S runtime -B build -G Ninja -DSIMDE_DIR=<path to simde>
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(NBA2K18Recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ppc" CACHE PATH "XenonRecomp output folder")
set(SIMDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/simde" CACHE PATH "simde headers (folder containing x86/)")

if(NOT EXISTS "${PPC_DIR}/ppc_recomp_shared.h")
    message(FATAL_ERROR "No recompiled code in ${PPC_DIR}. Run recompile.bat first.")
endif()
if(NOT EXISTS "${SIMDE_DIR}/x86/sse.h")
    message(FATAL_ERROR "simde headers not found in ${SIMDE_DIR}.")
endif()

# x86-64-v2 (SSE4.2) lets simde map vector instructions to native SSE.
set(ARCH_FLAGS -march=x86-64-v2)

# ---- recompiled game code ----
file(GLOB PPC_SOURCES "${PPC_DIR}/ppc_recomp.*.cpp" "${PPC_DIR}/ppc_func_mapping.cpp")
add_library(game_code OBJECT ${PPC_SOURCES})
target_include_directories(game_code PRIVATE "${PPC_DIR}" "${SIMDE_DIR}")
# -O1 keeps build times and memory reasonable for ~800 MB of generated C++
target_compile_options(game_code PRIVATE ${ARCH_FLAGS} -O1 -w -fno-strict-aliasing)

# ---- stubs for imports not implemented yet ----
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
set(STUBS_CPP "${CMAKE_CURRENT_BINARY_DIR}/stubs.cpp")
add_custom_command(
    OUTPUT "${STUBS_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DPPC_DIR=${PPC_DIR} -DRUNTIME_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUT=${STUBS_CPP} -P "${CMAKE_CURRENT_SOURCE_DIR}/gen_stubs.cmake"
    DEPENDS ${RUNTIME_SOURCES} "${PPC_DIR}/ppc_recomp_shared.h" "${CMAKE_CURRENT_SOURCE_DIR}/gen_stubs.cmake"
    COMMENT "Generating import stubs")

# ---- executable ----
add_executable(NBA2K18 ${RUNTIME_SOURCES} "${STUBS_CPP}" $<TARGET_OBJECTS:game_code>)
target_include_directories(NBA2K18 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PPC_DIR}" "${SIMDE_DIR}")
target_compile_options(NBA2K18 PRIVATE ${ARCH_FLAGS} -fno-strict-aliasing)

if(WIN32)
    # one self-contained exe: no compiler runtime DLLs needed next to it
    target_link_options(NBA2K18 PRIVATE -static)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(NBA2K18 PRIVATE Threads::Threads)
endif()
