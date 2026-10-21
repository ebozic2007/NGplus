cmake_minimum_required(VERSION 3.26)

set(MOD_VERSION "1.0.0")
configure_file(mod.json.in mod.json @ONLY NEWLINE_STYLE UNIX)

project(newgameplus CXX)

if (NOT DUSKLIGHT_VERSION)
  set(DUSKLIGHT_VERSION "b245c6bef8b4a370afb2453104a585dd41c97676")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(newgameplus
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON ${CMAKE_CURRENT_BINARY_DIR}/mod.json
        RES_DIR res
)