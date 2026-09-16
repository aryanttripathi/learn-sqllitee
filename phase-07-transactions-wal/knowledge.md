# Phase 7 — Transactions, Isolation, WAL, and Recovery

Phase 2 covered the rollback journal. This phase covers the *other* durability engine —
WAL — plus the full transaction semantics layered on top of both.

---

## 7.1 Transaction semantics at the SQL level

```sql
BEGIN;              -- DEFERRED (default): no lock taken until the first read/write
BEGIN DEFERRED;
BEGIN IMMEDIATE;    -- take a write lock (RESERVED / WAL write lock) right now
BEGIN EXCLUSIVE;    -- rollback mode: take EXCLUSIVE now; WAL mode: same as IMMEDIATE
COMMIT;  /  END;
ROLLBACK;
SAVEPOINT s;  RELEASE s;  ROLLBACK TO s;
```

| Form | First lock | Typical failure mode |
|---|---|---|
| `BEGIN` | none until first statement | upgrade deadlock → `SQLITE_BUSY` that retrying can't fix |
| `BEGIN IMMEDIATE` | writer lock immediately | clean `SQLITE_BUSY` at the start, retry works |
| `BEGIN EXCLUSIVE` | exclusive immediately | blocks readers too (rollback mode) |

**Rule for application code:** if a transaction will write, use `BEGIN IMMEDIATE`. This
single change removes most "database is locked" bugs.

### Isolation levels
- **Rollback mode:** serializable. Readers and the writer cannot overlap at the moment of
  the actual database write (EXCLUSIVE lock).
- **WAL mode:** readers get a **snapshot** taken at the start of their read transaction
  (snapshot isolation); one writer proceeds concurrently. Writers still serialize.
- **Within one connection**, a statement sees the connection's own uncommitted changes.
- `PRAGMA read_uncommitted` only matters in shared-cache mode (legacy).

### Autocommit
Every statement outside an explicit transaction is wrapped in its own transaction. That is
why 1000 inserts without `BEGIN` cost 1000 fsyncs and 1000 inserts inside `BEGIN...COMMIT`
cost one. `sqlite3_get_autocommit()` reports the state.

---

## 7.2 WAL: the idea

Rollback journal: **write the old data elsewhere, then modify the database.**
WAL: **leave the database alone, append the new data elsewhere.**

```
 rollback journal                          WAL
 ────────────────                          ───
 db  : [ p1 ][ p2 ][ p3 ]                  db  : [ p1 ][ p2 ][ p3 ]      ← unchanged
 jrnl: [old p2]                            wal : [frame p2'][frame p3'][frame p2'']
 write p2 into db                          readers read db, but override with the
 commit = delete journal                   newest frame for each page they need
                                           commit = write a frame with dbsize != 0
```

Consequences:
- **Readers never block the writer, and the writer never blocks readers.** Each reader has
  a fixed "end mark" in the WAL and simply ignores later frames.
- Commit is a small append + one fsync (or none with `synchronous=NORMAL`), instead of
  writing every changed page twice.
- The database file is updated later, in bulk, by a **checkpoint**.
- Costs: a third file (`-shm`), no network filesystems, readers must do a WAL index lookup
  per page, and long-running readers prevent checkpointing (WAL growth).

---

## 7.3 The WAL file format (verified bytes)

```
WAL header (32 bytes, big-endian):
 0   4  magic: 0x377f0682 or 0x377f0683; the LOW BIT selects the byte order used
        for checksum word reads (bit=0 little-endian, bit=1 big-endian).
        In wal.c: hdr.bigEndCksum = magic&1, and walChecksumBytes() reads words
        natively when bigEndCksum == SQLITE_BIGENDIAN.
 4   4  file format version = 3007000
 8   4  database page size
12   4  checkpoint sequence number
16   4  salt-1  (incremented on every WAL reset/restart)
20   4  salt-2  (random on every reset)
24   4  checksum-1 of the first 24 bytes
28   4  checksum-2

Frame header (24 bytes) before every page image:
 0   4  page number
 4   4  database size in pages AFTER this commit — NON-ZERO ⇒ this is a COMMIT frame
 8   4  salt-1 copied from the WAL header
12   4  salt-2
16   4  checksum-1  (cumulative, chained from the previous frame)
20   4  checksum-2
```

Real bytes from a live database on this machine:

