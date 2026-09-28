#!/bin/bash
set -u

# --- colors (degrade gracefully when output isn't a terminal) ---
if [ -t 1 ]; then
    GREEN=$'\033[0;32m'; RED=$'\033[0;31m'; YELLOW=$'\033[0;33m'; RESET=$'\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; RESET=''
fi

# run_step "description" [--optional] command args...
#   logs [OK] on success, [FAIL] + exit on failure.
#   --optional turns a failure into a non-fatal [SKIP] (e.g. stopping a
#   service that isn't installed yet on the very first deploy).
run_step() {
    local desc="$1"; shift
    local optional=""
    if [ "${1:-}" = "--optional" ]; then optional="1"; shift; fi

    echo ">> ${desc}..."
    if "$@"; then
        echo "${GREEN}[OK]${RESET}   ${desc}"
    else
        local rc=$?
        if [ -n "$optional" ]; then
            echo "${YELLOW}[SKIP]${RESET} ${desc} (exit ${rc}, continuing)"
        else
            echo "${RED}[FAIL]${RESET} ${desc} (exit ${rc})"
            exit "${rc}"
        fi
    fi
}

run_step "Stop lunar.service"                 --optional sudo systemctl stop lunar.service
run_step "Install binary to /usr/local/bin"   sudo cp build/lunar /usr/local/bin
run_step "Install service unit"               sudo cp lunar.service /etc/systemd/system/
run_step "Reload systemd units"               sudo systemctl daemon-reload
run_step "Enable and start lunar.service"     sudo systemctl enable --now lunar.service
run_step "Check lunar.service status"         sudo systemctl status lunar.service

echo "${GREEN}All steps completed successfully.${RESET}"

