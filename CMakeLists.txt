cmake_minimum_required(VERSION 3.20)

set(BNIO_VERSION "0.2.0" CACHE STRING "bnio package version")

project(
  bnio
  VERSION "${BNIO_VERSION}"
  DESCRIPTION "A small native asynchronous I/O library"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bexec.cmake")

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BNIO_IS_TOP_LEVEL ON)
else()
  set(BNIO_IS_TOP_LEVEL OFF)
endif()

# ---- Platform detection ----
#
# bnio uses io_uring on Linux and kqueue on Darwin/FreeBSD. Platform-native
# headers and dependencies are compiled only for their matching backend.
set(BNIO_PLATFORM_LINUX_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(BNIO_PLATFORM_LINUX_DEFAULT ON)
endif()
option(BNIO_PLATFORM_LINUX "Build the Linux io_uring backend"
       ${BNIO_PLATFORM_LINUX_DEFAULT})

set(BNIO_PLATFORM_BSD_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(BNIO_PLATFORM_BSD_DEFAULT ON)
endif()
option(BNIO_PLATFORM_BSD "Build the BSD kqueue backend"
       ${BNIO_PLATFORM_BSD_DEFAULT})

if(BNIO_PLATFORM_LINUX AND BNIO_PLATFORM_BSD)
  message(FATAL_ERROR
          "bnio builds exactly one native backend per target; select Linux or BSD, not both.")
endif()

if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  find_package(OpenSSL REQUIRED)
endif()

if(BNIO_PLATFORM_LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.1)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BNIO_BUILD_TESTS "Build bnio tests" ${BNIO_IS_TOP_LEVEL})
option(BNIO_BUILD_EXAMPLES "Build bnio examples" ${BNIO_IS_TOP_LEVEL})
option(BNIO_BUILD_BENCHMARKS "Build bnio benchmark executables" OFF)
option(BNIO_BUILD_ASIO_EXAMPLES "Build standalone Asio echo server example (auto-fetches Asio)" OFF)
option(BNIO_ENABLE_WARNINGS "Enable compiler warnings for bnio targets" ON)
option(BNIO_ENABLE_TSAN "Build bnio targets with ThreadSanitizer" OFF)
option(BNIO_ENABLE_COVERAGE "Instrument bnio targets for coverage reporting" OFF)
option(BNIO_INSTALL "Generate bnio installation rules" ${BNIO_IS_TOP_LEVEL})
option(BNIO_PACKAGE "Generate CPack packaging rules" OFF)

if(BNIO_INSTALL)
  set(
    BNIO_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/bnio"
    CACHE STRING "Directory in which to install the bnio CMake package")
endif()

if(BNIO_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

bnio_resolve_bexec_dependency()

# ---- Standalone Asio dependency resolution ----
#
# Call bnio_resolve_asio_dependency() to ensure asio::asio is available.
# Tries find_package first; falls back to FetchContent from GitHub.

function(bnio_resolve_asio_dependency)
  if(TARGET asio::asio)
    return()
  endif()

  include(FetchContent)

  find_package(asio CONFIG QUIET)
  if(asio_FOUND)
    message(STATUS "Using system asio (found via find_package)")
    return()
  endif()

  message(STATUS "Fetching standalone Asio from GitHub...")
  FetchContent_Declare(
    asio_fetch
    GIT_REPOSITORY "https://github.com/chriskohlhoff/asio.git"
    GIT_TAG "asio-1-30-2"
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(asio_fetch)

  if(NOT TARGET asio::asio)
    if(TARGET asio)
      add_library(asio::asio ALIAS asio)
    else()
      add_library(asio INTERFACE)
      add_library(asio::asio ALIAS asio)
      target_include_directories(asio INTERFACE
                                 "${asio_fetch_SOURCE_DIR}/asio/include")
      target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
    endif()
  endif()
  message(STATUS "Asio fetched successfully (asio::asio)")
endfunction()

if(BNIO_BUILD_ASIO_EXAMPLES OR BNIO_BUILD_BENCHMARKS)
  bnio_resolve_asio_dependency()
endif()

function(bnio_set_project_warnings target)
  if(NOT BNIO_ENABLE_WARNINGS)
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

if(BNIO_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "BNIO_ENABLE_TSAN requires GCC or Clang")
  endif()
endif()

if(BNIO_ENABLE_COVERAGE)
  if(BNIO_ENABLE_TSAN)
    message(FATAL_ERROR
            "BNIO_ENABLE_COVERAGE cannot be combined with BNIO_ENABLE_TSAN")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
  else()
    message(FATAL_ERROR
            "BNIO_ENABLE_COVERAGE requires GCC, Clang, or AppleClang")
  endif()
endif()

add_subdirectory(include)
add_subdirectory(src)

if(BNIO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BNIO_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(BNIO_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BNIO_INSTALL)
  configure_package_config_file(
    cmake/bnioConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/bnioConfig.cmake"
    INSTALL_DESTINATION "${BNIO_INSTALL_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bnioConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/bnioConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/bnioConfigVersion.cmake"
    DESTINATION "${BNIO_INSTALL_CMAKEDIR}"
    COMPONENT development)

  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    file(
      RELATIVE_PATH BNIO_PC_PREFIX_RELATIVE
      "${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  else()
    file(
      RELATIVE_PATH BNIO_PC_PREFIX_RELATIVE
      "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
  endif()

  set(BNIO_PC_REQUIRES "bexec >= ${BNIO_BEXEC_MIN_VERSION}, openssl")
  if(BNIO_PLATFORM_LINUX)
    string(APPEND BNIO_PC_REQUIRES ", liburing >= 2.1")
  endif()
  if(BUILD_SHARED_LIBS)
    set(BNIO_PC_SHARED_CFLAGS " -DBNIO_SHARED_LIBRARY")
  else()
    set(BNIO_PC_SHARED_CFLAGS "")
  endif()
  configure_file(cmake/bnio.pc.in "${CMAKE_CURRENT_BINARY_DIR}/bnio.pc"
                 @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/bnio.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT development)
  install(FILES LICENSE README.md
          DESTINATION "${CMAKE_INSTALL_DOCDIR}"
          COMPONENT development)
endif()

# ---- CPack packaging ----
if(BNIO_PACKAGE)
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/cpack.cmake")
endif()
