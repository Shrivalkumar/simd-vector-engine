# Local distributed topology

Start a coordinator and three durable shard processes:

```bash
docker compose -f deploy/compose/docker-compose.yml up --build
```

The coordinator accepts public vector-search gRPC traffic on port `7100` and
fans global queries to its three logical shard leaders. Each shard keeps its
WAL on its own named volume. This topology intentionally uses insecure local
gRPC credentials; production deployment must terminate external TLS and use
mTLS between coordinator, Raft, and shard channels.

The Compose profile is a functional development cluster. It does not claim
high availability: the currently implemented coordinator directory is
in-process and replication transport is the next durable-control-plane step.

