# Scopes — permissions and parsing

Scopes encode what an access token is allowed to do. AT Proto OAuth scopes are richer than generic OAuth: they include positional parameters, query parameters, and references to external "permission set" lexicons.

Authoritative source: <https://atproto.com/specs/permission>.

## Scope string grammar

```
scope        = resource [ ":" positional ] [ "?" params ]
resource     = "atproto" | "transition" | "account" | "identity" | "blob"
             | "repo" | "rpc" | "include"
             | "openid" | "profile" | "email"         # OIDC, rarely relevant
positional   = URL-encoded bare value
params       = param ("&" param)*
param        = key "=" URL-encoded-value
```

Multiple scopes are joined with single spaces: `"atproto transition:generic repo:app.bsky.feed.post?action=create"`.

## Required baseline

- **`atproto`** — declares the atproto OAuth profile. Mandatory on every session. No parameters.
- The authorize request's `scope` MUST be a subset of the client metadata `scope` field. You can't request scopes you didn't declare.

## Transitional scopes (legacy)

Intended as a migration path from App Passwords. Treat as "everything the old App Password could do, minus account management".

| Scope | Grants |
|---|---|
| `transition:generic` | Broad PDS permissions: write any record type, upload blobs, read/write preferences, most XRPC endpoints, service auth. Excludes account management and `chat.bsky.*`. |
| `transition:chat.bsky` | Adds `chat.bsky.*` Lexicons + service auth. Requires `transition:generic`. |
| `transition:email` | Read account email address and confirmation status via `com.atproto.server.getSession`. |

Goal: deprecate these over time. For new apps, prefer granular scopes; transitional scopes mean "I haven't thought about permissions".

## Granular scopes

### `repo:*` — record writes

```
repo:<collection>[?action=create&action=update&action=delete]
repo:*
```

- `collection` is an NSID (e.g. `app.bsky.feed.post`) or `*` (all collections).
- `action` is optional; if absent, all three actions (create/update/delete) are allowed. Repeat to grant multiple actions.

Examples:

- `repo:app.bsky.feed.post?action=create&action=update` — post + edit posts, but not delete
- `repo:*?action=create` — write any new record in any collection
- `repo:com.example.widget` — full create/update/delete on widgets

Partial wildcards like `repo:app.bsky.*` are NOT supported.

### `rpc:*` — XRPC calls

```
rpc:<lxm>?aud=<did>
rpc:<lxm>?aud=*
rpc:*?aud=<did>
```

- `lxm` is the XRPC method NSID (e.g. `app.bsky.feed.searchPosts`) or `*`.
- `aud` is the target service DID, usually a service endpoint like `did:web:api.bsky.app#bsky_appview`, or `*`.
- At least one of `lxm` or `aud` MUST be a concrete value — both wildcarded is forbidden.

Example: `rpc:app.bsky.feed.searchPosts?aud=did:web:api.bsky.app%23bsky_appview` — call the AppView's search endpoint, nothing else.

### `blob:*` — media upload mime filters

```
blob:<mime-pattern>[&accept=<mime-pattern>...]
blob?accept=image/*&accept=video/mp4
```

- `mime-pattern` is `*/*` (all), `type/*` (image/video/audio wildcard), or `type/subtype` (exact). No `*/subtype`.
- `accept` query param can also be used; either positional or `accept=` works, but not both for the same scope.

### `account:*` — account hosting admin

```
account:<attr>[?action=read|manage]
account:email?action=manage
account:repo?action=read
```

- `attr` is `email`, `repo`, or `status`.
- `action` defaults to `read`; `manage` implies read.

### `identity:*` — handle management

```
identity:handle
identity:*
```

Currently the only attribute is `handle` (or `*`). `identity:handle` is what a handle-changer UI would request.

### `include:*` — permission set reference

```
include:<nsid>[?aud=<did>]
include:com.example.authBasicFeatures?aud=did:web:api.example.com%23svc_appview
```

`include` points at a **permission set** lexicon published elsewhere. Authorization server dereferences it (with caching) and expands it into the granular scopes it contains.

See `permission-sets.md` on atproto.com for the lexicon shape.

## Percent-encoding

Scope values are percent-encoded within the scope string where needed. The canonical percent-encoding hazards:

