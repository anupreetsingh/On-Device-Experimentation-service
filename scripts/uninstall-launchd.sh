#!/bin/sh
# Stops expd (SIGTERM, so it flushes its exposures) and removes the launchd agent. The state
# directory (device ID, config, snapshot, exposure log) is left in place.
set -eu

label=${EXPD_LABEL:-com.example.expd}
launchctl bootout "gui/$(id -u)/$label" 2> /dev/null || true
rm -f "$HOME/Library/LaunchAgents/$label.plist"
echo "removed $label; state kept in ${EXP_STATE_DIR:-$HOME/.expd}"
