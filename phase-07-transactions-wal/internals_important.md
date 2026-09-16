# Phase 7 — Internals Reference: Transactions & WAL

## Constants

```
WAL magic            0x377f0682 (LSB 0 ⇒ little-endian checksum words)
                     0x377f0683 (LSB 1 ⇒ big-endian checksum words)
WAL format version   3007000
WAL header size      32 bytes
WAL frame header     24 bytes
Frame size           24 + page size
File size            32 + nFrame*(24+pageSize)
Commit frame         frame header bytes 4..7 (db size in pages) != 0
Default -shm size    32768 bytes (grown as needed)
HASHTABLE_NPAGE      4096   frames covered per wal-index block
HASHTABLE_NSLOT      8192   hash slots per block (2× NPAGE, so load factor ≤ 0.5)
SQLITE_SHM_NLOCK     8      shm lock slots
WAL_WRITE_LOCK       0
WAL_CKPT_LOCK        1
WAL_RECOVER_LOCK     2
WAL_READ_LOCK(i)     3+i    for i in 0..4  (5 read marks)
wal_autocheckpoint   1000 pages by default
```

## Files
| File | Contents |
|---|---|
| `wal.c` | the whole WAL implementation; its header comment is the design document |
| `pager.c` | switches between journal and WAL paths; savepoints; commit phases |
| `os_unix.c` | `unixShmMap`/`unixShmLock` — the `-shm` file and its locks |
| `vdbeaux.c` | `sqlite3VdbeHalt`, `vdbeCommit` — the multi-database commit protocol |
| `main.c` | `sqlite3_wal_hook`, `sqlite3_wal_checkpoint_v2`, `sqlite3_snapshot_*` |
| `btree.c` | `sqlite3BtreeBeginTrans/CommitPhaseOne/CommitPhaseTwo/Savepoint` |
| `fkey.c` | deferred FK counters (`OP_FkCounter`, `OP_FkIfZero`) |

## Structures

```c
struct WalIndexHdr {          /* stored TWICE in the -shm for torn-write detection */
  u32 iVersion;
  u32 unused;
  u32 iChange;                /* bumped on every transaction */
  u8 isInit;
  u8 bigEndCksum;
  u16 szPage;                 /* page size / 256, or 1 for 65536 */
  u32 mxFrame;                /* the last valid frame in the WAL */
  u32 nPage;                  /* database size in pages */
  u32 aFrameCksum[2];         /* running checksum after mxFrame */
  u32 aSalt[2];
  u32 aCksum[2];              /* checksum of this header */
};

struct WalCkptInfo {
  u32 nBackfill;              /* frames already copied into the db file */
  u32 aReadMark[WAL_NREADER]; /* 5 reader end-marks; aReadMark[0] is always 0 */
  u8  aLock[SQLITE_SHM_NLOCK];
  u32 nBackfillAttempted;
  u32 notUsed0;
};

struct Wal {
  sqlite3_vfs *pVfs;
  sqlite3_file *pDbFd, *pWalFd;
  u32 iCallback;
  i64 mxWalSize;              /* journal_size_limit */
  int nWiData; volatile u32 **apWiData;   /* the mapped -shm blocks */
  u32 szPage;
  i16 readLock;               /* which read mark this connection holds (-1 = none) */
  u8 syncFlags, exclusiveMode, writeLock, ckptLock, readOnly, truncateOnCommit;
  u8 syncHeader, padToSectorBoundary, bShmUnreliable;
  WalIndexHdr hdr;            /* this connection's snapshot of the header */
  u32 minFrame;               /* the earliest frame this reader may use */
  ...
};
```

## Key functions (`wal.c`)
| Function | Role |
|---|---|
| `sqlite3WalOpen` / `Close` | open the `-wal`, set up shm |
| `walIndexRecover()` | rebuild the wal-index by scanning and checksum-verifying frames |
| `walChecksumBytes()` | the Fibonacci checksum |
| `walIndexHdr()` / `walIndexWriteHdr()` | read/write the two header copies |
| `walTryBeginRead()` / `sqlite3WalBeginReadTransaction()` | the reader protocol |
| `sqlite3WalEndReadTransaction()` | release the read mark |
| `sqlite3WalFindFrame()` | hash lookup: page → newest frame ≤ my read mark |
| `sqlite3WalReadFrame()` | read a page image out of the WAL |
| `sqlite3WalBeginWriteTransaction()` | take `WAL_WRITE_LOCK`, validate snapshot |
| `sqlite3WalFrames()` | append frames; the last one carries the commit size |
| `walWriteOneFrame()` / `walEncodeFrame()` | build a frame header + checksum |
| `sqlite3WalCheckpoint()` / `walCheckpoint()` | backfill into the database |
| `walRestartHdr()` | reset the WAL (new salts) so it is reused from frame 1 |
| `sqlite3WalSavepoint()` / `SavepointUndo()` | truncate back to a frame count |
| `sqlite3WalSnapshotGet/Open/Recover/Cmp` | the snapshot API |
| `walIndexAppend()` / `walHash()` / `walNextHash()` | the wal-index hash tables |

## Protocol summaries

**Reader**
```
loop:
  hdr = read WalIndexHdr twice, compare  (retry if torn)
  if mxFrame == nBackfill and no WAL content needed:
      use read mark 0 (pure database file), lock slot 3
  else:
      find i with aReadMark[i] <= mxFrame, maximizing aReadMark[i]
      if none suitable: try to claim a slot and set aReadMark[i] = mxFrame
      SHARED-lock WAL_READ_LOCK(i)
  re-read the header; if changed, unlock and retry
snapshot = db file + WAL frames [minFrame .. aReadMark[i]]
```

