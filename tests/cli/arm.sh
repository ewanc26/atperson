#!/bin/sh
# `atperson autonomy arm | preflight | disarm`: the one-time setup for
# unattended operation. A fresh install is not ready and stays fail-closed; a dry
# run writes nothing; --apply makes the preflight pass; disarm undoes it; bad
# input is refused without touching state.
set -eu
BIN=$1
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
export ATPERSON_HOME=$DIR ATPERSON_TRAINING_HOME=$DIR/training
unset ATPERSON_LEDGER ATPERSON_STATE ATPERSON_SCHEDULER ATPERSON_ALLOW_EXTERNAL_PUBLISHING \
    ATPERSON_IDENTIFIER ATPERSON_APP_PASSWORD ATPERSON_SELF_DID

READY_ENV="ATPERSON_SCHEDULER=1 ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true ATPERSON_IDENTIFIER=x ATPERSON_APP_PASSWORD=y ATPERSON_SELF_DID=did:plc:self"

# A fresh install is not ready, and says why.
if "$BIN" autonomy preflight >"$DIR/fresh.out" 2>&1; then
    echo "a fresh install must not be ready"; cat "$DIR/fresh.out"; exit 1
fi
grep -q "NOT READY" "$DIR/fresh.out" || { echo "expected NOT READY"; cat "$DIR/fresh.out"; exit 1; }
grep -q "FAIL\] scheduler" "$DIR/fresh.out" || { echo "expected scheduler blocker"; exit 1; }
grep -q "FAIL\] policy" "$DIR/fresh.out" || { echo "expected policy blocker"; exit 1; }

# A dry run prints the plan and the environment lines and writes nothing.
"$BIN" autonomy arm --kinds post:3/1d,like:20/1h --scope moon --min-plan 0.3 >"$DIR/plan.out"
grep -q "dry run; nothing is written" "$DIR/plan.out" || { echo "expected dry-run banner"; cat "$DIR/plan.out"; exit 1; }
grep -q "enable post at 3 per 1d" "$DIR/plan.out" || { echo "missing post plan"; exit 1; }
grep -q "ATPERSON_SCHEDULER=1" "$DIR/plan.out" || { echo "missing env lines"; exit 1; }
[ ! -e "$DIR/outbound-policy.json" ] || { echo "dry run wrote a policy"; exit 1; }
[ ! -d "$DIR/envelopes" ] || [ -z "$(ls "$DIR/envelopes")" ] || { echo "dry run wrote an envelope"; exit 1; }

# Refused input changes nothing.
for bad in "--kinds" "--kinds post" "--kinds post:0/1d" "--kinds post:3/0" "--kinds bogus:3/1d" \
           "--kinds unfollow:3/1d" "--kinds post:3/1d,post:4/1d" "--kinds post:3/1d --min-plan 2" \
           "--kinds post:3/1d --expires nonsense" "--kinds post:3/1d --id Bad_Id" \
           "--kinds post:3/1d --valence-guard 0.5" "--kinds post:3/1d --nope"; do
    # shellcheck disable=SC2086
    if "$BIN" autonomy arm $bad --apply >/dev/null 2>&1; then
        echo "arm accepted bad input: $bad"; exit 1
    fi
done
[ ! -e "$DIR/outbound-policy.json" ] || { echo "a refused arm wrote a policy"; exit 1; }
if "$BIN" autonomy arm --apply >/dev/null 2>&1; then echo "arm without --kinds must fail"; exit 1; fi

# Apply: everything is written, and the preflight passes with the environment.
env $READY_ENV "$BIN" autonomy arm --kinds post:3/1d,like:20/1h --scope moon --min-plan 0.3 \
    --graduated-likes --apply >"$DIR/apply.out"
grep -q "^written:" "$DIR/apply.out" || { echo "expected written line"; cat "$DIR/apply.out"; exit 1; }
grep -q "ATPERSON_SCHEDULER_GRADUATED_LIKES=1" "$DIR/apply.out" || { echo "missing optional env line"; exit 1; }
grep -q "READY. The daemon will act" "$DIR/apply.out" || { echo "expected READY after apply"; cat "$DIR/apply.out"; exit 1; }
[ -f "$DIR/envelopes/autonomy.json" ] && [ -f "$DIR/envelopes/autonomy-actions.json" ] \
    || { echo "expected the two envelopes"; ls "$DIR/envelopes"; exit 1; }
