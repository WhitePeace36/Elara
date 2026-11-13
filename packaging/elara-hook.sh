#!/bin/sh
# Called by the package scripts of all package formats:
#   elara-hook install   new installation: enable and start elara.service
#   elara-hook upgrade   update or reinstall: start elara.service, or restart
#                        it if it runs (it keeps the old scheduler otherwise);
#                        enabled at boot or not stays as it is
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

# A unit file of an administrator's directory takes precedence over the one of
# the package (/usr/lib/systemd/system), so the service kept running with the
# old unit, and with the old binary if it names one: one left by the install.sh
# of earlier versions, or a copy made by hand. Removed, so the unit of the
# package is the one that counts. Drop-ins (elara.service.d, from systemctl
# edit) stay, and so does a mask (a link to /dev/null, which isn't a file).
LOCAL_UNIT=/etc/systemd/system/elara.service
replace_local_unit() {
    if [ -f "$LOCAL_UNIT" ]; then
        if rm -f "$LOCAL_UNIT"; then
            echo "elara: removed $LOCAL_UNIT, the unit of the package (in /usr/lib/systemd/system) replaces it"
        else
            echo "elara: could not remove $LOCAL_UNIT, it overrides the unit of this package"
        fi
        if [ -e /usr/local/bin/elara ]; then
            echo "elara: /usr/local/bin/elara is an old binary not used any more (the service runs /usr/bin/elara), you can remove it"
        fi
    fi
}

case "$1" in
install)
    replace_local_unit
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
        # Restarted if it runs already (installed another way before): it
        # keeps the old scheduler otherwise.
        systemctl enable elara.service >/dev/null 2>&1 || true
        systemctl restart elara.service || echo "elara: could not start elara.service, see: journalctl -u elara.service"
    else
        systemctl enable elara.service >/dev/null 2>&1 || true
    fi
    ;;
upgrade)
    replace_local_unit
    if live; then
        systemctl daemon-reload || true
        # Starts it if it doesn't run.
        systemctl restart elara.service || echo "elara: could not start elara.service, see: journalctl -u elara.service"
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
