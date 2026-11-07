# 3.20: DEPFILE of add_custom_command with Makefile generators
# (find_program(... REQUIRED) already needs 3.18)
cmake_minimum_required(VERSION 3.20)
# The loader is built with clang and lld (-flto=thin, -fuse-ld=lld, see
# source/CMakeLists.txt), the BPF part needs clang anyway. Another compiler can
# still be chosen with CC/CXX or CMAKE_<LANG>_COMPILER.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  set(CMAKE_CXX_COMPILER clang++)
endif()

project(elara LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Installed like a distribution package (/usr/bin/elara, the unit in
# /usr/lib/systemd/system), not under /usr/local, unless a prefix is given.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

# Version of the packages: 0.r<commits>.g<commit>, like the PKGBUILD's pkgver
# (increases with every commit, sorts correctly for pacman, dpkg and rpm).
set(ELARA_VERSION 0)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE ELARA_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE ELARA_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(ELARA_GIT_COUNT AND ELARA_GIT_HASH)
    set(ELARA_VERSION 0.r${ELARA_GIT_COUNT}.g${ELARA_GIT_HASH})
  endif()
endif()
message(STATUS "elara version: ${ELARA_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)

option(ELARA_NATIVE_ARCH "Build the loader with -march=native (binary only runs on cpus like the build machine)" OFF)

find_program(CLANG_EXECUTABLE clang REQUIRED)
find_program(BPFTOOL_EXECUTABLE bpftool REQUIRED)

execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE HOST_MACHINE_ARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
string(TOLOWER "${HOST_MACHINE_ARCH}" HOST_MACHINE_ARCH)

if(HOST_MACHINE_ARCH MATCHES "^(x86_64|i[3-6]86|x86)$")
    set(BPF_TARGET_ARCH "x86")
elseif(HOST_MACHINE_ARCH MATCHES "^(aarch64|arm64)$")
    set(BPF_TARGET_ARCH "arm64")
elseif(HOST_MACHINE_ARCH MATCHES "^arm")
    set(BPF_TARGET_ARCH "arm")
elseif(HOST_MACHINE_ARCH MATCHES "^riscv")
    set(BPF_TARGET_ARCH "riscv")
elseif(HOST_MACHINE_ARCH MATCHES "^(ppc|powerpc)")
    set(BPF_TARGET_ARCH "powerpc")
elseif(HOST_MACHINE_ARCH MATCHES "^s390")
    set(BPF_TARGET_ARCH "s390")
elseif(HOST_MACHINE_ARCH MATCHES "^mips")
    set(BPF_TARGET_ARCH "mips")
elseif(HOST_MACHINE_ARCH MATCHES "^sparc")
    set(BPF_TARGET_ARCH "sparc")
elseif(HOST_MACHINE_ARCH MATCHES "^loongarch")
    set(BPF_TARGET_ARCH "loongarch")
else()
    message(FATAL_ERROR "Unsupported machine arch '${HOST_MACHINE_ARCH}' for BPF __TARGET_ARCH mapping")
endif()

message(STATUS "BPF target arch: ${BPF_TARGET_ARCH} (host: ${HOST_MACHINE_ARCH})")


add_subdirectory(source)

include(packaging/packaging.cmake)
