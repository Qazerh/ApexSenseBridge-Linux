#!/bin/sh
# Run using pkexec for system device access; no daemon is installed.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install -m 0644 "$project_dir/packaging/linux/70-apexsensebridge.rules" /etc/udev/rules.d/70-apexsensebridge.rules
install -m 0644 "$project_dir/packaging/linux/71-apexsensebridge-flydigi.rules" /etc/udev/rules.d/71-apexsensebridge-flydigi.rules
printf '%s\n' uhid hid-playstation > /etc/modules-load.d/apexsensebridge.conf
modprobe uhid
modprobe hid-playstation
udevadm control --reload-rules
udevadm trigger --subsystem-match=misc --sysname-match=uhid
udevadm trigger --subsystem-match=hidraw
udevadm settle
