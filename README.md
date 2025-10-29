# TinySNS Distributed System

TinySNS is a teaching-scale, distributed social networking service implemented with **gRPC**, **Protocol Buffers**, and **glog**.  
This repository contains the final multi-node extension of the project: a coordinator (for discovery and failover awareness), a pool of SNS servers, and the interactive client.

---

## 1. Architecture at a Glance

| Component | Binary | Responsibilities | Key RPCs |
|-----------|--------|------------------|----------|
| Coordinator | `coordinator` | Tracks server liveness via heartbeats, assigns clients to clusters | `Heartbeat`, `GetServer` |
| SNS Server | `tsd` | Core social network logic (login, follow graph, timeline streaming) and heartbeat emission | `Login`, `List`, `Follow`, `UnFollow`, `Timeline` |
| Client | `tsc` | CLI for users; resolves a serving node through the coordinator, then issues SNS RPCs | `Login`, `List`, `Follow`, `UnFollow`, `Timeline` |

- **Cluster model**: The coordinator maintains three logical clusters. Clients are deterministically mapped to a cluster using `(client_id - 1) % 3 + 1`. Each cluster can host one or more SNS servers (the current implementation tracks the first active server per cluster).
- **Heartbeat flow**: Every server threads a heartbeat loop (`tsd.cc:70-111`) that calls `CoordService::Heartbeat` every five seconds. A server is considered inactive if no heartbeat was observed for >10s (`coordinator.cc:129-160`).
- **Failure handling**: When all servers in a cluster miss heartbeats, the coordinator rejects client assignments for that cluster (`grpc::UNAVAILABLE`). Servers will be marked active again as soon as fresh heartbeats arrive.

---

## 2. Repository Layout

| Path | Description |
|------|-------------|
| `coordinator.cc` | Coordinator service implementation and heartbeat watchdog |
| `coordinator.proto` | Protobuf service definition for the coordinator (`CoordService`) |
| `tsd.cc` | SNS server implementation with heartbeat thread and gRPC service handlers |
| `tsc.cc` | Command-line client built on the provided `IClient` framework (`client.h/.cc`) |
| `sns.proto` | SNS service definition (`SNSService`) shared by server and client |
| `Makefile` | Generates protobuf bindings and links `tsc`, `tsd`, and `coordinator` |
| `tsn-service_start.sh` | Convenience script to start a server with log-to-stderr enabled |
| `RUN_README.md` | Docker commands for the course reference container |
| `SERVER_README.md` | Legacy MP1 notes (single-server version) |

Generated protobuf sources (`*.pb.cc`, `*.pb.h`) and timeline data files (`*.timeline`, `*_follow_time.txt`) are created in place.

---

## 3. Prerequisites

This project targets the CSCE 438 course environment (Ubuntu with gRPC, protobuf, and glog preinstalled). To build elsewhere, install:

- `g++` with C++17 support
- `protobuf` compiler (`protoc ≥ 3.0`)
- `grpc` C++ libraries and plugin
- `glog`

The `Makefile` discovers the necessary flags via `pkg-config`. Adjust `PKG_CONFIG_PATH` if your toolchain is installed in a non-default location (see the first lines of `Makefile`).

---

## 4. Building

```bash
make -j4
```

Targets:

- `make` / `make all` — builds `coordinator`, `tsd`, and `tsc`, generating protobuf bindings on demand.
- `make clean` — removes binaries, intermediates, and timeline artifacts (`*.txt`).

The build assumes you run it inside the workspace root. When protobuf or gRPC binaries are missing, the `system-check` target prints diagnostics describing what to install.

---

## 5. Running the System

The binaries expect to be launched from the repository root (so timeline files stay organized). All examples below use localhost; change hostnames as required.

### 5.1 Start the Coordinator

```bash
./coordinator -p 9090
```

- Binds to `0.0.0.0:<port>`.
- Logs are emitted through glog (`coordinator-<port>.<hostname>.log.<pid>`).

### 5.2 Start SNS Servers

```
./tsd \
  -p 5000 \        # server listening port
  -h localhost \   # coordinator host
  -k 9090 \        # coordinator port
  -c 1 \           # cluster id (1..3)
  -s 1             # server id (currently advisory)
```