```
$ xxd -l 56 w.db-wal
00000000: 377f 0682 002d e218 0000 1000 0000 0000  7....-..........
          ^magic (LSB=0 -> little-endian checksum words)
               ^3007000  ^pagesize  ^ckpt seq=0
00000010: 2723 4773 66b1 d4d2 482e 263a d2ea af45  '#Gsf...H.&:...E
          ^salt-1   ^salt-2   ^cksum-1  ^cksum-2
00000020: 0000 0001 0000 0000 2723 4773 66b1 d4d2  ← FRAME 1 HEADER
          ^pgno=1   ^dbsize=0 (not a commit)  ^salts copied
00000030: 394f 71f7 617e 5cb2                      ^cksum-1 ^cksum-2

$ xxd -s 4152 -l 24 w.db-wal      # frame 2 = 32 + 24 + 4096 + ...
00001038: 0000 0002 0000 0002 2723 4773 66b1 d4d2
          ^pgno=2   ^dbsize=2  ← NON-ZERO ⇒ COMMIT FRAME: the txn ends here
00001048: 29b9 b3a5 56e2 6241
```

**The commit point in WAL mode is the frame whose "database size" field is non-zero.** If a
crash truncates the WAL mid-transaction, recovery simply stops at the last valid commit
frame — the incomplete frames are invisible because the checksum chain breaks.

### The checksum algorithm

```c
/* s0,s1 carried forward from the previous frame (or the header) */
for(i=0; i<n; i+=2){          /* n = number of 32-bit words */
  s0 += x[i]   + s1;
  s1 += x[i+1] + s0;
}
```
A Fibonacci-weighted running sum over 32-bit words, chained across frames. It is fast and
detects torn/reordered writes, which is all it must do. The byte order used is chosen by
the magic number, so a WAL is not portable across endianness (unlike the database file).

**Salts** are the other half of the safety argument: on WAL reset the salts change, so old
frames left in the file can never be mistaken for current ones.

---

## 7.4 The wal-index (`-shm`) — how readers find pages fast

Scanning the whole WAL for every page read would be O(WAL size). Instead, all connections
share a memory-mapped index file:

```
 <db>-shm layout:
 ┌──────────────────────────────────────────────────────────┐
 │ WalIndexHdr  (two copies, for atomic update detection)   │
 │   iVersion, unused, iChange, isInit, bigEndCksum,        │
 │   szPage, mxFrame, nPage, aFrameCksum[2], aSalt[2],       │
 │   aCksum[2]                                              │
 ├──────────────────────────────────────────────────────────┤
 │ WalCkptInfo                                              │
 │   nBackfill       — frames already copied into the db    │
 │   aReadMark[5]    — the five reader "end marks"          │
 │   aLock[SQLITE_SHM_NLOCK]                                │
 │   nBackfillAttempted, ...                                │
 ├──────────────────────────────────────────────────────────┤
 │ index block 0:  page-number array for frames 1..4096     │
 │                 + an 8192-slot hash table                │
 ├──────────────────────────────────────────────────────────┤
 │ index block 1:  frames 4097..8192 + hash table           │
 │ ...                                                      │
 └──────────────────────────────────────────────────────────┘
```

Page lookup: hash the page number, probe the hash table of the *newest* index block first,
walking backwards, and take the **largest frame number ≤ my read mark** that holds that
page. O(1) in practice.

The `-shm` file is pure derived data: delete it and it is rebuilt by WAL recovery.

### The shm locks (this is the real concurrency protocol)

```
slot 0 : WAL_WRITE_LOCK     — one writer at a time
slot 1 : WAL_CKPT_LOCK      — one checkpointer at a time
slot 2 : WAL_RECOVER_LOCK   — one recoverer at a time
slot 3 : WAL_READ_LOCK(0)   — readers using only the db file (aReadMark[0] == 0)
slot 4 : WAL_READ_LOCK(1)   ┐
slot 5 : WAL_READ_LOCK(2)   │ each paired with aReadMark[i], the reader's WAL end frame
slot 6 : WAL_READ_LOCK(3)   │
slot 7 : WAL_READ_LOCK(4)   ┘
```

### Reader algorithm (`walTryBeginRead`)
```
1. read the WalIndexHdr (twice, compare, to catch a torn update)
2. find the read mark with the largest value <= mxFrame
   - if none is suitable, try to SET one to mxFrame (needs an exclusive shm lock briefly)
3. take a SHARED lock on that read mark's slot
4. re-check the header didn't change; if it did, retry
5. snapshot = "the database file, overridden by WAL frames 1..aReadMark[i]"
```
The reader then holds that shared lock for the whole read transaction. A checkpointer may
not backfill past `min(aReadMark[])`, which is exactly how long-running readers stall
checkpointing.

