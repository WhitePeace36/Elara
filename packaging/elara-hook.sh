#!/bin/sh
# Called by the package scripts of all package formats:
#   elara-hook install   new installation: enable and start elara.service
#   elara-hook upgrade   restart a running elara (it keeps the old scheduler
#                        otherwise), leave enabled or not as it is
#   elara-hook remove    stop and disable elara.service
#
# Only one sched_ext scheduler can be loaded at a time. Enabled at boot next
# to elara, scx.service or scx_loader.service (which loads its default
# scheduler when it starts at boot) would get there first and keep elara from
# loading, so they are disabled on install and enabled again on remove (only
# those this script disabled). Running ones are stopped on install.

STATE_DIR=/var/lib/elara
STATE=$STATE_DIR/disabled-units
OTHERS="scx.service scx_loader.service"

# systemd runs (not a chroot or an image being built): units can be started.
live() { [ -d /run/systemd/system ]; }

case "$1" in
install)
    # Left by the install.sh of earlier versions: it takes precedence over
    # the unit of the package and starts the old binary.
    if [ -f /etc/systemd/system/elara.service ]; then
        echo "elara: /etc/systemd/system/elara.service (from the old install.sh) overrides the unit of this package."
        echo "elara: remove it and the old binary: rm /etc/systemd/system/elara.service /usr/local/bin/elara; systemctl daemon-reload; systemctl restart elara.service"
    fi
    live && systemctl daemon-reload || true
    disabled=""
    for unit in $OTHERS; do
        if systemctl is-enabled --quiet "$unit" 2>/dev/null; then
            systemctl disable "$unit" >/dev/null 2>&1 || true
            disabled="$disabled $unit"
        fi
    done
    if [ -n "$disabled" ]; then
        mkdir -p "$STATE_DIR"
        printf '%s\n' $disabled > "$STATE"
        echo "elara: disabled$disabled (only one sched_ext scheduler can run), enabled again when elara is removed"
    fi
    if live; then
        # Whatever scheduler they run is in the way (elara.service has no
        # Conflicts= with them, see there).
        systemctl stop $OTHERS >/dev/null 2>&1 || true
        systemctl enable --now elara.service || echo "elara: could not start elara.service, see: journalctl -u elara.service"
    else
        systemctl enable elara.service >/dev/null 2>&1 || true
    fi
    ;;
upgrade)
    if live; then
        systemctl daemon-reload || true
        systemctl try-restart elara.service || true
    fi
    ;;
remove)
    if live; then
        systemctl disable --now elara.service >/dev/null 2>&1 || true
    else
        systemctl disable elara.service >/dev/null 2>&1 || true
    fi
    if [ -f "$STATE" ]; then
        for unit in $(cat "$STATE"); do
            if live; then
                systemctl enable --now "$unit" >/dev/null 2>&1 || true
            else
                systemctl enable "$unit" >/dev/null 2>&1 || true
            fi
            echo "elara: enabled $unit again"
        done
        rm -f "$STATE"
        rmdir "$STATE_DIR" 2>/dev/null || true
    fi
    ;;
esac
exit 0
