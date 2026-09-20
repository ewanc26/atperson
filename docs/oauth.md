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
   owner-only permissions, separately from the learned model and ledger;
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

## Current status

Wolfram already provides OAuth metadata discovery, PKCE, PAR, DPoP, callback
validation, token exchange and session serialization. atperson still uses the
Wolfram app-password agent wrapper, so the next implementation slice is the
loopback callback/session adapter and a session-backed `WolframSession` path.
Until that adapter lands, `ATPERSON_APP_PASSWORD` remains the supported
runtime credential and OAuth tokens must not be copied into that variable.
