#!/usr/bin/env bash
#
# scripts/build_all.sh - build the user-space app, the IPC client and the
# kernel module, then run the self test.
#
# The kernel module step is skipped (with a notice) when the kernel headers
# for the running kernel are not installed, so this script also works on a
# machine where only the user-space part is being evaluated.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> user-space application"
make app || exit 1

echo
echo "==> POSIX IPC client"
make ipc-client || exit 1

echo
echo "==> self test"
make test || exit 1
./build/selftest || exit 1

echo
echo "==> kernel module"
if [[ -d "/lib/modules/$(uname -r)/build" ]]; then
    make driver || exit 1
    ls -l driver/smart_meter_driver.ko
else
    echo "skipped: /lib/modules/$(uname -r)/build is missing."
    echo "install the headers first:  sudo apt install linux-headers-$(uname -r)"
fi

echo
echo "all build steps finished"
