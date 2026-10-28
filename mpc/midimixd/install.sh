#!/bin/sh
# Installs or upgrades midimixd on the MPC (as root, from a folder holding this script, midimixd,
# midimixd.service, acvs-midimixd.conf and uninstall.sh; `make -C mpc install` does it all):
#
#   /data/midimixd/midimixd, uninstall.sh                 the daemon, and what removes it later
#   /etc/systemd/system/midimixd.service                  runs it, restarts it if it dies
#   /etc/systemd/system/acvs.service.d/91-midimixd.conf   starts it whenever MPC starts
#
# Why the hook in MPC's unit rather than `systemctl enable`: /etc is an overlay (upper layer on /data)
# that systemd mounts after reading its units at boot, so a unit enabled there isn't started at boot.
# The addins' mpc-addins-reload.service (mpc-vst-plugins' docs/ADDINS.md) reloads systemd before MPC
# starts, so the hook is seen by then. Without that reload (no addin installed) midimixd only starts
# by hand after a reboot: `systemctl start midimixd`.
#
# MPC isn't restarted. ROOT= installs into a scratch tree, SYSTEMCTL= replaces systemctl (tests).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
ROOT=${ROOT:-}
SYSTEMCTL=${SYSTEMCTL:-systemctl}
dir=$ROOT/data/midimixd
units=$ROOT/etc/systemd/system

for f in midimixd midimixd.service acvs-midimixd.conf uninstall.sh; do
    [ -f "$here/$f" ] || { echo "install.sh: $f should be next to it" >&2; exit 1; }
done

mkdir -p "$dir" "$units/acvs.service.d"
# Staged, then renamed over the running binary.
cp "$here/midimixd" "$dir/midimixd.new"
chmod 755 "$dir/midimixd.new"
mv -f "$dir/midimixd.new" "$dir/midimixd"
cp "$here/uninstall.sh" "$dir/uninstall.sh"
cp "$here/midimixd.service" "$units/midimixd.service"
cp "$here/acvs-midimixd.conf" "$units/acvs.service.d/91-midimixd.conf"

$SYSTEMCTL daemon-reload
$SYSTEMCTL restart midimixd.service
echo "Installed midimixd in $dir; it starts with MPC. Log: journalctl -u midimixd"

if [ ! -f "$ROOT/usr/lib/systemd/system/mpc-addins-reload.service" ]; then
    echo "No mpc-addins-reload.service: after a reboot midimixd won't start until 'systemctl start midimixd'." >&2
fi
sleep 1
if ! $SYSTEMCTL is-active --quiet midimixd.service; then
    echo "midimixd isn't running: another one started by hand? (exit status 3: journalctl -u midimixd)" >&2
    exit 1
fi
