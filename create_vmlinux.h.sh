# /bin/bash

bpftool btf dump file /sys/kernel/btf/vmlinux format c > ./include/vmlinux.h
