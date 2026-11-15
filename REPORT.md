# Technical Report — Peer-to-Peer Distributed File Sharing System

AOS Assignment 3 (Monsoon 2025). Build/run instructions, the command reference and message tables are in [README.md](README.md). This report explains **how** the system was built and **why**, as spec §10.2 asks:
- the implementation approach;
- the synchronization algorithm;
- the piece selection strategy;
- the protocol design rationale;
- the challenges and their solutions;
- references.

---

## 1. Implementation approach

### 1.1 Guiding principles
- **Small set of features, done reliably** (spec §12). Every feature was built only when the previous layer was tested.
- **One mechanism per problem, reused everywhere:**
  - one framing layer for all three conversations;
  - one connection server for the tracker and every peer server;
  - one `apply()` for every tracker state change, which made replication almost free.
- **Every blocking network call has a timeout,** so failures become errors instead of hangs.
- **Prove it:** unit tests, end-to-end tests with the real binaries, failure injection, and runs under ASan, UBSan, TSan and valgrind.

### 1.2 Build order (each step left a working, tested system)
| Step | Delivered | Key tests |
|---|---|---|
| 1 | Framed protocol, socket helpers, build system | Partial send/recv, truncation, bad headers, SIGPIPE |
| 2 | Tracker: users, sessions, groups; `ConnServer`; graceful shutdown | Rule tests, 8 concurrent clients, stop with clients connected |
| 3 | Client commands, `TrackerLink` failover, integration harness | Failover with re-login, hung tracker |
| 4 | SHA-1 from FIPS 180-4, 512 KiB piece hashing, upload/list/stop_share | Official vectors, `sha1sum` cross-check, 1 GiB in ~5 MB |
| 5 | Peer server, single-peer download, `.part` + atomic rename | Corrupted/dead seeder, hostile requests |
| 6 | Parallel multi-peer download, rarest-first `PiecePicker`, partial seeding | Picker race test, 3-seeder download, 1 GiB speed-up |
| 7 | Tracker-to-tracker sync, client re-announce | Byte-identical convergence, restart, partition, failover |
| 8 | Failure injection | Kill/freeze seeders and trackers mid-download, fd/thread bursts |

### 1.3 Components
- **Tracker:**
  - `TrackerState`: all data plus the rules, with no networking.
  - `TrackerServer`: request handling.
  - `SyncLink`: replication to the other tracker.
  - `main`: console and `signalfd`-based shutdown.
- **Client:**
  - `TrackerLink`: failover, re-login, re-announce.
  - `ClientApp`: commands.
  - `PeerServer`: seeding.
  - `DownloadManager` + `PiecePicker`: parallel downloads.
  - `SharedFiles`: a thread-safe registry, including partially downloaded files.
- **Common:**
  - `protocol`, `net`, `ConnServer`;
  - `sha1`;
  - `file_meta`: piece math, streaming hashing, EINTR-safe `read`/`pread`/`pwrite` loops.

### 1.4 Constraints honoured
- C++17 using only the standard library and POSIX system calls.
- **No** `system()`, `exec*`, `popen`, `fork`, filesystem library, database or external crypto library. This was checked by grepping the sources.
- Files are handled with `open`/`pread`/`pwrite`/`ftruncate`/`link`/`rename`/`stat`.
- The build has 0 warnings under `-Wall -Wextra -Wpedantic`. `-Werror` is deliberately left out of the shipped Makefiles, so a newer compiler with new warnings can't break the build. The check script enforces 0 warnings instead.

---

## 2. Protocol design rationale

**Why framing.** TCP is a byte stream: one `send` may arrive in several `recv`s, and several sends may arrive in one. Every message is therefore a frame: an 8-byte header (magic `P2`, type, status, 32-bit big-endian length) followed by exactly `length` payload bytes.