### Writer algorithm
```
1. take WAL_WRITE_LOCK (exclusive)
2. verify my snapshot is still current (else SQLITE_BUSY_SNAPSHOT)
3. append frames for every dirty page
4. the last frame of the transaction carries dbsize != 0  ← COMMIT
5. (synchronous=FULL) fsync the WAL before writing the commit frame
6. update mxFrame in the WalIndexHdr, release the write lock
```

### Checkpointer algorithm (`walCheckpoint`)
```
1. take WAL_CKPT_LOCK
2. mxSafeFrame = min(aReadMark[1..4] in use, mxFrame)
3. copy frames nBackfill+1 .. mxSafeFrame into the database file
   (for each page, only the NEWEST frame ≤ mxSafeFrame)
4. fsync the database
5. nBackfill = mxSafeFrame
6. if every frame was backfilled and no reader is using the WAL:
      reset the WAL (salts change, mxFrame=0) so it is reused from the start
```

---

## 7.5 Checkpoint modes

| Mode | Behaviour |
|---|---|
| `PASSIVE` (default, auto) | copy what you can without waiting; never blocks readers or writers |
| `FULL` | wait for readers so that **all** frames can be backfilled |
| `RESTART` | FULL, plus wait until no reader is using the WAL, so the next writer restarts at the beginning of the file |
| `TRUNCATE` | RESTART, plus truncate the WAL to zero bytes |

```sql
PRAGMA wal_checkpoint(PASSIVE);   -- returns (busy, nLog, nCheckpointed)
PRAGMA wal_autocheckpoint = 1000; -- pages; 0 disables; default 1000 (≈4 MB at 4 KiB)
PRAGMA journal_size_limit = N;    -- truncate the WAL back to N bytes after checkpoint
```

`sqlite3_wal_checkpoint_v2()` is the C API; `sqlite3_wal_hook()` lets you run your own
checkpoint policy (e.g. checkpoint only when the WAL exceeds X and no reader is active).

**WAL growth** is the classic production problem: a long-running read transaction pins
`aReadMark`, so nothing can be backfilled, so the WAL grows without bound. Diagnosis:
`PRAGMA wal_checkpoint(PASSIVE)` returns a non-zero busy flag and a large `nLog`. Fix: stop
holding read transactions open (usually an un-finalized statement).

---

## 7.6 `PRAGMA synchronous` in WAL mode (different meaning!)

| Level | WAL-mode behaviour | Risk |
|---|---|---|
| `OFF` | no syncs at all | power loss can corrupt |
| `NORMAL` | sync the WAL only at checkpoints, not at each commit | **power loss loses recent commits but cannot corrupt** |
| `FULL` | sync the WAL at every commit | nothing lost |
| `EXTRA` | as FULL, plus sync the directory on journal delete | maximal |

`journal_mode=WAL` + `synchronous=NORMAL` is the standard high-throughput production
setting, and it is safe against process crashes and OS crashes — only a power failure or
hard kernel panic can lose the most recent transactions, never the database structure.

---

## 7.7 WAL recovery

If a connection opens a database with a WAL and no valid `-shm` (or it is the first
connection):

```
walIndexRecover():
  1. take WAL_RECOVER_LOCK exclusively
  2. read the WAL header; verify the magic and the header checksum
     - if invalid, the WAL is ignored entirely (treated as empty)
  3. walk frames from the start, verifying the checksum chain and salts
  4. stop at the first frame that fails a checksum or has wrong salts
  5. mxFrame = the last frame of the last *commit* transaction seen
  6. rebuild the wal-index hash tables from those frames
```

Note what recovery does **not** do: it does not modify the database file. Recovery is pure
index reconstruction, which makes it fast and idempotent.

The `-shm` and `-wal` files are deleted when the last connection closes (unless
`PRAGMA persist_wal`/`SQLITE_FCNTL_PERSIST_WAL` is set). If the process crashes, they
remain, and the next opener recovers.

---

## 7.8 Savepoints and nested transactions

```sql
BEGIN;
  INSERT ...;
  SAVEPOINT a;
     UPDATE ...;
     SAVEPOINT b;
        DELETE ...;
     ROLLBACK TO b;      -- undo the DELETE, keep the UPDATE, b still open
  RELEASE a;             -- merge a's changes into the outer transaction
COMMIT;
```

