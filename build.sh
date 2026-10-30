#!/bin/bash
# Stop at the first error: a failed vmlinux.h dump would otherwise build against
# the old one.
set -e

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j
