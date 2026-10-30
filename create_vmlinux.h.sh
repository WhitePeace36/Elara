#!/bin/bash
# Regenerate include/vmlinux.h from the running kernel (needs Linux 7.3 or newer,
# see README.md). Dumped to a temporary file first: a failed dump leaves the old
# vmlinux.h as it was.
set -e
cd "$(dirname "$0")"

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$tmp"
chmod 644 "$tmp"
mv "$tmp" ./include/vmlinux.h
