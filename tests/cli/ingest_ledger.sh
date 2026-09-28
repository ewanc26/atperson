#!/bin/sh
# Regression: local `ingest`/`ingest-file` must go through the durable ledger.
# Checks dedup by (source, content), episode formation, and that `rebuild`
# reproduces the same learned state instead of dropping local observations.
set -eu
BIN=$1
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
export ATPERSON_HOME=$DIR ATPERSON_TRAINING_HOME=$DIR/training
unset ATPERSON_LEDGER ATPERSON_STATE

"$BIN" ingest "the silver moon rises over the quiet harbour" local:a >/dev/null
"$BIN" ingest "the silver moon rises over the quiet harbour" local:a | grep -q "already learned"
"$BIN" ingest "the quiet harbour holds silver boats" local:b >/dev/null
printf 'cats sleep by the warm window\n' >"$DIR/n.txt"
"$BIN" ingest-file "$DIR/n.txt" >/dev/null

[ -f "$DIR/training/ledger.bin" ] || { echo "no ledger written"; exit 1; }
before=$("$BIN" stats)
echo "$before" | grep -q "^observations: 3$" || { echo "expected 3 observations"; echo "$before"; exit 1; }
echo "$before" | grep -q "^episodes: 3 " || { echo "expected 3 episodes"; echo "$before"; exit 1; }

"$BIN" rebuild >/dev/null
after=$("$BIN" stats)
[ "$before" = "$after" ] || { echo "rebuild changed learned state"; echo "$before"; echo "$after"; exit 1; }
