#!/bin/sh
# Removes what install.sh installed: sh /data/midimixd/uninstall.sh (as root on the MPC). The
# Midimix's LEDs go off. MPC isn't restarted. ROOT= and SYSTEMCTL= as in install.sh.
set -eu

ROOT=${ROOT:-}
SYSTEMCTL=${SYSTEMCTL:-systemctl}
dir=$ROOT/data/midimixd
units=$ROOT/etc/systemd/system

# The hook first, so that an MPC start in between doesn't start midimixd again.
rm -f "$units/acvs.service.d/91-midimixd.conf"
$SYSTEMCTL daemon-reload
$SYSTEMCTL stop midimixd.service 2>/dev/null || true
rm -f "$units/midimixd.service"
$SYSTEMCTL daemon-reload
$SYSTEMCTL reset-failed midimixd.service 2>/dev/null || true

rm -f "$dir/midimixd" "$dir/midimixd.new" "$dir/uninstall.sh"
if rmdir "$dir" 2>/dev/null; then
    echo "Removed midimixd."
else
    echo "Removed midimixd; kept $dir, which holds other files." >&2
fi
