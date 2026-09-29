#!/bin/sh
# `outbound proposals` is the operator's read-only review of frozen scheduler
# proposals: it lists digest/kind/text with the policy and control-gate verdicts
# and must approve, admit, consume and execute nothing.
set -eu
BIN=$1
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
export ATPERSON_HOME=$DIR ATPERSON_TRAINING_HOME=$DIR/training
export ATPERSON_SCHEDULER_PROPOSALS=$DIR/proposals
unset ATPERSON_LEDGER ATPERSON_STATE

# Nothing pending.
"$BIN" outbound proposals | grep -q "no pending scheduler proposals" \
    || { echo "expected empty message"; exit 1; }

mkdir -p "$DIR/proposals"
LONG=$(printf 'w%.0s' $(seq 1 200))
cat >"$DIR/proposals/0123456789abcdef.json" <<JSON
{"format":"atperson-outbound-action","version":1,"kind":"post","text":"$LONG","rkey":"3kabc","created_at":"2026-09-17T00:00:00Z","digest":"0123456789abcdef"}
JSON
cat >"$DIR/proposals/fedcba9876543210.json" <<'JSON'
{"format":"atperson-outbound-action","version":1,"kind":"like","rkey":"3kdef","created_at":"2026-09-17T00:00:01Z","digest":"fedcba9876543210","subject":"at://did:plc:x/app.bsky.feed.post/abc"}
JSON
printf 'not json' >"$DIR/proposals/broken.json"

before_control=$(cat "$DIR/control.json" 2>/dev/null || true)

"$BIN" outbound proposals >"$DIR/list.out"
grep -q "^0123456789abcdef post rkey 3kabc" "$DIR/list.out" || { echo "missing post"; cat "$DIR/list.out"; exit 1; }
grep -q "^fedcba9876543210 like rkey 3kdef" "$DIR/list.out" || { echo "missing like"; cat "$DIR/list.out"; exit 1; }
grep -q "subject: at://did:plc:x/app.bsky.feed.post/abc" "$DIR/list.out" || { echo "missing subject"; exit 1; }
grep -q "broken.json: unreadable" "$DIR/list.out" || { echo "unreadable file not reported"; exit 1; }
grep -q "policy: deny (kind_disabled)" "$DIR/list.out" || { echo "missing policy verdict"; cat "$DIR/list.out"; exit 1; }
grep -q "control gate: blocked" "$DIR/list.out" || { echo "missing control verdict"; cat "$DIR/list.out"; exit 1; }
grep -q "3 pending proposal(s)" "$DIR/list.out" || { echo "missing summary"; cat "$DIR/list.out"; exit 1; }
# Long text is previewed in the list...
grep -q '\.\.\. (200 bytes' "$DIR/list.out" || { echo "long text not previewed"; exit 1; }

# ...and shown in full for a single digest.
"$BIN" outbound proposals 0123456789abcdef >"$DIR/one.out"
grep -q "text: $LONG" "$DIR/one.out" || { echo "full text missing"; exit 1; }
if grep -q "fedcba9876543210" "$DIR/one.out"; then echo "digest filter leaked another proposal"; exit 1; fi
"$BIN" outbound proposals 0000000000000000 | grep -q "no pending scheduler proposal with that digest" \
    || { echo "expected no-match message"; exit 1; }

# Read-only: every file is still there and nothing was approved.
[ "$(ls "$DIR/proposals" | wc -l | tr -d ' ')" = "3" ] || { echo "proposals were consumed"; exit 1; }
after_control=$(cat "$DIR/control.json" 2>/dev/null || true)
[ "$before_control" = "$after_control" ] || { echo "control state changed"; exit 1; }
if "$BIN" control status | grep -q "0123456789abcdef"; then echo "digest was approved"; exit 1; fi