Implementation (`PagerSavepoint`):
```c
struct PagerSavepoint {
  i64 iOffset;         /* starting offset in the main journal */
  i64 iHdrOffset;
  Bitvec *pInSavepoint;/* pages written since this savepoint */
  Pgno nOrig;          /* original database size */
  Pgno iSubRec;        /* index of the first record in the sub-journal */
  u32 aWalData[WAL_SAVEPOINT_NDATA];   /* WAL frame counts, for WAL mode */
};
```
- In rollback mode, rolling back to a savepoint replays journal records from `iOffset` plus
  the sub-journal records from `iSubRec`.
- In WAL mode it truncates the WAL back to the recorded frame count.
- `SAVEPOINT` outside a transaction starts one; `RELEASE` of the outermost commits.

**Statement journals** are the implicit version: every statement that can abort partway
(constraint violation with `ON CONFLICT ABORT`, the default) opens one, so the statement can
be undone without killing the transaction.

---

## 7.9 Conflict resolution

```sql
INSERT OR ROLLBACK/ABORT/FAIL/IGNORE/REPLACE INTO ...
CREATE TABLE t(a UNIQUE ON CONFLICT REPLACE, ...);
INSERT ... ON CONFLICT(col) DO UPDATE SET ...   -- upsert (3.24+)
```

| Mode | On violation |
|---|---|
| `ROLLBACK` | abort the statement **and** roll back the whole transaction |
| `ABORT` (default) | undo this statement only (needs the statement journal), keep the transaction |
| `FAIL` | stop the statement where it is; **prior changes in the statement remain** |
| `IGNORE` | skip the offending row, continue |
| `REPLACE` | delete the conflicting rows, then insert (fires delete triggers only if `recursive_triggers` is on) |

`FAIL` is the surprising one — it is the only mode that leaves a statement half-applied.

---

## 7.10 Foreign keys and deferred constraints

```sql
PRAGMA foreign_keys = ON;        -- OFF by default for backwards compatibility!
... REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED
```
Immediate FK constraints are checked per statement; deferred ones are counted in a VDBE
register (`OP_FkCounter`) and verified at `COMMIT` (`OP_FkIfZero`). `PRAGMA
foreign_key_check` verifies an entire database offline.

---

## 7.11 The snapshot API and BEGIN CONCURRENT

```c
sqlite3_snapshot_get(db, "main", &pSnap);       /* capture the current WAL snapshot */
sqlite3_snapshot_open(db, "main", pSnap);       /* start a read txn at that snapshot */
sqlite3_snapshot_recover(db, "main");
sqlite3_snapshot_cmp(p1, p2);
```
This exposes WAL snapshot isolation directly: useful for consistent multi-statement reads
or for a reader that must see exactly the state another connection saw.

`BEGIN CONCURRENT` (an official but separate branch, and the basis of libSQL/hctree work)
allows multiple write transactions to proceed optimistically and conflict-check page sets
at commit. It is not in the mainline release; know that it exists and why it is hard: the
b-tree page-level granularity makes false conflicts common.

---

## 7.12 Choosing a mode (practical summary)

| Situation | Setting |
|---|---|
| Server-ish app, many readers, one writer | `journal_mode=WAL`, `synchronous=NORMAL`, `busy_timeout=5000`, `BEGIN IMMEDIATE` for writes |
| Single-process embedded, must survive power loss | WAL + `synchronous=FULL` |
| Read-only distribution (e.g. a shipped dataset) | rollback mode, `PRAGMA query_only`, or `immutable=1` URI |
| Network filesystem | **don't** — but if forced: rollback mode + `unix-dotfile` VFS |
| Bulk load | one big transaction, `PRAGMA synchronous=OFF` temporarily, `PRAGMA journal_mode=OFF`, then rebuild and turn durability back on |
| Many tiny writes | batch them; one fsync per transaction dominates everything |

---

## 7.13 How corruption actually happens

From `sqlite.org/howtocorrupt.html` — the causes, in rough frequency order:
1. **A buggy or lying `fsync()`** (network filesystems, some flash controllers, containers
   with `nobarrier`).
2. **Two processes using different locking semantics** on the same file (e.g. one using a
   custom VFS with `unix-none`).
3. **Deleting or copying the `-journal`/`-wal` file** while the database is in use.
   Backups must copy all files together, or use `sqlite3_backup`/`VACUUM INTO`.
4. **`journal_mode=MEMORY`/`OFF`** plus a crash.
5. Hardware faults, bad RAM.
6. File descriptors leaking across `fork()`.
7. A process crashing while holding the file open on a filesystem that reorders writes.

The practical safety checklist: WAL + `synchronous` ≥ NORMAL, never move the sidecar files,
use `VACUUM INTO` or the backup API for copies, don't put databases on NFS, and run
`PRAGMA integrity_check` in your test suite.
