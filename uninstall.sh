set -u

# --- colors (degrade gracefully when output isn't a terminal) ---
if [ -t 1 ]; then
    GREEN=$'\033[0;32m'; RED=$'\033[0;31m'; YELLOW=$'\033[0;33m'; RESET=$'\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; RESET=''
fi

# run_step "description" [--optional] command args...
#   logs [OK] on success, [FAIL] + exit on failure.
#   --optional turns a failure into a non-fatal [SKIP] -- used for every
#   teardown step so cleanup keeps going even if a piece is already gone.
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

run_step "Disable and stop lunar.service"   --optional sudo systemctl disable --now lunar.service
run_step "Remove service unit"              --optional sudo rm /etc/systemd/system/lunar.service
run_step "Remove binary from /usr/local/bin" --optional sudo rm /usr/local/bin/lunar
run_step "Reload systemd units"             --optional sudo systemctl daemon-reload

echo "${GREEN}Teardown complete.${RESET}"