**Writer**
```
EXCLUSIVE-lock WAL_WRITE_LOCK
if my hdr != shared hdr   -> SQLITE_BUSY_SNAPSHOT (my read snapshot is stale)
append frames; the final frame of the txn has dbsize != 0
(synchronous >= FULL) fsync the WAL before the commit frame is durable
update mxFrame + iChange in the WalIndexHdr (both copies)
release WAL_WRITE_LOCK
```

**Checkpointer**
```
EXCLUSIVE-lock WAL_CKPT_LOCK
mxSafeFrame = mxFrame; for each in-use read mark, mxSafeFrame = min(..., aReadMark[i])
copy pages for frames nBackfill+1..mxSafeFrame into the db (newest frame per page wins)
fsync the db; nBackfill = mxSafeFrame
if fully backfilled and no readers are in the WAL: walRestartHdr() (new salts, mxFrame=0)
mode RESTART/TRUNCATE additionally waits for readers and truncates the file
```

## `PRAGMA synchronous` — the two different meanings
| Level | Rollback mode | WAL mode |
|---|---|---|
| OFF | can corrupt on power loss | can corrupt on power loss |
| NORMAL | can corrupt on power loss (on some filesystems) | **cannot corrupt**; may lose recent commits |
| FULL | safe | safe |
| EXTRA | safe + directory sync | safe + directory sync |

## Pragmas / APIs
```sql
PRAGMA journal_mode = WAL | DELETE | TRUNCATE | PERSIST | MEMORY | OFF;
PRAGMA wal_autocheckpoint = 1000;
PRAGMA wal_checkpoint(PASSIVE|FULL|RESTART|TRUNCATE);   -- returns (busy,nLog,nCkpt)
PRAGMA journal_size_limit = N;
PRAGMA synchronous = OFF|NORMAL|FULL|EXTRA;
PRAGMA locking_mode = NORMAL|EXCLUSIVE;
PRAGMA busy_timeout = ms;
PRAGMA foreign_keys = ON;      PRAGMA defer_foreign_keys = ON;
PRAGMA foreign_key_check;      PRAGMA integrity_check;
PRAGMA query_only = ON;
```
```c
sqlite3_busy_timeout(db, ms);   sqlite3_busy_handler(db, cb, arg);
sqlite3_wal_hook(db, cb, arg);  sqlite3_wal_autocheckpoint(db, N);
sqlite3_wal_checkpoint_v2(db, "main", mode, &nLog, &nCkpt);
sqlite3_snapshot_get/open/free/cmp/recover(...);
sqlite3_backup_init/step/finish(...);
sqlite3_get_autocommit(db);
sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_WRITE, ...);
```

## Error codes to distinguish
```
SQLITE_BUSY              lock unavailable; retrying may work
SQLITE_BUSY_SNAPSHOT     WAL: your read snapshot is stale; you must restart the txn
SQLITE_BUSY_RECOVERY     another process is running WAL recovery
SQLITE_BUSY_TIMEOUT      the busy timeout elapsed (blocking VFS)
SQLITE_LOCKED            table-level conflict within the same process (shared cache)
SQLITE_LOCKED_SHAREDCACHE
SQLITE_PROTOCOL          shm protocol failure / too many retries
SQLITE_CANTOPEN          often a missing directory or a WAL on a read-only filesystem
SQLITE_READONLY_ROLLBACK a hot journal exists but the file is read-only
SQLITE_READONLY_RECOVERY WAL recovery needed but the file is read-only
```

## Gotchas

1. `journal_mode=WAL` is **persistent** in the database header (bytes 18/19 become 2).
2. A WAL database needs the directory to be writable (for `-wal` and `-shm`) — a read-only
   WAL database can only be opened if the WAL is already checkpointed and you pass
   `immutable=1` or `PRAGMA query_only`.
3. **WAL does not work on network filesystems** (no reliable shared memory).
4. `-shm` is derived data; deleting it while no connection is open is harmless.
   Deleting the `-wal` loses committed transactions.
5. Copying a WAL database requires copying `db`+`-wal` together (and even then, only when
   no writer is active). Prefer `VACUUM INTO` or the backup API.
6. A long-running read transaction stops checkpointing ⇒ unbounded WAL growth. Find the
   culprit with `PRAGMA wal_checkpoint(PASSIVE)` returning busy.
7. `PRAGMA locking_mode=EXCLUSIVE` + WAL keeps the wal-index in heap memory — no `-shm`
   file at all, and a large speedup for single-process use.
8. `BEGIN EXCLUSIVE` in WAL mode behaves like `BEGIN IMMEDIATE`; there is no way to block
   readers in WAL mode.
9. The busy handler is **not** invoked for `SQLITE_BUSY_SNAPSHOT` — the transaction must be
   restarted by the application.
10. `sqlite3_close()` on the last connection checkpoints and deletes the WAL, which is why
    naive inspection of a `-wal` after a shell exits shows an empty file.
11. Deferred foreign keys are counted in a VDBE register; an uncommitted violation at
    `COMMIT` returns `SQLITE_CONSTRAINT_FOREIGNKEY` and the transaction stays open.
12. `PRAGMA foreign_keys` is **OFF by default** and cannot be changed inside a transaction.

## Canonical reading
`sqlite.org/wal.html`, `sqlite.org/walformat.html`, `sqlite.org/atomiccommit.html`,
`sqlite.org/lockingv3.html`, `sqlite.org/howtocorrupt.html`, `sqlite.org/isolation.html`,
and the 400-line header comment at the top of `src/wal.c` — which is better than all of them.
