#!/bin/sh
# Installs expd as a systemd user service with socket activation: systemd creates
# ~/.expd/expd.sock now and starts expd on the first connection.
#
#   scripts/install-systemd.sh [path/to/expd]
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
expd=${1:-$repo/build/expd}
units=$HOME/.config/systemd/user

if [ ! -x "$expd" ]; then
  echo "no expd at $expd; build it first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi

install -D -m 755 "$expd" "$HOME/.local/bin/expd"
install -D -m 644 "$repo/deploy/systemd/expd.socket" "$units/expd.socket"
install -D -m 644 "$repo/deploy/systemd/expd.service" "$units/expd.service"
systemctl --user daemon-reload
systemctl --user enable --now expd.socket
echo "installed expd.socket: expd starts on the first connection to ~/.expd/expd.sock"
