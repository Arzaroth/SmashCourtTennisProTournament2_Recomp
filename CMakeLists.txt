cmake_minimum_required(VERSION 3.21)
project(SmashCourtTennisProTournament2_Recomp LANGUAGES C CXX)

set(SCT2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Output of ps2_recomp for SLUS_209.33 (scripts/recomp.sh), VU1 microcode in vu1/ (scripts/compile_vu1.sh)")

set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "Enable very verbose/agressive PS2 runtime logs")

add_subdirectory(PS2Recomp)

file(GLOB SCT2_GENERATED_SOURCES CONFIGURE_DEPENDS "${SCT2_GENERATED_DIR}/*.cpp")
if(NOT SCT2_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources in ${SCT2_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime's placeholder function table clashes with the generated register_functions.cpp.
get_target_property(_sct2_runner_sources ps2EntryRunner SOURCES)
list(FILTER _sct2_runner_sources EXCLUDE REGEX "src/runner/register_functions\\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_sct2_runner_sources})

file(GLOB SCT2_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")

file(GLOB SCT2_VU1_SOURCES CONFIGURE_DEPENDS "${SCT2_GENERATED_DIR}/vu1/*.cpp")
if(SCT2_VU1_SOURCES)
    set_source_files_properties(${SCT2_VU1_SOURCES} PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        COMPILE_OPTIONS "-O3;-g0")
else()
    message(STATUS "SmashCourtTennisProTournament2_Recomp: no compiled VU1 microcode in ${SCT2_GENERATED_DIR}/vu1, "
        "VU1 runs in the interpreter (scripts/compile_vu1.sh)")
endif()

file(GLOB SCT2_OVERLAY_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${SCT2_GENERATED_DIR}/overlays/*")
set(SCT2_OVERLAY_SOURCES "")
foreach(_sct2_overlay_dir ${SCT2_OVERLAY_DIRS})
    if(IS_DIRECTORY "${_sct2_overlay_dir}")
        file(GLOB _sct2_overlay_sources CONFIGURE_DEPENDS "${_sct2_overlay_dir}/*.cpp")
        list(APPEND SCT2_OVERLAY_SOURCES ${_sct2_overlay_sources})
        target_include_directories(ps2EntryRunner PRIVATE "${_sct2_overlay_dir}")
    endif()
endforeach()

target_sources(ps2EntryRunner PRIVATE
    ${SCT2_GENERATED_SOURCES} ${SCT2_RUNTIME_SOURCES} ${SCT2_VU1_SOURCES} ${SCT2_OVERLAY_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${SCT2_GENERATED_DIR}")

list(LENGTH SCT2_GENERATED_SOURCES _sct2_count)
list(LENGTH SCT2_VU1_SOURCES _sct2_vu1_count)
list(LENGTH SCT2_OVERLAY_SOURCES _sct2_overlay_count)
message(STATUS "SmashCourtTennisProTournament2_Recomp: ${_sct2_count} generated sources, ${_sct2_vu1_count} compiled VU1 "
    "file(s), ${_sct2_overlay_count} overlay source(s)")
