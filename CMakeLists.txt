cmake_minimum_required(VERSION 3.16)
project(lunar LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)

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
