#!/bin/sh
# Regression: size-capped rotation through the CLI. With no cap `rotate` is a
# no-op; with a ledger cap it releases raw text but keeps identity and dedup,
# and a released observation is forgotten by `rebuild`, not re-learned.
set -eu
BIN=$1
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
export ATPERSON_HOME=$DIR ATPERSON_TRAINING_HOME=$DIR/training
unset ATPERSON_LEDGER ATPERSON_STATE ATPERSON_LEDGER_MAX_BYTES ATPERSON_MODEL_MAX_BYTES

i=0
while [ "$i" -lt 8 ]; do
    "$BIN" ingest "harbour lantern number $i drifts over the quiet grey water tonight" "local:r$i" >/dev/null
    i=$((i + 1))
done

# No cap configured: refuses to guess a bound.
if "$BIN" rotate >"$DIR/none.out" 2>&1; then
    echo "rotate without a cap should not succeed"; cat "$DIR/none.out"; exit 1
fi
grep -q "MAX_BYTES" "$DIR/none.out" || { echo "expected cap hint"; cat "$DIR/none.out"; exit 1; }

before=$("$BIN" stats)
echo "$before" | grep -q "^observations: 8$" || { echo "expected 8 observations"; echo "$before"; exit 1; }
size=$(wc -c <"$DIR/training/ledger.bin")

# A cap far below the payload total releases text, never entries.
ATPERSON_LEDGER_MAX_BYTES=$((size / 2)) "$BIN" rotate >"$DIR/rot.out"
grep -q "released" "$DIR/rot.out" || { echo "expected released payloads"; cat "$DIR/rot.out"; exit 1; }
after_size=$(wc -c <"$DIR/training/ledger.bin")
[ "$after_size" -lt "$size" ] || { echo "ledger did not shrink"; exit 1; }

# Identity and dedup survive: the same content is not learned again.
"$BIN" ingest "harbour lantern number 0 drifts over the quiet grey water tonight" local:r0 | grep -q "already learned" \
    || { echo "released observation was re-learned"; exit 1; }

# A second rotation under the same cap releases nothing more.
ATPERSON_LEDGER_MAX_BYTES=$((size / 2)) "$BIN" rotate >"$DIR/rot2.out"
if grep -q "released [1-9]" "$DIR/rot2.out"; then
    echo "second rotate released more payloads"; cat "$DIR/rot2.out"; exit 1
fi

# Rebuild succeeds and reports the released observations as forgotten.
"$BIN" rebuild >"$DIR/rebuild.out"
grep -q "released" "$DIR/rebuild.out" || { echo "rebuild did not report released"; cat "$DIR/rebuild.out"; exit 1; }

# A model cap below the ledger mirror cannot be reached by pruning; it must be
# refused with a warning, not chased by eroding the vocabulary.
nodes_before=$("$BIN" stats | sed -n 's/^nodes: //p')
ATPERSON_MODEL_MAX_BYTES=1 "$BIN" rotate >"$DIR/model.out"
grep -q "not pruning vocabulary" "$DIR/model.out" || { echo "expected floor warning"; cat "$DIR/model.out"; exit 1; }
nodes_after=$("$BIN" stats | sed -n 's/^nodes: //p')
[ "$nodes_before" = "$nodes_after" ] || { echo "vocabulary eroded: $nodes_before -> $nodes_after"; exit 1; }
