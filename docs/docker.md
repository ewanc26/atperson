# Docker deployment

`atperson` has a multi-stage Docker build and a Compose configuration for running the network-enabled runtime in a container. Linux cgroup limits feed into the same dynamic resource-budget code as a native process, so a constrained container is treated as constrained rather than as though it owns the whole host.

## Build

```sh
docker build -t atperson:latest .
```

The resulting image includes the network runtime (`ATPERSON_BUILD_NETWORK=ON`).

## Persistent state

The container uses `/var/lib/atperson` as `ATPERSON_HOME`. Mount it somewhere persistent; otherwise destroying the container destroys the model snapshot, observation ledger, action journal, cursor and runtime metadata with it.

```sh
docker run --rm \
  -v atperson-data:/var/lib/atperson \
  atperson:latest stats
```

Resource detection can be inspected the same way:

```sh
docker run --rm \
  -v atperson-data:/var/lib/atperson \
  atperson:latest resources
```

Local text ingestion also works normally:

```sh
docker run --rm \
  -v atperson-data:/var/lib/atperson \
  atperson:latest ingest "hello containerised world" local:docker-test
```

## Docker Compose

Provide AT Protocol credentials at runtime. They should not be baked into the image:

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export ATPERSON_SERVICE="https://bsky.social" # optional
```

Start the configured daemon:

```sh
docker compose up -d
```

Useful inspection commands:

```sh
docker compose logs -f
docker compose exec atperson atperson stats
docker compose exec atperson atperson control status
```

The Compose service defines a `healthcheck` that polls the supervisor
health surface (`atperson autonomy health`; exit 0 healthy, 1 stale, 2
unreadable/never started) with a `start_period` covering the gap before
the daemon's first completed cycle. `docker compose ps` reports the
result. See [`recovery.md`](recovery.md) for the health contract and
recovery procedures.

`docker compose stop` gives the daemon a normal container stop signal, which follows the daemon's graceful flush path.

## Container limits

On Linux, `atperson` probes cgroup v1/v2 memory and CPU limits and folds them into its effective resource view.

```sh
docker run --rm \
  --memory 1g \
  --cpus 2.0 \
  -v atperson-data:/var/lib/atperson \
  atperson:latest resources
```

A smaller effective budget can reduce graph-growth ceilings and timeline page sizes. It does **not** change tokenisation, learning equations, planner scoring or replay semantics.

Compose limits can be set in the normal service resource configuration:

```yaml
services:
  atperson:
    image: atperson:latest
    deploy:
      resources:
        limits:
          cpus: '2.0'
          memory: 1G
```

See [`resources.md`](resources.md) for the budgeting rules.

## Security and persistence rules

- A fresh container does not seed vocabulary, persona, opinions or preferences.
- Credentials are read at runtime and are not written into image layers by the provided configuration.
- Persist `/var/lib/atperson` if you expect learned state to survive container replacement.
- Outbound network writes are still governed by the same policy/control path as a native deployment. Running in Docker does not bypass it.