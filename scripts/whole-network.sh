#!/bin/sh
# Ingest the public network through Jetstream in bounded chunks.
#
# `atperson jetstream` saves the model and resume cursor when a run ends, so a
# crash or Ctrl-C loses at most one chunk of model updates (the observation
# ledger is durable per event; `atperson rebuild` recovers the rest).
#
# Usage: scripts/whole-network.sh [atperson-binary] [chunk-seconds] [max-hours]
#   max-hours  total wall-clock limit (default 1; 0 = run until stopped)
# Stop early with Ctrl-C, or `atperson control pause` (the loop then idles and
# resumes after `atperson control resume`).
# Load settings first: set -a; . "$HOME/.ewanc26/atperson/.env"; set +a
BIN=${1:-./build/atperson}
CHUNK_S=${2:-300}
MAX_H=${3:-1}
CHUNK_MS=$((CHUNK_S * 1000))
END=0
[ "$MAX_H" -gt 0 ] && END=$(( $(date +%s) + MAX_H * 3600 ))
trap 'exit 0' INT TERM
while [ "$END" -eq 0 ] || [ "$(date +%s)" -lt "$END" ]; do
    if "$BIN" control 2>/dev/null | grep -q '^paused: yes'; then
        sleep 30
        continue
    fi
    "$BIN" jetstream 1000000000 "$CHUNK_MS" || {
        echo "whole-network: run failed ($?), retrying in 15s" >&2
        sleep 15
    }
done
echo "whole-network: time limit reached, stopping"
