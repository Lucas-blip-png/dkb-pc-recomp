#!/bin/sh
# Starts the game the setup built into game/ beside this file. RecompLauncher starts this (recomplauncher.json), so its
# start program is in the package from the first moment, before the setup has built game/dkb.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
if [ ! -x "$here/game/dkb" ]; then
    echo "game/dkb is not built yet: run ./setup.sh with --dest $here/game first (README-SETUP.txt)." >&2
    exit 1
fi
cd "$here/game"
exec ./dkb "$@"
