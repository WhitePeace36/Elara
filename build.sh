#!/bin/bash

bpftool btf dump file /sys/kernel/btf/vmlinux format c > ./include/vmlinux.h
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j