- Each server starts a detached heartbeat thread that registers itself with the coordinator and sends heartbeats every five seconds.
- When multiple servers advertise the same cluster, the coordinator will route clients to the first active server it detects in that cluster.

### 5.3 Start Clients

```
./tsc \
  -h localhost \   # coordinator host
  -k 9090 \        # coordinator port
  -u 7             # numeric client id / username
```

Client startup flow:

1. Contacts the coordinator via `CoordService::GetServer` to obtain the hostname/port for its cluster.
2. Connects to the designated SNS server and issues `Login`.
3. Enters command mode (see §6).

If the coordinator cannot find an active server for the client’s cluster, the client prints `Command failed` with `UNAVAILABLE`.

### 5.4 Example Session

```
# terminal 1
./coordinator -p 9090

# terminal 2 (cluster 1)
./tsd -p 5000 -h localhost -k 9090 -c 1 -s 1

# terminal 3 (cluster 2)
./tsd -p 5001 -h localhost -k 9090 -c 2 -s 1

# terminal 4 — client assigned to cluster 1: ((1-1)%3)+1 = 1
./tsc -h localhost -k 9090 -u 1

# terminal 5 — client assigned to cluster 2: ((5-1)%3)+1 = 2
./tsc -h localhost -k 9090 -u 5
```

---

## 6. Client Commands

Once connected, the client accepts the following commands (case-insensitive):

| Command | Description |
|---------|-------------|
| `FOLLOW <username>` | Adds `<username>` to your follow list (no historic replay; only new posts stream). |
| `UNFOLLOW <username>` | Removes `<username>` from your follow list. |
| `LIST` | Prints all users known to the server and your current followers. |
| `TIMELINE` | Switches to streaming mode; type messages to post, press `Ctrl+C` to exit the client. |

Notes:

- Login is implicit; supplying the same `-u` while a session is active yields “User already logged in”.
- The timeline RPC is a bidirectional stream (`sns.proto:27-30`). When the client enters timeline mode it sends a handshake message and spawns reader/writer threads (`tsc.cc:134-192`).
- The server forwards new posts to all online followers via their active streams (`tsd.cc:251-292`).

---

## 7. Server State & Persistence

When running, the server keeps in-memory `Client` objects (`tsd.cc:48-68`) holding:

- username, connection state, follower/following vectors
- current timeline stream pointer (if the user is in timeline mode)

On disk, the server writes:

- `./<username>.timeline` — append-only log of the user’s posts, containing timestamp, author, and content.
- `./<username>_follow_time.txt` — records follow events as `<followee>|<epoch_seconds>` (used for potential replay filters).

Files are written relative to the server’s working directory. Delete them to reset state between runs.

---

## 8. Logging

All binaries initialize glog with component-specific prefixes:

- `coordinator-<port>` (coordinator)
- `server-<port>` (SNS servers)
- `client-<username>` (clients)

By default glog writes under `/tmp`. To stream logs to stderr instead, prefix commands with `GLOG_logtostderr=1`, e.g.:

```bash
GLOG_logtostderr=1 ./tsd -p 5000 -h localhost -k 9090 -c 1 -s 1
```

The helper script `tsn-service_start.sh` demonstrates this usage for the server.

---

## 9. Troubleshooting

- **`Command failed` on client startup** — Coordinator could not provide a server (likely no heartbeat for the target cluster). Ensure at least one `tsd` instance is running with the correct `-c` value.
- **Server never appears active** — Heartbeat RPC may be blocked by networking (firewall) or mismatched coordinator host/port. Logs (`server-*.log*`) contain the gRPC status codes for heartbeat attempts.
- **Stale timeline data** — Stop the server and remove `*.timeline` / `*_follow_time.txt` files before restarting, or run `make clean`.
- **Proto regeneration quirks** — If you edit `.proto` files manually, re-run `make clean && make` so that both `.pb.cc` and `.pb.h` are regenerated consistently.

---

## 10. Related Documents

- `RUN_README.md` — Docker invocations for the course-provided container image.
- `SERVER_README.md` — Background notes from the single-server milestone (kept for reference).

These documents are optional aids; the canonical instructions for the distributed build are contained in this README.

