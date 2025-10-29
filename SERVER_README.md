# Tiny SNS (CSCE 662 MP1)

A minimal social network service using **gRPC** and **Protocol Buffers**.  
Supports `LOGIN`, `FOLLOW`, `UNFOLLOW`, `LIST`, and a real-time `TIMELINE` mode with persistent timelines on the server.

## Build

This project targets the course VM environment (Ubuntu + gRPC/Protobuf preinstalled).

    make -j4

> Targets you should expect: `make` (default), `make clean`.  
> If your environment differs from the course VM, ensure gRPC and Protobuf dev packages are installed.

## Run

### Start the server

    ./tsd -p 5000

- The server listens on `0.0.0.0:<port>`.
- Example log line: `Server listening on 0.0.0.0:5000`.

### Start a client

    ./tsc -h <host> -p <port> -u <username>

**Examples**

    # In terminal 1:
    ./tsd -p 5000

    # In terminal 2:
    ./tsc -h 127.0.0.1 -p 5000 -u alice

    # In terminal 3:
    ./tsc -h 127.0.0.1 -p 5000 -u bob

## Client Commands (command mode)

- `FOLLOW <username>` — start following `<username>` (only new posts after this point will appear on your timeline).
- `UNFOLLOW <username>` — stop following `<username>`.
- `LIST` — prints:
  - all users registered on the server
  - followers **of the current user**
- `TIMELINE` — switch to timeline mode (irreversible). In timeline mode:
  - you immediately receive up to the **last 20** relevant posts (see below),
  - and then receive new posts in **real time**.
  - type to post; `Ctrl-C` to terminate the client.

## Behavior Summary

- **LOGIN**: duplicate login of the same username is rejected while the previous session is connected.
- **FOLLOW**: you will **not** see historical posts made **before** the time you followed. Only posts created **after** the follow moment are visible.
- **UNFOLLOW**: removes the relationship immediately.
- **LIST**: returns all users + current user’s followers.
- **TIMELINE**:
  - On entry, shows the latest **up to 20** posts from the users you follow, filtered to only include posts **after** you started following them.
  - Uses synchronous bidirectional streaming (ReaderWriter) for immediate delivery of new posts.
  - Once in timeline mode, there is **no** return to command mode.

## Persistence & File Layout (Server)

All files are created in the **server’s working directory** (i.e., where you run `./tsd`).

1. **Timeline files** (one per user):
   - **Path/Name**: `./<username>.timeline`  
   - **Format** (per post entry, with a blank line between posts):

        T YYYY-MM-DD HH:MM:SS
        U <username>
        W <post content>

   - Example: `alice.timeline`

2. **Follow time records** (per follower):
   - **Path/Name**: `./<follower_username>_follow_time.txt`
   - **Purpose**: record the epoch second when `<follower_username>` followed each followee, so that only posts **after** that moment are shown.
   - **Format** (one line per followee):

        <followee_username>|<epoch_seconds>

   - Example:

        bob_follow_time.txt
        └─ alice|1726620000

> On timeline entry, the server merges all followees’ `*.timeline` files, filters entries older than the saved follow time, sorts by time (desc), and sends the top 20 to the client. New posts are appended to the author’s `*.timeline` and streamed to online followers.

## Reproducing a Minimal Flow

1. Start server:

        ./tsd -p 5000

2. Start two clients:

        ./tsc -h 127.0.0.1 -p 5000 -u alice
        ./tsc -h 127.0.0.1 -p 5000 -u bob

3. In **bob** client:

        FOLLOW alice
        TIMELINE

   (Bob now receives Alice’s new posts in real time.)

4. In **alice** client:

        TIMELINE
        # type to post messages; Bob will see them immediately

## Notes & Assumptions

- Server stores files in the current working directory. To reset all data, stop the server and delete `*.timeline` and `*_follow_time.txt`.
- Self-follow/self-unfollow are treated as invalid username operations.
- If a user attempts to unfollow a user they are not following, the server returns a “not following” style message; the client displays it as an invalid target.
- The server uses in-memory structures for user/follow graphs plus file persistence for timelines. If you restart the server, existing timeline files remain, but **in-memory** follow graphs are rebuilt via new logins and subsequent FOLLOW actions (the spec only requires persistent timelines on disk).

## Directory Structure (suggested)

```
.
├─ tsd.cc          # server
├─ tsc.cc          # client
├─ *.proto         # protobuf definitions (and/or generated sources)
├─ Makefile
├─ README.md
├─ <user>.timeline
└─ <user>_follow_time.txt
```

## Troubleshooting

- **Client fails to connect**: ensure the server is running and the host/port are correct.  
- **No posts appear after FOLLOW**: expected until the followee posts something **after** the follow moment.  
- **Permission issues writing files**: ensure the server process has write permissions in its working directory.