# Peer-to-Peer Distributed File Sharing System

A BitTorrent-style file sharing system in C++17, written with POSIX sockets and threads only. Users form groups and share files inside them. Files are split into 512 KiB pieces, verified with SHA-1, and downloaded **from several peers in parallel**. Two **synchronized trackers** keep the metadata, and the system keeps working as long as one of them is up.

- **Spec:** AOS Assignment 3, IIIT Hyderabad, Monsoon 2025.
- **Technical report:** [REPORT.md](REPORT.md).
- **Revision notes and interview Q&A:** [NOTES.md](NOTES.md).

---

## Contents
1. [Features](#1-features)
2. [Build](#2-build)
3. [Run](#3-run)
4. [Commands](#4-commands)
5. [Architecture](#5-architecture)
6. [Network protocol and message formats](#6-network-protocol-and-message-formats)
7. [Tracker synchronization](#7-tracker-synchronization)
8. [Download algorithm and piece selection](#8-download-algorithm-and-piece-selection)
9. [Data structures](#9-data-structures)
10. [Concurrency model](#10-concurrency-model)
11. [Failure handling](#11-failure-handling)
12. [Testing](#12-testing)
13. [Measured performance](#13-measured-performance)
14. [Assumptions](#14-assumptions)
15. [Limitations](#15-limitations)
16. [Source layout](#16-source-layout)

---

## 1. Features
**Users and groups:**
- Every command in spec §4.1: registration, login, groups, join requests, owner-only operations, logout.
- If the owner leaves, the oldest remaining member becomes owner; an empty group is deleted.

**Files:**
- 512 KiB pieces with a **SHA-1 per piece and per file**. SHA-1 is implemented from FIPS 180-4.
- Files up to 1 GiB and beyond in **constant memory**: about 5 MB peak, measured.

**Downloads:**
- **Parallel multi-peer downloads** with **rarest-first** piece selection, piece maps, and **partial seeding** (downloaders serve the pieces they already have).
- Several downloads can run at once per client.
- Every piece is verified as it arrives. A corrupted piece is discarded and re-fetched from another peer, which is then never asked again.
- A whole-file SHA-1 check at the end. The file is written to `name.part` and gets its real name **atomically**.

**Trackers:**
- **Two synchronized trackers:** live operation forwarding, plus a snapshot and deterministic merge on every reconnect.
- Covers a restarted tracker (catches up) and a network partition (both sides converge).
- Heartbeats detect a dead or frozen tracker.
- **Client failover:** a client transparently switches tracker, logs in again and re-announces its shared files.

**Engineering:**
- Our own framed TCP protocol: length-prefixed, with magic bytes, typed messages, status codes, and partial send/receive handled everywhere.
- Graceful shutdown: no leaked memory, file descriptors or threads (checked with valgrind, ASan and `/proc`).

## 2. Build
Requirements: Linux, `g++` with C++17, and `make`. Developed with g++ 15.2 on Ubuntu 26.04 (WSL2).

```bash
make              # builds tracker/tracker and client/client (0 warnings with -Wall -Wextra -Wpedantic)
make test         # unit + integration + failure-injection tests (about 4 min)
make asan         # same tests with AddressSanitizer + UndefinedBehaviorSanitizer
make tsan         # same tests with ThreadSanitizer
make valgrind     # same tests under valgrind (memory errors, leaks, open fds)
make test-large   # 1 GiB end-to-end runs: upload, download from 1/2/3 seeders, speed + memory
make clean
bash scripts/check_all.sh   # everything above except test-large, with a warning count
```

Each directory also builds on its own: `make -C tracker`, `make -C client`.

## 3. Run
`tracker_info.txt` has one `IP PORT` line per tracker. Line 1 is tracker 1 and line 2 is tracker 2.
```
127.0.0.1 6001
127.0.0.1 6002
```

Start both trackers, each in its own terminal:
```bash
./tracker/tracker tracker_info.txt 1
```
```bash
./tracker/tracker tracker_info.txt 2
```

Start clients. `<IP>:<PORT>` is where the client **serves pieces to other peers**:
```bash
./client/client 127.0.0.1:7001 tracker_info.txt
```
```bash
./client/client 127.0.0.1:7002 tracker_info.txt
```

**Tracker console commands:**
- `quit`: graceful shutdown. Ctrl-C and SIGTERM do the same.
- `status`: counts, plus the sync state.
- `sync off` / `sync on`: cut or restore the tracker-to-tracker link, to simulate a network partition.

**Example session:**
```
client A> create_user alice pw
client A> login alice pw
client A> create_group movies
client A> upload_file movies /home/me/video.mp4
client B> create_user bob pw
client B> login bob pw
client B> join_group movies
client A> accept_request movies bob
client B> list_files movies
client B> download_file movies video.mp4 /home/me/downloads
client B> show_downloads
[C] [movies] video.mp4
```

## 4. Commands
| Command | Meaning |
|---|---|
| `create_user <user_id> <password>` | Register (ids: 1–64 chars of `A-Za-z0-9_.-`) |
| `login <user_id> <password>` | Start a session. Re-shares this client's previously shared files |
| `create_group <group_id>` | Create a group; you become its owner |
| `join_group <group_id>` | Send a join request |
| `leave_group <group_id>` | Leave; your shares in that group stop |
| `list_groups` | All groups |
| `list_requests <group_id>` | Pending requests (owner only) |
| `accept_request <group_id> <user_id>` | Accept a request (owner only) |
| `upload_file <group_id> <file_path>` | Hash the file and share it with the group (members only) |
| `list_files <group_id>` | Files in the group: size, pieces, seeders (members only) |
| `download_file <group_id> <file_name> <dest_dir>` | Start a background download into a **directory** |
| `show_downloads` | `[D] [g] f 45% (9/20 pieces, 3 peer(s))`, `[C] [g] f` (spec format), `[F] [g] f - reason` |
| `stop_share <group_id> <file_name>` | Stop sharing a file |
| `logout` | End the session and stop sharing |
| `help`, `quit` | Command list; exit (logs out first) |

Output convention: a plain sentence on success, `error: <reason>` on failure, `usage: …` for wrong arguments.

## 5. Architecture
```
            tracker_info.txt  (IP PORT per line)
     ┌──────────────────────┐   sync link (each pushes its own ops)   ┌──────────────────────┐
     │      TRACKER 1        │ ◄─────────────────────────────────────► │      TRACKER 2        │
     │ TrackerState (1 mutex)│   HELLO → SNAPSHOT → OP, OP… / PING     │ TrackerState          │
     │ ConnServer + SyncLink │                                          │ ConnServer + SyncLink │
     └─────────▲────────────┘                                          └──────────▲───────────┘
               │ metadata only: users, groups, file hashes, who seeds what         │ (failover)
     ┌─────────┴────────────┐       pieces flow peer-to-peer          ┌───────────┴──────────┐
     │       CLIENT A        │ ◄─────────────────────────────────────► │       CLIENT B        │
     │ TrackerLink (failover)│   GET_PIECE_MAP / GET_PIECE (512 KiB)   │ PeerServer (seeder)   │
     │ DownloadManager       │                                          │ DownloadManager       │
     │  └ PiecePicker+workers│                                          │ SharedFiles registry  │
     └──────────────────────┘                                          └──────────────────────┘
```

**The tracker never stores or relays file data.** It keeps metadata only: users, sessions, groups, file sizes and hashes, and who is seeding.

**A client is both a downloader and a seeder:**
- **`PeerServer`:** answers other peers' piece requests.
- **`DownloadManager`:** fetches files from other peers.
- **`TrackerLink`:** the single connection to "the tracker system", with failover.
- **`SharedFiles`:** a thread-safe registry of what this client shares, including partial downloads.

**Shared building blocks** in `common/`:
- `protocol` (framing), `net` (sockets), `sha1`, `file_meta` (pieces and I/O loops).
- `conn_server`: the accept loop, one thread per connection, and graceful stop. Used by both the tracker and the peer server.

## 6. Network protocol and message formats
All communication is TCP. Every message is one **frame**:
```
 byte:  0    1    2      3        4..7            8 …
      ┌────┬────┬──────┬────────┬──────────────┬─────────────────┐
      │ 'P'│ '2'│ type │ status │ length (BE32)│ payload         │
      └────┴────┴──────┴────────┴──────────────┴─────────────────┘
```
- **magic `P2`:** detects a desynchronized or foreign stream (`BAD_HEADER`).
- **type:** the message type. A **reply** uses the request's type with the top bit set (`type | 0x80`).
- **status:** `OK` in requests; the result code in replies.
- **length:** the payload size, at most **4 MiB**, checked *before* allocating.
- **Sending:** `send_all` loops until every byte is written, and uses `MSG_NOSIGNAL`. Header and payload go out in one buffer, which avoids Nagle/delayed-ACK stalls.
- **Receiving:** `recv_all` loops until exactly N bytes are in. A peer close is reported as `CLOSED` (between messages) or `TRUNCATED` (mid-message).
- **Payload:** text fields joined by `\n`, or raw bytes for piece data. Piece hashes travel as one string of 40 hex characters per piece.

| Type | Hex | Direction | Request fields → reply payload |
|---|---|---|---|
| PING | 01 | any | – → – (also the sync heartbeat) |
| CREATE_USER | 02 | client→tracker | user, password |
| LOGIN | 03 | client→tracker | user, password, my_peer_addr |
| LOGOUT | 04 | client→tracker | – |
| CREATE_GROUP / JOIN_GROUP / LEAVE_GROUP | 05/06/07 | client→tracker | group |
| LIST_GROUPS | 08 | client→tracker | – → group ids |
| LIST_REQUESTS | 09 | client→tracker | group → user ids |
| ACCEPT_REQUEST | 0A | client→tracker | group, user |
| UPLOAD_FILE | 0B | client→tracker | group, name, size, file_sha1, piece_sha1s (also used to become a seeder) |
| LIST_FILES | 0C | client→tracker | group → one field per file: `name\tsize\tseeders` |
| FILE_INFO | 0D | client→tracker | group, name → size, file_sha1, piece_sha1s, seeder addresses… |
| STOP_SHARE | 0F | client→tracker | group, name |
| SYNC_HELLO | 20 | tracker→tracker | my tracker number |
| SYNC_OP | 21 | tracker→tracker | op type, op args… |
| SYNC_SNAPSHOT | 22 | tracker→tracker | full state (see §7) |
| GET_PIECE_MAP | 41 | client→client | group, name → `"1101…"` (one char per piece) |
| GET_PIECE | 40 | client→client | group, name, index → raw piece bytes |

(`0x0E ADD_SEEDER` is reserved and unused, because `UPLOAD_FILE` with the same hash already means "one more seeder".)

**Status codes:**
- `OK` 0, `BAD_REQUEST` 1, `UNKNOWN_TYPE` 2, `NOT_LOGGED_IN` 3, `ALREADY_LOGGED_IN` 4
- `AUTH_FAILED` 5 (the same code for an unknown user and a wrong password), `USER_EXISTS` 6, `NO_SUCH_USER` 7
- `GROUP_EXISTS` 8, `NO_SUCH_GROUP` 9, `NOT_MEMBER` 10, `ALREADY_MEMBER` 11, `NOT_OWNER` 12
- `REQUEST_PENDING` 13, `NO_SUCH_REQUEST` 14
- `FILE_CONFLICT` 15 (same name, different content), `NO_SUCH_FILE` 16, `NO_SUCH_PIECE` 17
- `INTERNAL_ERROR` 18, `NOT_SHARING` 19, `SYNC_REFUSED` 20

## 7. Tracker synchronization
**State machine replication.** Every change to tracker state is an **operation** applied through one function, `TrackerState::apply()`. `apply()` validates and mutates under one mutex and is deterministic, so replaying the same operations gives the same state.

**The sync link.** Each tracker runs a `SyncLink` thread that connects to the other tracker and pushes **its own** clients' changes. Nothing is echoed back. On every (re)connect:
1. `SYNC_HELLO(my number)`.
2. `SYNC_SNAPSHOT`: the full state. The receiver **merges** it.
3. `SYNC_OP` for every later change, in order, each acknowledged.
4. A `PING` every 1 s while idle. The receiver drops a link that has been silent for 3.5 s.

**No gaps, no duplicates.** "Apply locally + queue for the peer" and "take snapshot + clear queue + start queueing" use the same mutex. So every change is either inside the snapshot or queued after it. Changes made while the link is down aren't queued; the next snapshot carries them.

**Merge rules.** The rules are written in terms of *tracker 1's copy* and *tracker 2's copy*, so both trackers compute identical results:

| Data | Rule |
|---|---|
| Users | Union. Same id with different passwords → tracker 1's wins |
| Sessions | Each tracker is the authority for the sessions **it created** (the session id starts with its number). Same user in both → tracker 1's wins |
| Groups | Union. Same id and same owner → members, requests and files combined (tracker 1's order first). Different owners (created during a partition) → tracker 1's wins |
| Files | Same name and same SHA-1 → seeder lists combined. Different content → tracker 1's wins |
| Afterwards | Seeders who aren't online are removed, along with files with no seeder left |

**Recovery cases:**
- **Restart:** a tracker comes back empty and receives the other's snapshot. Identical within milliseconds.
- **Partition:** both sides keep serving their own clients. On heal, they exchange snapshots and converge to **byte-identical** state.
- **Dead or frozen peer:** detected by a closed socket or missing heartbeats. Its clients' sessions are dropped. Those clients fail over, log in again and re-announce their files.

The consistency model is **eventually consistent / AP** in CAP terms: available during a partition, convergent after it. Details and trade-offs are in [REPORT.md](REPORT.md#3-synchronization-algorithm).

## 8. Download algorithm and piece selection
1. `FILE_INFO` from the tracker: size, file SHA-1, piece SHA-1s and seeder addresses.
2. Checks done up front, with an immediate error:
   - some *other* peer has the file;
   - the destination is a directory;
   - the file isn't already there;
   - you aren't already sharing it.
3. Create `<dest>/<name>.part` and size it with `ftruncate`, which makes a sparse file. Register it as a **partial share** (verified pieces are served to others), and announce it to the tracker after the first verified piece.
4. **One worker thread per peer (at most 4).** Each worker:
   - connects and gets the peer's **piece map**;
   - repeatedly asks the shared **`PiecePicker`** for the **rarest** piece (fewest peers have it) that its peer has and nobody is fetching, with random tie-breaks;
   - runs `GET_PIECE` → checks the length and **SHA-1** → `pwrite` at offset `index × 512 KiB`.

   Workers share the output file without a lock, because `pwrite` is positional.
5. **Failures:**
   - A bad hash puts the piece back and the peer is never trusted again.
   - A dead or slow peer (10 s timeout) puts the piece back.
   - A worker with nothing to take *waits* on a condition variable if a piece its peer has is in progress elsewhere (that piece might fail); otherwise it exits.
   - If pieces are still missing, the client asks the tracker for peers again, up to 3 rounds.
6. **Whole-file SHA-1** check → `link(.part, final)` (atomic; `rename` is the fallback) → keep seeding.

**Why rarest-first:** a piece that few peers have is the one at risk of disappearing. Fetching it first (and then seeding it) spreads it fastest. Random ties keep downloaders from all requesting the same piece. The `IN_PROGRESS` state guarantees no piece is downloaded twice.

## 9. Data structures
| Where | Structure | Why |
|---|---|---|
| Tracker | `std::map<user, {digest}>`, `std::map<user, {peer_addr, session_id}>` | Sorted output for listings; O(log n) is ample |
| Tracker | `Group {vector members, vector pending, map<name, SharedFile>}` | `members[0]` *is* the owner, so the oldest member taking over is automatic |
| Tracker | `SharedFile {size, file_sha1, piece_sha1s, vector seeders}` | Metadata only; the entry disappears with its last seeder |
| Tracker | `Op {type, args}` | One representation for local changes, sync forwarding and replay |
| Client | `SharedFiles: map<(group, name), {path, FileMeta, vector<bool> have}>` | `have` empty = complete; otherwise the partial-seeding bitmap |
| Client | `PiecePicker: vector<State>, vector<int> availability` | O(pieces) rarest-first scan with reservoir-sampled ties; 2048 pieces for 1 GiB |
| Client | `DownloadStatus` per job + atomics for counters | `show_downloads` reads a consistent snapshot without stopping workers |

## 10. Concurrency model
- **Tracker:** one accept thread, **one thread per connection**, plus one `SyncLink` thread. All state is behind **one mutex**, so check-and-act is atomic and deadlock is impossible.
- **Client:**
  - the main thread runs the command loop;
  - the peer server has an accept thread and one thread per peer connection;
  - each download has a coordinator thread plus up to 4 workers.
- **Thread safety:** `TrackerLink` serializes request/reply pairs on its socket with a mutex. `SharedFiles` has its own mutex. The `PiecePicker` uses a mutex and a condition variable.
- **Socket ownership:** a connection's fd is **closed only after its thread is joined**, so a shutdown can never act on a reused fd number.
- **Graceful shutdown:**
  - The tracker blocks SIGINT and SIGTERM in all threads and receives them through `signalfd`, polled next to stdin.
  - `shutdown()` wakes blocked `accept` and `recv` calls; then all threads are joined.
  - The client destroys objects in reverse order: downloads → peer server → registry → tracker link.

## 11. Failure handling
| Failure | Behaviour |
|---|---|
| Partial send/recv, signal interruptions | Loops everywhere (`send_all`, `recv_all`, `read_full`, `pwrite_full`), with `EINTR` retried |
| Peer closes / writes to a dead peer | `CLOSED`/`TRUNCATED` results; SIGPIPE ignored + `MSG_NOSIGNAL` |
| Garbage or oversized frame | Connection dropped (`BAD_HEADER` / `TOO_LARGE`); length checked before allocating |
| Seeder dies or freezes mid-download | Piece returned to the picker, other peers continue (10 s peer timeout) |
| Corrupted piece | Detected by SHA-1, re-fetched elsewhere, peer distrusted |
| Only seeder lost | Clean `[F]` failure; `.part` deleted; partial share withdrawn |
| Client crashes | Tracker logs it out automatically → it stops being listed as a seeder |
| Tracker dies or freezes | Clients fail over (5 s reply timeout) with re-login + re-announce; the other tracker detects it via heartbeat |
| Tracker restarts | Catches up from the other tracker's snapshot |
| Trackers partitioned | Both keep working; converge on reconnect |
| Both trackers down | `error: no tracker reachable`; clients keep running; transfers in progress continue |

## 12. Testing
All suites run with `make test`, and again under ASan+UBSan, TSan and valgrind (`scripts/check_all.sh`).

| Suite | Checks | What it covers |
|---|---|---|
| `tests/test_protocol.cpp` | 65 | Framing: 512 KiB/4 MiB messages, byte-by-byte delivery, glued frames, truncation, bad magic, oversized length, timeouts, SIGPIPE, endpoint/config parsing, fd leak count |
| `tests/test_tracker.cpp` | 135 | User/group/file rules, owner handover, password digest, a real in-process server, auto-logout, garbage input, 8 concurrent clients racing, graceful stop |
| `tests/test_client.cpp` | 25 | Tracker failover with re-login, dead/hung tracker, both down and recovery, concurrent callers |
| `tests/test_files.cpp` | 48 | SHA-1 official test vectors (incl. one million 'a'), streaming at padding boundaries, piece math, `hash_file`, `pread`/`pwrite` |
| `tests/test_picker.cpp` | 38 | Rarest-first, no duplicates, WAIT vs NONE, random ties, blocking wake-up, 8 threads × 2000 pieces exactly once |
| `tests/test_download.cpp` | 158 | Peer server (hostile requests, path traversal), corrupted/dead/partial seeders, 3-seeder parallel download, failure cleanup |
| `tests/test_sync.cpp` | 81 | Merge rules (byte-identical convergence after a partition, conflicts, session authority), live sync, restart, partition, failover re-announce |
| `tests/sha1_crosscheck.sh` | 10 sizes | Our piece and file hashes vs `sha1sum` (incl. exactly 512 KiB ± 1); 1 GiB with `make test-large` |
| `tests/smoke_tracker.sh` | 9 | Real tracker binary: `quit`/SIGTERM under valgrind, port reuse, port-in-use error |
| `tests/integration.sh` | 173 | 2 trackers + up to 8 client processes: every command, uploads/downloads checked with `cmp`, corruption, 3-seeder parallel download, cross-tracker use, `kill -9` tracker, empty restart, partition and heal |
| `tests/failures.sh` | 109 | Seeder `kill -9`/`SIGSTOP` mid-download, only seeder lost, downloader crash and retry, tracker killed mid-download, tracker frozen/resumed, both trackers down, fd/thread counts after 100 connection bursts |
| `tests/large.sh` | 46 | `make test-large`: 1 GiB upload, download from 1/2/3 seeders, `cmp`, speed, peak memory |

**How the integration tests work.** Each process reads its stdin from a FIFO held open by the script. `send c1 "login alice pw"` types a command, and `expect c1 "^logged in"` waits for the matching output line. `retry` re-runs a command until state that arrives asynchronously shows up. Logs go to `test_runs/<suite>/`.

## 13. Measured performance
Loopback, one machine (WSL2, 8 cores), 1 GiB random file, plain build:

| | Result |
|---|---|
| Hashing on upload (2048 pieces) | Peak memory **≈ 4.9 MB** (constant: the same as for a 1-byte file) |
| Download from 1 seeder | **44 MiB/s** (22.9 s) |
| Download from 2 seeders | **63 MiB/s**, pieces split 1011 / 1037 |
| Download from 3 seeders | **78 MiB/s**, pieces split 672 / 679 / 697; the counts always sum to 2048 (no duplicates) |
| Downloader / seeder peak memory | ≤ 6.7 MB / ≈ 6 MB |

On one machine, every process shares the same CPU: SHA-1 on both ends, the final whole-file re-hash, and the loopback copies. Over a real network, each seeder's upload bandwidth is the bottleneck, and parallelism helps more.

## 14. Assumptions
- **IPv4** addresses. `tracker_info.txt` has exactly two lines; blank lines, `#` comments and Windows `\r` are tolerated.
- **Ids:** user and group ids are 1–64 chars of `A-Za-z0-9_.-`. Passwords are 1–128 printable characters with no spaces. File names can't contain whitespace or `/`, and paths can't contain spaces (commands are whitespace-separated).
- **Logins:**
  - One active login per user. The *same* client address may log in again, which is how failover works.
  - "No such user" and "wrong password" give the same error.
- **Destination:** the download destination is a **directory**, and the file keeps its name. You can't download a file you're already sharing in that group.
- **File names:**
  - Same name and same content in a group = one more seeder.
  - Same name with different content is rejected.
  - A file stays listed only while at least one online member shares it.
- **Logout and re-login:** logout stops sharing. Logging in again as the same user on the same client re-shares those files without rehashing.
- **Group membership:** a crashed client is logged out but stays a member of its groups.
- **Empty files** are rejected.
- **The status format** `[C] [group] file` is used as specified. `[D]` (in progress) and `[F]` (failed) are additions.

## 15. Limitations
- **No persistence:** state lives in memory. If both trackers die, users and groups are lost; clients can re-share files once a tracker is back.
- **Merge limitations:**
  - Deletions during a partition (leave, stop_share) can be undone by the union merge, because there are no tombstones.
  - On conflicting creates, the tracker-2 version loses.
- **Single message for snapshots and piece lists:** a snapshot must fit in one 4 MiB message, and a file's piece list must fit too (about 100k pieces ≈ 50 GB).
- **No download resume:** a downloader that crashes starts the file again (a stale `.part` is overwritten).
- **Retries aren't idempotent:** a client request retried on the other tracker after a failover may report an error for an operation that actually succeeded. Request ids would fix this.
- **Frozen clients:** a frozen client holds one tracker thread until it dies (no TCP keepalive). Thread-per-connection is sized for a handful of clients, not thousands.
- **No encryption:** passwords cross the network in plaintext. Stored passwords are `SHA1(user:password)`, which is salted by user id but fast; real systems would use TLS and bcrypt/argon2.
- **SHA-1 strength:** SHA-1 detects accidental corruption but not a determined attacker (collisions are practical). SHA-256 would be needed against malicious peers.

## 16. Source layout
```
common/    protocol.{h,cpp}   framing, message types, status codes
           net.{h,cpp}        sockets (listen/connect with timeout), tracker_info parsing
           conn_server.{h,cpp} accept loop + thread per connection + graceful stop
           sha1.{h,cpp}       SHA-1 (FIPS 180-4), streaming
           file_meta.{h,cpp}  pieces, streaming file hashing, read/pread/pwrite loops
           flags.mk, common.mk  shared build settings (normal / ASan / TSan flavours)
tracker/   main.cpp           console (quit/status/sync on|off), signalfd shutdown
           state.{h,cpp}      all tracker data + rules, apply(), snapshot/merge
           server.{h,cpp}     request handling, sync receiver, auto-logout
           sync.{h,cpp}       SyncLink: snapshot + op forwarding + heartbeats
client/    main.cpp           wiring + command loop
           commands.{h,cpp}   the user commands
           tracker_link.{h,cpp} tracker connection with failover, re-login, re-announce
           peer_server.{h,cpp}  serves piece maps and pieces
           downloads.{h,cpp}  parallel download manager
           piece_picker.{h,cpp} rarest-first picker
           shared_files.h     registry of shared (and partially downloaded) files
tests/     unit tests (*.cpp), integration/failure scripts (*.sh), lib.sh harness
scripts/   check_all.sh (full gate), fix_crlf.sh
```

**Layout note:** the spec asks for `tracker/` and `client/` directories with their sources. Code used by both (framing, sockets, SHA-1, the connection server) lives once in `common/`, which both Makefiles compile in. This avoids two copies drifting apart.
