# OAuth and repository permissions

`atperson` should use OAuth for a human-operated entity rather than treating an
app password as its long-term identity credential. The protocol implementation
belongs in Wolfram; this document defines the atperson integration boundary.

## Localhost flow

The planned command is a local interactive authorization flow:

1. atperson starts a loopback HTTP listener on `127.0.0.1` using an ephemeral
   port;
2. it uses the loopback URL as the OAuth redirect URI and opens the browser;
3. the callback is accepted only once, with the state, issuer, PKCE and DPoP
   checks performed by Wolfram;
4. the resulting OAuth session is stored in the runtime data directory with
   owner-only permissions, separately from the learned model, social observation
   ledger and protocol evidence ledger;
5. sync and explicitly approved publishing reuse that session and refresh it
   through Wolfram.

The callback listener must bind to loopback only, use a single-use state, stop
after the first valid callback (or a short timeout), and never log authorization
codes, access tokens, refresh tokens or DPoP private material.

## Scope policy

The requested scope is configurable, but the default must be the smallest set
needed by the enabled operation. For normal public-record publishing this is a
collection-specific `repo:<collection>` permission set.

`repo:*` means all public repository record types and actions. It is suitable
only for an explicit operator choice when the entity genuinely needs arbitrary
record access. It must be displayed clearly before authorization and never be
silently inferred from a generic “repository access” setting.

`account:repo?action=manage` is intentionally not part of the normal entity
scope. It authorizes whole-repository CAR import/migration and is materially
different from ordinary record reads and writes. It should require a separate,
purpose-specific command and confirmation if it is ever implemented.

## Learning and evidence boundary

OAuth proves authorization for the scopes the authorization server granted; it
does not prove that protocol data is correct, verified or safe to learn.

OAuth session state therefore has no authority in the protocol-learning model:

- authorization codes, tokens, refresh state and DPoP private keys are runtime
  credentials only and must never be copied into either durable learning ledger;
- a successful login must not satisfy a protocol capability gate or change
  verification state;
- data acquired through an authenticated Wolfram session must still cross the
  normal protocol evidence boundary with its source, event type, provenance and
  verification result;
- requested or granted scopes are operator/runtime policy, not learned
  preference and not permission for the social model to publish.

The protocol evidence and capability contract is defined in
[`docs/protocol-learning.md`](protocol-learning.md).

## Current status

Wolfram already provides OAuth metadata discovery, PKCE, PAR, DPoP, callback
validation, token exchange and session serialization. atperson exposes
read-only OAuth planning/metadata inspection through the `protocol` command
family, but the runtime still uses the Wolfram app-password agent wrapper for
authenticated sync/publishing. The next implementation slice is the loopback
callback/session adapter and a session-backed `WolframSession` path.

Until that adapter lands, `ATPERSON_APP_PASSWORD` remains the supported
runtime credential and OAuth tokens must not be copied into that variable.
