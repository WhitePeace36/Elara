# Tests of the topology detection of the loader (source/elara_topology.h). They
# read made-up sysfs trees from a temporary directory: no BPF, no root.
add_executable(topology_test topology_test.cpp)
target_include_directories(topology_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_options(topology_test PRIVATE -Wall -Wextra)
add_test(NAME topology COMMAND topology_test)