| Decision | Reason |
|---|---|
| **Length prefix** (not delimiters) | Works for binary piece data with no escaping; the receiver knows exactly how much to read |
| **Magic bytes** | Detects a desynchronized stream or a foreign client immediately (`BAD_HEADER`) instead of misreading garbage |
| **4 MiB cap, checked before allocating** | A corrupt or hostile header can't trigger a multi-gigabyte allocation |
| **Reply type = request type \| 0x80** | Every reply names its request; mismatches are treated as a broken connection |
| **Status byte in every reply** | Typed error codes (21 of them) instead of parsing error strings; the client maps them to messages |
| **Text fields separated by `\n`** | Human-readable in logs and simple to parse; user input is whitespace-tokenized, so fields can never contain `\n` |
| **Header + payload in one `send`** | Avoids the Nagle + delayed-ACK interaction (~40 ms stall per small write pair) |
| **`send_all` / `recv_all` loops** | Partial transmissions (spec §5.3) are handled in one place; `EINTR` retried; peer close distinguished as `CLOSED` vs `TRUNCATED` |
| **`MSG_NOSIGNAL` + SIGPIPE ignored** | Writing to a dead peer returns `EPIPE` instead of killing the process |
| **Same error for unknown user and wrong password** | Prevents user enumeration |
| **Extensible type space** | New types get new numbers and are never reused; unknown types get `UNKNOWN_TYPE` and the connection stays usable |

**Message integrity:**
- TCP checksums catch most transmission errors.
- Magic + length validation catches desynchronization.
- All file data is verified end to end with SHA-1 (per piece and per file), so corruption from any source, even a faulty peer disk, is detected.

---

## 3. Synchronization algorithm

### 3.1 Goals (spec §7)
Both trackers give accurate answers, and the system operates while at least one is up. It must survive temporary disconnection between the trackers and recover when a tracker comes back.

### 3.2 State machine replication
- Every state change is an `Op` (type + arguments) applied through `TrackerState::apply()`, which validates and mutates under one mutex.
- `apply()` is deterministic. So if both trackers apply the same operations in the same order, they reach the same state.
- Read-only queries never change state.

### 3.3 The sync link
- Each tracker runs a `SyncLink` thread that connects to the other tracker's normal port and **pushes only its own clients' operations**. There's one connection per direction and no echo loops.
- Each link:
  1. sends `SYNC_HELLO(n)`;
  2. sends `SYNC_SNAPSHOT(full state)`, which the receiver merges;
  3. streams `SYNC_OP`s in order, each acknowledged (keeps order, provides backpressure, detects a dead link at once);
  4. sends a `PING` every 1 s while idle.
- **Atomicity:** "apply + enqueue" and "snapshot + clear queue + start forwarding" use one mutex, so a change is never lost or skipped. Changes made while disconnected aren't buffered; the snapshot on reconnect replaces them, which keeps memory bounded.

### 3.4 Snapshot + deterministic merge
The snapshot is a canonical text encoding: sorted maps, so equal states give equal strings. The tests compare snapshots byte-for-byte to prove convergence.

The merge never uses "mine/theirs". It's defined on *tracker 1's copy* and *tracker 2's copy*, so tracker 1 merging tracker 2's snapshot and tracker 2 merging tracker 1's produce **identical** results:
- **Users:** union. On a conflict (same id, different password digest), tracker 1's version wins.
- **Sessions:** a session id starts with the number of the tracker that created it, the one holding the client's TCP connection. Each tracker is **authoritative for its own sessions**: sessions owned by tracker 1 come from tracker 1's copy, and likewise for tracker 2. The same user in both → tracker 1's wins. Sessions are *soft state*: only the tracker holding the connection knows whether the client is really there.
- **Groups:** union.
  - Same id and same owner → members, then pending requests, then files, combined as an ordered union with tracker 1's order first. Pending users who became members are removed.
  - Same id with different owners (two groups created independently during a partition) → tracker 1's group wins.
- **Files:**
  - Same name, size and SHA-1 → seeder lists combined.
  - Different content → tracker 1's file wins.
- **Pruning:** seeders who aren't online are removed, then files with no seeder.

### 3.5 Failure detection and recovery
| Event | What happens |
|---|---|
| Peer tracker crashes | Its sockets close → the receiving side's connection thread ends → **drops the sessions owned by that tracker** and their seeder entries |
| Peer tracker freezes | No PINGs → the 3.5 s receive timeout ends the connection → same as a crash |
| Clients of the lost tracker | Their next request fails or times out (5 s) → `TrackerLink` connects to the survivor, **logs in again** (same address may re-login) and **re-announces its shared files** |
| Lost tracker restarts (empty) | Its link and the survivor's link reconnect; each sends a snapshot; the restarted tracker merges the survivor's full state |
| Partition (`sync off`) | Both trackers keep serving their own clients and drop the other's sessions; on `sync on` both snapshots are exchanged and merged → identical state |
| Late duplicate events | Session ids make a stale "logout" (from an old connection) a no-op, so it can't end a newer login |

