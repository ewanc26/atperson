#!/bin/sh
# Continuously ingest the public network through Jetstream.
#
# `atperson jetstream` saves the model and the resume cursor when a run ends,
# so the loop uses bounded chunks: a crash or Ctrl-C loses at most one chunk of
# model updates (the observation ledger is durable per event, so
# `atperson rebuild` recovers anything a chunk did not snapshot).
#
# Usage: scripts/whole-network.sh [path-to-atperson] [chunk-seconds]
# Reads ATPERSON_* settings from the environment; source your .env first:
#   set -a; . "$HOME/.ewanc26/atperson/.env"; set +a
BIN=${1:-./build/atperson}
CHUNK_MS=$(( ${2:-300} * 1000 ))
trap 'exit 0' INT TERM
while :; do
    "$BIN" jetstream 1000000000 "$CHUNK_MS" || {
        echo "whole-network: run failed ($?), retrying in 15s" >&2
        sleep 15
    }
done
