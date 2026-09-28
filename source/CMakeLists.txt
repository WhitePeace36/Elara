set(BPF_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lunar.bpf.c
)
set(BPF_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/defines.h
    ${CMAKE_CURRENT_SOURCE_DIR}/datatypes.h
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatches.h
)

set(BPF_OBJ
    ${CMAKE_CURRENT_BINARY_DIR}/lunar.bpf.o
)
set(BPF_SKEL
    ${CMAKE_CURRENT_BINARY_DIR}/lunar.skel.h
)

add_custom_command(
    OUTPUT ${BPF_OBJ}
    COMMAND ${CLANG_EXECUTABLE}
        -target bpf -g -O2 -Wall -Wno-compare-distinct-pointer-types -mcpu=v3 -D__TARGET_ARCH_${BPF_TARGET_ARCH} -mlittle-endian -idirafter
        -I${SCX_INCLUDE}
        -I${CMAKE_CURRENT_SOURCE_DIR}
        -I${CMAKE_SOURCE_DIR}
        ${LIBBPF_CFLAGS_OTHER}
        -c ${BPF_SRC} -o ${BPF_OBJ}
    DEPENDS ${BPF_SRC}
            ${BPF_HEADER}
    COMMENT "Compiling BPF scheduler"
    VERBATIM
)

set(lunarSource
    main.cpp
)

set(lunarHeader
    datatypes.h
    defines.h
    dispatches.h
    helpers.h
    lunar_topology.h
)
add_custom_command(
    OUTPUT ${BPF_SKEL}
    COMMAND sh -c "${BPFTOOL_EXECUTABLE} gen skeleton ${BPF_OBJ} > ${BPF_SKEL}"
    DEPENDS ${BPF_OBJ}
    COMMENT "Generating BPF skeleton"
    VERBATIM
)

add_custom_target(lunar_bpf DEPENDS ${BPF_OBJ} ${BPF_SKEL})

set_target_properties(lunar_bpf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(lunar
    ${lunarSource}
    ${lunarHeader}
)

add_dependencies(lunar lunar_bpf)

target_include_directories(lunar PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}
    ${LIBBPF_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/scx
)

target_link_libraries(lunar PRIVATE ${LIBBPF_LIBRARIES})

target_compile_options(lunar PRIVATE
    -Wall -Wextra -Wpedantic -Wno-overlength-strings -fno-strict-aliasing -fwrapv -O3 -flto=thin -march=native -Wno-unused-parameter -Wno-missing-declarations
)

target_link_options(lunar PRIVATE
    -flto=thin -fuse-ld=lld -march=native
)

set_target_properties(lunar PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})


source_group(SourceFiles FILES ${lunarSource})
source_group(HeaderFiles FILES ${lunarHeader})

install(TARGETS lunar RUNTIME DESTINATION bin)
