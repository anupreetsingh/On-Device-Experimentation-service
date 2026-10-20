#!/bin/sh
# Installs expd as a per-user launchd agent. launchd creates expd's socket now and starts expd
# on the first connection to it.
#
#   scripts/install-launchd.sh [path/to/expd]
#
# EXP_STATE_DIR (default ~/.expd) and EXPD_LABEL (default com.example.expd) override the
# defaults. Apps must see the same EXP_STATE_DIR to find the socket and snapshot.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
expd=${1:-$repo/build/expd}
state_dir=${EXP_STATE_DIR:-$HOME/.expd}
label=${EXPD_LABEL:-com.example.expd}
plist=$HOME/Library/LaunchAgents/$label.plist

if [ ! -x "$expd" ]; then
  echo "no expd at $expd; build it first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi
expd=$(cd "$(dirname "$expd")" && pwd)/$(basename "$expd")

mkdir -p "$state_dir"
chmod 700 "$state_dir"
mkdir -p "$(dirname "$plist")"
sed -e "s|@LABEL@|$label|g" -e "s|@EXPD@|$expd|g" -e "s|@STATE_DIR@|$state_dir|g" \
  "$repo/deploy/launchd/com.example.expd.plist.in" > "$plist"
plutil -lint "$plist" > /dev/null

launchctl bootout "gui/$(id -u)/$label" 2> /dev/null || true
launchctl bootstrap "gui/$(id -u)" "$plist"
echo "installed $label: expd starts on the first connection to $state_dir/expd.sock"
