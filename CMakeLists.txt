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
option(SMACKER_BUILD_TOOLS "Build the driver and smk2avi example programs" OFF)

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

if(SMACKER_BUILD_TOOLS)
  add_executable(driver src/tools/driver.c)
  add_executable(smk2avi src/tools/smk2avi.c)
  foreach(tool driver smk2avi)
    # The library's PUBLIC include directory propagates through the link.
    smacker_set_compile_settings(${tool})
    target_link_libraries(${tool} PRIVATE ${SMACKER_LINK_TARGET})
  endforeach()
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