- `#` in an `aud` service reference MUST be `%23`: `aud=did:web:api.example.com%23svc_appview`.
- `&` and `=` within values likewise.
- Spaces between scopes stay as literal spaces (scope strings aren't URL-form-encoded; in a form-encoded body the whole `scope=...` value is URL-encoded normally).

## Subsumption (which scope grants which)

Not formally defined as a lattice in the spec, but in practice:

- A more-wildcarded scope subsumes a more-specific one with the same resource. `repo:*` subsumes `repo:app.bsky.feed.post`.
- Query params narrow the grant. `repo:foo?action=create` does NOT subsume `repo:foo?action=delete`.
- If you have both `repo:*` and `repo:foo?action=create`, the specific one is redundant — the Rust crate's `parse_multiple_reduced` strips such redundancies.

Clients should request the **narrowest** scope they actually need. Over-requesting erodes trust and may be rejected by cautious users.

## Scope in the client metadata vs authorize request

Client metadata declares the **possible** scope:

```json
"scope": "atproto transition:generic repo:app.bsky.feed.post?action=create rpc:app.bsky.feed.getAuthorFeed?aud=*"
```

The authorize request's `scope` parameter is a **subset** of that. You can request less but not more.

Many clients just duplicate the metadata `scope` into the authorize request. Fine, but consider step-up: request only `atproto` on first login, then start a fresh flow requesting more scopes when the user tries to use a feature that needs them.

## Scope in the token response

The AT Proto profile requires the AS to echo granted scopes in the token response:

```json
"scope": "atproto transition:generic"
```

The client MUST:

1. Verify `atproto` is present. If not, reject the session.
2. Use the echoed scope as the source of truth for what the session can do. The user may have ticked off items on the consent screen — you get less than you asked for.

## Permission sets

A permission set is a lexicon with type `permission-set` that bundles granular permissions with user-facing labels:

```json
{
  "lexicon": 1,
  "id": "com.example.authBasicFeatures",
  "defs": {
    "main": {
      "type": "permission-set",
      "title": "Basic App Functionality",
      "detail": "Creation of posts and interactions",
      "permissions": [
        { "type": "permission", "resource": "repo",
          "collection": ["app.example.post"] },
        { "type": "permission", "resource": "rpc",
          "inheritAud": true,
          "lxm": ["app.example.getFeed", "app.example.getProfile"] }
      ]
    }
  }
}
```

Caching:

- AS fetches and caches; may serve stale up to 24h, must refresh within 90 days for new sessions.
- Permission set updates propagate to new sessions automatically. Existing tokens keep the scope they were minted with.

Namespace authority:

- A permission set may reference resources in its own NSID group or deeper (sub-domains).
- Cannot reference sibling groups or parents. `com.example.auth.basic` may grant `com.example.widget.*`, but NOT `com.other.thing.*` or `com.*`.

## Implementation sketch (scope parsing)

The reference atproto-oauth implementations ship a rich scope parser (e.g. Rust `atproto-oauth`'s `scopes` module: `parse`, `parse_multiple`, `parse_multiple_reduced` which removes subsumed scopes, `serialize_multiple` with lexicographic sort). atperson needs no scope parser today: OAuth is not active (it uses legacy app-password login), and the read-only network path never requests scopes. If scope parsing is ever needed, Wolfram exposes none — treat scopes as opaque strings here and add parsing only behind a concrete need.

TypeScript and Go libraries typically do not expose a rich scope parser — they treat scope strings as opaque and rely on the AS for semantic decisions. If you need to reason about scopes programmatically (e.g. feature-gating in the UI), port the Rust parser or request only the scopes you understand.

## Common mistakes

- **Missing `atproto`** — everyone's first bug. `scope=transition:generic` without `atproto` → AS rejects or client-side check rejects the token.
- **Partial wildcard** — `repo:app.bsky.*` is not valid. Use `repo:*` or list specific NSIDs.
- **Raw `#` in `aud`** — must be `%23`. URL libraries may do this for you inside a query-string builder but not in a scope literal.
- **Asking for more than you declared** — AS rejects with `invalid_scope`. The authorize `scope` MUST be a subset of client-metadata `scope`.
- **Assuming you got what you asked for** — the AS may grant less. Use the token response's `scope` field as truth.