env $READY_ENV "$BIN" autonomy preflight >"$DIR/ready.out" || { echo "preflight should pass"; cat "$DIR/ready.out"; exit 1; }
grep -q "output-guard" "$DIR/ready.out" || { echo "preflight should report the output guard"; cat "$DIR/ready.out"; exit 1; }
printf 'alpha\n' >"$DIR/output-denylist.txt"
env $READY_ENV "$BIN" autonomy preflight | grep -q "denylist has 1 term" || { echo "preflight should count denylist terms"; exit 1; }
grep -q "post: at most 3 per 1d" "$DIR/ready.out" || { echo "missing post bound"; exit 1; }
grep -q "like: at most 20 per 1h" "$DIR/ready.out" || { echo "missing like bound"; exit 1; }

# Without the environment the same state is not ready: setup on disk is not enough.
if "$BIN" autonomy preflight >/dev/null 2>&1; then echo "preflight passed without the environment"; exit 1; fi

# The operator's kill switches keep working.
"$BIN" control pause >/dev/null
if env $READY_ENV "$BIN" autonomy preflight >/dev/null 2>&1; then echo "paused must not be ready"; exit 1; fi
"$BIN" control resume >/dev/null
env $READY_ENV "$BIN" autonomy preflight >/dev/null || { echo "resume should be ready again"; exit 1; }

# Disarm: writes off, envelopes gone, no longer ready.
"$BIN" autonomy disarm >"$DIR/disarm.out"
grep -q "disarmed" "$DIR/disarm.out" || { echo "expected disarmed"; cat "$DIR/disarm.out"; exit 1; }
[ -z "$(ls "$DIR/envelopes")" ] || { echo "envelopes should be revoked"; exit 1; }
if env $READY_ENV "$BIN" autonomy preflight >/dev/null 2>&1; then echo "disarmed must not be ready"; exit 1; fi
"$BIN" autonomy disarm >/dev/null || { echo "disarm must be idempotent"; exit 1; }

# The circuit breaker is visible and can be reset by hand.
"$BIN" autonomy breaker >"$DIR/breaker.out"
grep -q "circuit breaker: closed" "$DIR/breaker.out" || { echo "expected a closed breaker"; cat "$DIR/breaker.out"; exit 1; }
cat >"$DIR/scheduler-breaker.json" <<JSON
{"version":1,"consecutive_failures":3,"open_until":99999999999,"cooldown_seconds":900,"trips":2,"last_failure_at":1789639200,"last_failure_detail":"write_failed: network down","proposal_failures":{"abc":1}}
JSON
"$BIN" autonomy breaker status >"$DIR/breaker-open.out"
grep -q "circuit breaker: open" "$DIR/breaker-open.out" || { echo "expected an open breaker"; cat "$DIR/breaker-open.out"; exit 1; }
grep -q "network down" "$DIR/breaker-open.out" || { echo "expected the last failure"; exit 1; }
env $READY_ENV "$BIN" autonomy arm --kinds post:3/1d --apply >"$DIR/rearm.out"
grep -q "circuit-breaker" "$DIR/rearm.out" || { echo "preflight should show the breaker"; cat "$DIR/rearm.out"; exit 1; }
"$BIN" autonomy breaker reset >/dev/null
[ ! -e "$DIR/scheduler-breaker.json" ] || { echo "reset should clear the state"; exit 1; }
"$BIN" autonomy breaker | grep -q "circuit breaker: closed" || { echo "closed after reset"; exit 1; }
if "$BIN" autonomy breaker bogus >/dev/null 2>&1; then echo "bad breaker subcommand accepted"; exit 1; fi

# Engagement consent: the preflight reports it and catches a broken opt-out list.
env $READY_ENV "$BIN" autonomy arm --kinds like:5/1h --engagement open --apply >"$DIR/eng.out"
grep -q "ATPERSON_ENGAGEMENT=open" "$DIR/eng.out" || { echo "expected the engagement env line"; cat "$DIR/eng.out"; exit 1; }
grep -q "engagement" "$DIR/eng.out" || { echo "preflight should report engagement"; exit 1; }
printf 'did:plc:one\nalice.bsky.social\n' >"$DIR/do-not-engage.txt"
if env $READY_ENV "$BIN" autonomy preflight >"$DIR/eng-bad.out" 2>&1; then echo "a broken opt-out list must block"; cat "$DIR/eng-bad.out"; exit 1; fi
grep -q "NOT excluded" "$DIR/eng-bad.out" || { echo "expected the opt-out warning"; cat "$DIR/eng-bad.out"; exit 1; }
printf 'did:plc:one\n' >"$DIR/do-not-engage.txt"
env $READY_ENV "$BIN" autonomy preflight >/dev/null || { echo "a valid opt-out list should be ready"; exit 1; }
if "$BIN" autonomy arm --kinds like:5/1h --engagement everyone >/dev/null 2>&1; then echo "bad --engagement accepted"; exit 1; fi
