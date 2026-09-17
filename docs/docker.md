# Docker Deployment Guide

`atperson` provides a multi-stage Docker build and Docker Compose configuration for containerised deployments. The runtime automatically detects container memory and CPU limits (via cgroup v1/v2 probing) and dynamically scales graph growth budgets and timeline page sizes to match the allocated container resources.

## Quickstart

### Build the Image

Build a multi-stage image containing `atperson` compiled with network support (`ATPERSON_BUILD_NETWORK=ON`):

```sh
docker build -t atperson:latest .
```

### Run One-Off Commands

The data directory inside the container is located at `/var/lib/atperson` (overridden by `ATPERSON_HOME`). Mount a host volume so learned state, snapshots, and the observation ledger persist across container runs:

```sh
# View system resource detection and dynamic cgroup limits inside the container
docker run --rm -v atperson-data:/var/lib/atperson atperson:latest resources

# Check entity graph statistics
docker run --rm -v atperson-data:/var/lib/atperson atperson:latest stats
```

### Ingesting Data

Ingest local text directly into the entity's memory inside the container:

```sh
docker run --rm \
  -v atperson-data:/var/lib/atperson \
  atperson:latest ingest "hello containerized world" local:docker-test
```

---

## Running with Docker Compose

`docker-compose.yml` orchestrates the `atperson` daemon alongside volume persistence and runtime environment configuration.

### 1. Configure Environment Credentials

Create or export credentials on your host machine. Credentials are read at container start from environment variables — **they are never baked into image layers**:

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export ATPERSON_SERVICE="https://bsky.social" # optional
```

### 2. Launch the Daemon

Start the container in daemon mode:

```sh
docker compose up -d
```

The daemon will run bounded ingestion cycles with automatic backoff, periodic model snapshots, and graceful shutdown on container stop (`docker compose stop`).

### 3. Check Logs and Daemon Status

```sh
docker compose logs -f
docker compose exec atperson atperson stats
docker compose exec atperson atperson control status
```

---

## Resource Budgeting & Container Limits

`atperson` probes cgroup v1 and cgroup v2 memory and CPU quotas on Linux and folds container limits into its runtime resource manager (`src/app/resource/system.cpp`).

When you apply container resource constraints, `atperson` automatically scales down graph node/edge ceilings, memory reserves, and timeline sync page sizes:

```sh
# Run with a 1 GiB memory limit and 2 CPUs
docker run --rm \
  --memory 1g \
  --cpus 2.0 \
  -v atperson-data:/var/lib/atperson \
  atperson:latest resources
```

Example output under cgroup restrictions:
```text
cpu: 10 host logical, 2.00 effective (container-limited)
memory: 1022.0 MiB available / 1.00 GiB effective (container-limited; host 11.73 GiB)
memory reserve: 128.0 MiB; graph growth budget: 256.0 MiB
graph ceilings: nodes 314572, edges 2936012
sync budget: page size 24
```

You can also specify resource limits directly in `docker-compose.yml`:

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

---

## Security & Persistence Invariants

1. **Empty-Start Invariant**: The containerized binary begins with zero vocabulary and zero seeded persona.
2. **Credential Safety**: Authentication tokens and app passwords are read strictly from runtime environment variables. Image layers contain no secret state.
3. **Durable Persistence**: All state (model snapshots, observation ledger, action journal, and ingestion cursor) lives under `/var/lib/atperson`. Always attach a volume mount (`-v atperson-data:/var/lib/atperson`) to retain state across container rebuilds.