### 3.6 Consistency model and trade-offs
- **Eventual consistency, AP in CAP terms:**
  - during a partition both trackers stay available and accept changes;
  - afterwards they converge deterministically;
  - normally a change reaches the other tracker within milliseconds, but a client may briefly read stale data from the other tracker.
- **Why not consensus (Raft/Paxos)?** With two nodes, a majority is both of them. So any single failure would block writes, which contradicts "operate while at least one tracker is up". Consensus also needs a persistent log.
- **Accepted limitations:**
  - Deletions during a partition can be resurrected by the union merge (no tombstones).
  - On conflicting creates, tracker 2's version loses.
  - No persistence.
- **How to lift them:** tombstones with timestamps or version vectors, and an on-disk op log or snapshot.

---

## 4. Piece selection strategy

### 4.1 Requirements (spec §6)
Use the available peers efficiently and avoid duplicate downloads. The strategy is the implementer's choice: sequential, random, rarest-first, or peer reliability.

### 4.2 Chosen: rarest-first with random tie-break, one worker per peer
- **Piece maps:** each worker first asks its peer for a **piece map** (`GET_PIECE_MAP` → `"1101…"`), so partial seeders (downloaders that already have some pieces) take part.
- **Availability:** the `PiecePicker` keeps a per-piece availability count (how many connected peers have the piece) and a state per piece: `NEEDED → IN_PROGRESS → DONE`.
- **Picking:** a worker asks for a piece. The picker scans for the NEEDED piece **that this worker's peer has** with the **lowest availability**. Ties are broken uniformly at random with reservoir sampling, in one O(pieces) pass (2048 pieces for 1 GiB).
- **No duplicates:** a piece is IN_PROGRESS for exactly one worker.
- **Failures:** a failed fetch (bad SHA-1, peer death, 10 s timeout) puts the piece back to NEEDED. A peer that sent bad data is never asked again (simple peer reliability).
- **Waiting:** if nothing is free but a piece this peer has is IN_PROGRESS elsewhere, the worker **waits** on a condition variable, because that piece may fail and need this peer. Otherwise the worker exits. `done`, `failed`, `remove_peer` and `cancel` all notify.
- **Limits:** at most 4 workers per download, which bounds threads and sockets. If pieces remain when all workers end, the coordinator asks the tracker for fresh peers, up to 3 rounds.

### 4.3 Why rarest-first
- **Availability:** a piece held by few peers is the most likely to vanish if those peers leave. Fetching it first, and then seeding it straight away (partial seeding), raises its availability for everyone.
- **Load spreading:** random tie-breaking stops all downloaders requesting the same piece from the same peer at the same moment.
- **The alternatives were considered:**
  - **Sequential:** simplest, and allows streaming playback, but herds everyone onto the same pieces.
  - **Random:** spreads load but ignores rarity.
  - **Fixed range per peer:** simple, but a slow peer stalls its whole range.

### 4.4 Results (1 GiB, loopback)
- **Speed:** 1 seeder 44 MiB/s, 2 seeders 63 MiB/s, 3 seeders 78 MiB/s.
- **Load split:** pieces split almost evenly (672 / 679 / 697), and the counts always sum to 2048, so there were no duplicates.
- **Memory:** downloader peak ≤ 6.7 MB.

Because everything shares one CPU on a single machine, scaling isn't linear. Over a real network, per-seeder bandwidth dominates and parallelism helps more.

### 4.5 Writing pieces
- The output file is pre-sized with `ftruncate`, which makes a sparse file.
- Workers write with `pwrite(fd, data, len, index × 512 KiB)`. It's positional (no shared file offset), and each byte range has exactly one writer, so **no lock is needed**.
- Each piece is SHA-1-verified before writing, and the whole file is re-hashed before `link`/`rename` gives it its final name **atomically**.

---

## 5. Challenges encountered and solutions

