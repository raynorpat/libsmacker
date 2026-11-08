cmake_minimum_required(VERSION 3.16)
project(libsmacker VERSION 1.2.0 LANGUAGES C)

# Default to an optimized build with symbols for single-config generators.
# Multi-config generators choose the configuration at build time instead.
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT isMultiConfig AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

option(SMACKER_BUILD_STATIC "Build the static libsmacker library" ON)
option(SMACKER_BUILD_SHARED "Build the shared libsmacker library" ON)
option(SMACKER_BUILD_TOOLS "Build the example programs" OFF)
option(SMACKER_BUILD_TESTS "Build the encoder test suite" OFF)
set(SMACKER_SDL_VERSION "AUTO" CACHE STRING
  "SDL version for smk_sdl_player: AUTO, 2 or 3")
set_property(CACHE SMACKER_SDL_VERSION PROPERTY STRINGS AUTO 2 3)

if(NOT SMACKER_BUILD_STATIC AND NOT SMACKER_BUILD_SHARED)
  message(FATAL_ERROR
    "Nothing to build: enable SMACKER_BUILD_STATIC and/or SMACKER_BUILD_SHARED")
endif()

# Language standard and warning flags shared by every target.
function(smacker_set_compile_settings target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

  if(MSVC)
    target_compile_options(${target} PRIVATE /W3)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# Library targets expose their headers to consumers in both trees.
function(smacker_set_library_settings target)
  smacker_set_compile_settings(${target})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endfunction()

if(SMACKER_BUILD_STATIC)
  add_library(smacker_static STATIC src/smacker.c)
  smacker_set_library_settings(smacker_static)
  set_target_properties(smacker_static PROPERTIES OUTPUT_NAME smacker)
endif()

if(SMACKER_BUILD_SHARED)
  add_library(smacker_shared SHARED src/smacker.c)
  smacker_set_library_settings(smacker_shared)
  set_target_properties(smacker_shared PROPERTIES
    OUTPUT_NAME smacker
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Keep smacker.lib (static) and the import library (shared) from colliding on Windows.
if(WIN32 AND SMACKER_BUILD_STATIC AND SMACKER_BUILD_SHARED)
  set_target_properties(smacker_shared PROPERTIES ARCHIVE_OUTPUT_NAME smacker_dll)
endif()

if(SMACKER_BUILD_SHARED)
  set(SMACKER_LINK_TARGET smacker_shared)
else()
  set(SMACKER_LINK_TARGET smacker_static)
endif()

add_library(Smacker::smacker ALIAS ${SMACKER_LINK_TARGET})

# Reusable encoder sources, compiled directly into build_smk and the tests.
set(SMACKER_ENCODER_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/encoder/encoder.c)
set(SMACKER_TOOL_INPUT_SOURCES)

if(SMACKER_BUILD_TOOLS)
  add_executable(driver src/tools/driver.c)
  add_executable(smk2avi src/tools/smk2avi.c)
  foreach(tool driver smk2avi)
    # The library's PUBLIC include directory propagates through the link.
    smacker_set_compile_settings(${tool})
    target_link_libraries(${tool} PRIVATE ${SMACKER_LINK_TARGET})
  endforeach()

  add_executable(build_smk
    src/tools/build_smk/main.c
    ${SMACKER_TOOL_INPUT_SOURCES}
    ${SMACKER_ENCODER_SOURCES})
  target_include_directories(build_smk PRIVATE
    src/encoder
    src/tools/build_smk)
  smacker_set_compile_settings(build_smk)
  target_link_libraries(build_smk PRIVATE ${SMACKER_LINK_TARGET})

  # The SDL player links the static library, so it needs both SDL and
  # SMACKER_BUILD_STATIC. Prefer SDL3, fall back to SDL2, and skip the
  # example rather than breaking the rest of the tool build.
  if(NOT SMACKER_SDL_VERSION MATCHES "^(AUTO|2|3)$")
    message(FATAL_ERROR
      "SMACKER_SDL_VERSION must be AUTO, 2 or 3 (got '${SMACKER_SDL_VERSION}')")
  endif()

  if(NOT SMACKER_BUILD_STATIC)
    message(WARNING
      "smk_sdl_player requires SMACKER_BUILD_STATIC=ON; skipping it.")
  else()
    set(SMACKER_SDL_TARGET "")
    set(SMACKER_USE_SDL3 OFF)

    if(SMACKER_SDL_VERSION STREQUAL "AUTO" OR SMACKER_SDL_VERSION STREQUAL "3")
      find_package(SDL3 QUIET CONFIG)
      if(SDL3_FOUND)
        set(SMACKER_SDL_TARGET SDL3::SDL3)
        set(SMACKER_USE_SDL3 ON)
      elseif(SMACKER_SDL_VERSION STREQUAL "3")
        message(FATAL_ERROR
          "SMACKER_SDL_VERSION=3 but no SDL3 CMake package was found.")
      endif()
    endif()

    if(SMACKER_SDL_TARGET STREQUAL "" AND
        (SMACKER_SDL_VERSION STREQUAL "AUTO" OR SMACKER_SDL_VERSION STREQUAL "2"))
      find_package(SDL2 QUIET CONFIG)
      if(SDL2_FOUND)
        set(SMACKER_SDL_TARGET SDL2::SDL2)
      elseif(SMACKER_SDL_VERSION STREQUAL "2")
        message(FATAL_ERROR
          "SMACKER_SDL_VERSION=2 but no SDL2 CMake package was found.")
      endif()
    endif()

    if(SMACKER_SDL_TARGET STREQUAL "")
      message(WARNING "SDL2/SDL3 not found; skipping smk_sdl_player.")
    else()
      add_executable(smk_sdl_player src/tools/smk_sdl_player.c)
      smacker_set_compile_settings(smk_sdl_player)
      if(SMACKER_USE_SDL3)
        target_compile_definitions(smk_sdl_player PRIVATE SMK_USE_SDL3=1)
      elseif(TARGET SDL2::SDL2main)
        target_link_libraries(smk_sdl_player PRIVATE SDL2::SDL2main)
      endif()
      target_link_libraries(smk_sdl_player PRIVATE
        smacker_static ${SMACKER_SDL_TARGET})
      message(STATUS "smk_sdl_player: linking smacker_static and ${SMACKER_SDL_TARGET}")
    endif()
  endif()
endif()

if(SMACKER_BUILD_TESTS)
  if(NOT SMACKER_BUILD_TOOLS)
    message(FATAL_ERROR "SMACKER_BUILD_TESTS requires SMACKER_BUILD_TOOLS=ON")
  endif()
  enable_testing()
  add_subdirectory(tests)
endif()

if(SMACKER_BUILD_STATIC)
  install(TARGETS smacker_static
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(SMACKER_BUILD_SHARED)
  install(TARGETS smacker_shared
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(FILES src/smacker.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