| # | Challenge | Solution |
|---|---|---|
| 1 | TCP has no message boundaries; partial `send`/`recv` | Length-prefixed frames, `send_all`/`recv_all` loops; tested with byte-by-byte delivery and glued frames |
| 2 | Writing to a dead peer kills the process (SIGPIPE) | `MSG_NOSIGNAL` + SIGPIPE ignored; a unit test proves survival |
| 3 | Shutting down threads blocked in `accept`/`recv` | `shutdown()` wakes them; join; **close fds only after joining** (avoids acting on a reused fd number) |
| 4 | A dropped bad client wasn't closed until the next accept (found by a test) | The connection thread calls `shutdown()` as it exits; the fd is still closed after the join |
| 5 | Async-signal-safety with threads | Block SIGINT/SIGTERM before creating threads; receive them through `signalfd` in the poll loop |
| 6 | Hashing 1 GiB without 1 GiB of memory | Streaming SHA-1 with one reusable 512 KiB buffer: peak ≈ 5 MB, measured with `/usr/bin/time` and `/proc/<pid>/status` |
| 7 | Detecting a **frozen** (not crashed) tracker or peer | Timeouts on every request (5 s tracker, 10 s peer) and heartbeats + receive timeout on the sync link |
| 8 | A stale disconnect logging out a newer login after failover | Per-login session ids; LOGOUT only removes the session if the id still matches |
| 9 | Duplicate or skipped changes when the sync link (re)connects | "Apply+enqueue" and "snapshot+clear queue" under one mutex |
| 10 | Both trackers must converge identically after a partition | Merge rules defined on tracker 1's / tracker 2's copies (not mine/theirs); canonical snapshots compared byte-for-byte in tests |
| 11 | A dead tracker's sessions lingering as "online" ghost seeders | Session ownership: each tracker is authoritative for its own; drop the peer's sessions when the link dies; clients re-announce after failover |
| 12 | Parallel workers must never fetch the same piece and must not deadlock | A central picker with an IN_PROGRESS state + condition variable (WAIT vs NONE); proven with an 8-thread × 2000-piece exactly-once test under TSan |
| 13 | A half-written file appearing under its final name | Write to `.part`; verify; `link()`/`rename()` atomically; delete `.part` on failure |
| 14 | Tests that passed by luck: parallel downloads made "corrupt piece 1" non-deterministic; asynchronous replication made "act on the other tracker immediately" racy | Corrupt *every* piece of the bad seeder (exactly one bad piece is then guaranteed); wait/retry until replicated state is visible |
| 15 | Windows/WSL tooling: PowerShell expanding `$var` inside `bash -c`; `sed` over all files corrupting object files | Scripts kept in files; `scripts/fix_crlf.sh` touches text files only |
| 16 | Proving robustness rather than claiming it | `tests/failures.sh`: kill/freeze seeders and trackers mid-download, downloader crash, both trackers down, fd/thread counts under 100 connection bursts, all under ASan/TSan/valgrind too |

---

## 6. Verification summary
- **0 compiler warnings.**
- **Unit tests:** 550 checks over 7 programs.
- **Scripts:**
  - SHA-1 cross-check against `sha1sum` (10 sizes, plus 1 GiB);
  - tracker smoke test;
  - integration (173 checks);
  - failure injection (109 checks);
  - 1 GiB end-to-end (46 checks).
- **Modes:** everything runs in plain, ASan+UBSan, TSan and valgrind modes with no reports, no leaks and no leaked file descriptors.

---

## 7. References
1. NIST, *FIPS PUB 180-4: Secure Hash Standard* (SHA-1 definition and padding).
2. D. Eastlake, P. Jones, *RFC 3174: US Secure Hash Algorithm 1 (SHA1)* (test vectors).
3. B. Cohen, *The BitTorrent Protocol Specification* (BEP 3), and *Incentives Build Robustness in BitTorrent* (2003) (pieces, piece maps, rarest-first).
4. S. Gilbert, N. Lynch, *Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services* (2002) (CAP).
5. D. Ongaro, J. Ousterhout, *In Search of an Understandable Consensus Algorithm (Raft)* (2014) (why consensus needs a majority).
6. F. Schneider, *Implementing Fault-Tolerant Services Using the State Machine Approach* (1990) (state machine replication).
7. W. R. Stevens, B. Fenner, A. Rudoff, *UNIX Network Programming, Vol. 1* (sockets, partial I/O, SIGPIPE, non-blocking connect with timeout).
8. Linux man pages: `socket(7)`, `tcp(7)` (`SO_REUSEADDR`, Nagle), `send(2)` (`MSG_NOSIGNAL`), `pwrite(2)`, `ftruncate(2)`, `rename(2)`, `link(2)`, `signalfd(2)`, `pthread_sigmask(3)`.
9. *Mutation testing* and *fault injection* as general testing practices (used here to validate the tests themselves).
