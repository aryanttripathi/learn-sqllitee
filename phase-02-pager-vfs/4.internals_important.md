# Phase 2 — Internals Reference: Pager, Locking, VFS

## Constants

```
PENDING_BYTE    0x40000000 (1 GiB)   ; the lock byte page starts here
RESERVED_BYTE   PENDING_BYTE + 1
SHARED_FIRST    PENDING_BYTE + 2
SHARED_SIZE     510
Journal magic   d9 d5 05 f9 20 a1 63 d7
Journal hdr     magic(8) nRec(4) cksumInit(4) dbSize(4) sectorSize(4) pageSize(4) = 28 bytes,
                padded up to sectorSize (min 512)
Journal record  pgno(4) | pageSize bytes | cksum(4)
nRec = 0xffffffff  ⇒ "unknown, compute from file size" (transaction still in flight)
Default sector  512 (SQLITE_DEFAULT_SECTOR_SIZE 4096 on some builds); from xSectorSize
Sync flags      SQLITE_SYNC_NORMAL 0x2, SQLITE_SYNC_FULL 0x3, SQLITE_SYNC_DATAONLY 0x10
```

## Pager states (`pager.c`, `Pager.eState`)

| Value | State | Locks held | db file |
|---|---|---|---|
| 0 | `PAGER_OPEN` | NONE (or UNKNOWN) | untouched |
| 1 | `PAGER_READER` | SHARED | consistent snapshot |
| 2 | `PAGER_WRITER_LOCKED` | RESERVED | untouched; journal header written |
| 3 | `PAGER_WRITER_CACHEMOD` | RESERVED | untouched; dirty pages in cache |
| 4 | `PAGER_WRITER_DBMOD` | EXCLUSIVE | **contains uncommitted data** |
| 5 | `PAGER_WRITER_FINISHED` | EXCLUSIVE | committed, journal not yet finalized |
| 6 | `PAGER_ERROR` | any | unknown; cache must be reset |

Lock levels (`Pager.eLock`, `sqlite3_file` xLock arg):
`SQLITE_LOCK_NONE 0`, `SHARED 1`, `RESERVED 2`, `PENDING 3`, `EXCLUSIVE 4`.

## Key functions (`pager.c`)

| Function | Role |
|---|---|
| `sqlite3PagerOpen()` | create Pager, open file, compute page size |
| `sqlite3PagerSharedLock()` | OPEN → READER; **hot journal detection + playback** |
| `hasHotJournal()` | the four-condition test |
| `pager_playback()` | roll back a hot (or aborted) journal |
| `pager_playback_one_page()` | verify checksum, write original page back |
| `sqlite3PagerBegin()` | READER → WRITER_LOCKED (take RESERVED) |
| `sqlite3PagerWrite()` | mark page writeable; journals the original on first call |
| `pagerAddPageToRollbackJournal()` / `pager_write()` | append a journal record |
| `pager_write_pagelist()` | flush dirty pages to the db file |
| `pagerStress()` | pcache callback when cache is full → **spill** |
| `sqlite3PagerCommitPhaseOne()` | sync journal, write db, sync db |
| `sqlite3PagerCommitPhaseTwo()` | finalize journal = commit point; drop locks |
| `sqlite3PagerRollback()` | undo via journal playback |
| `pager_end_transaction()` | journal finalization per journal mode |
| `sqlite3PagerOpenSavepoint()` / `sqlite3PagerSavepoint()` | savepoint create/rollback |
| `pagerOpenSubjournal()` | statement journal (often in-memory) |
| `sqlite3PagerSetCachesize()` / `SetJournalMode()` / `SetFlags()` | pragmas land here |
| `pagerSyncHotJournal()`, `pager_delsuper()` | super-journal handling |
| `pager_cksum()` | journal record checksum (cksumInit + byte sum, sparse) |

The checksum is deliberately weak and sampled (every 200th byte): it exists to detect
*torn writes*, not malicious tampering.

## Key structures

```c
struct Pager {
  sqlite3_vfs *pVfs;
  u8 exclusiveMode, journalMode, useJournal, noSync, fullSync, extraSync;
  u8 syncFlags, walSyncFlags, tempFile, noLock, readOnly, memDb;
  u8 eState, eLock, changeCountDone, setSuper, doNotSpill, subjInMemory;
  Pgno dbSize, dbOrigSize, dbFileSize, dbHintSize;
  int errCode;
  int nRec;                 /* records in the journal */
  u32 cksumInit;            /* nonce */
  u32 nSubRec;              /* records in the sub-journal */
  Bitvec *pInJournal;       /* one bit per page: "original already journaled" */
  sqlite3_file *fd, *jfd, *sjfd;   /* db, journal, sub-journal */
  i64 journalOff, journalHdr, journalSizeLimit;
  PagerSavepoint *aSavepoint; int nSavepoint;
  PCache *pPCache;
  Wal *pWal;
  ...
};
```

`pInJournal` is a `Bitvec` — the "have I already saved this page's original?" test must be
O(1) and must not allocate per page. Read `bitvec.c`; it is 400 lines of elegance:
below 4000 pages it is a plain bit array, above that it becomes a 2-level hash/tree.

## Page cache (`pcache.c` / `pcache1.c`)

| Function | Role |
|---|---|
| `sqlite3PcacheFetch()` / `FetchFinish()` | get a page, maybe allocating |
| `sqlite3PcacheMakeDirty()` / `MakeClean()` | dirty-list maintenance |
| `sqlite3PcacheRelease()` | unpin (nRef--) |
| `sqlite3PcacheDirtyList()` | sorted list of dirty pages for flushing |
| `sqlite3PcacheTruncate()` | drop pages above N (rollback/truncate) |
| `pcache1Fetch()` / `pcache1Unpin()` | default implementation, LRU |

Flags on `PgHdr`: `PGHDR_CLEAN`, `PGHDR_DIRTY`, `PGHDR_WRITEABLE`, `PGHDR_NEED_SYNC`,
`PGHDR_DONT_WRITE`, `PGHDR_MMAP`.

`PGHDR_NEED_SYNC` is the interesting one: it means "this page's journal record has not
been synced yet, so this page must not be written to the db file". It is the in-memory
encoding of the journal-before-database ordering rule.

Replaceable cache: `sqlite3_config(SQLITE_CONFIG_PCACHE2, &myMethods)` with
`sqlite3_pcache_methods2`.

## VFS reference

Registration: `sqlite3_vfs_register(pVfs, makeDefault)`, `sqlite3_vfs_find(zName)`,
`sqlite3_vfs_unregister(pVfs)`. Select per-connection with `sqlite3_open_v2(..., zVfs)` or
per-URI: `file:x.db?vfs=trace`.

Built-in unix VFSes (`os_unix.c`): `unix` (POSIX advisory locks), `unix-dotfile`
(lock files — use on NFS), `unix-flock`, `unix-none` (no locking), `unix-excl`
(permanent EXCLUSIVE lock; enables heap-memory wal-index), `unix-namedsem` (VxWorks).
Also `memdb` (in-memory VFS backing `:memory:` style databases with `sqlite3_deserialize`).

File-control opcodes you will meet (`xFileControl`):
`SQLITE_FCNTL_LOCKSTATE`, `SIZE_HINT`, `CHUNK_SIZE`, `SYNC`, `COMMIT_PHASETWO`,
`PERSIST_WAL`, `POWERSAFE_OVERWRITE`, `PRAGMA` (lets a VFS implement custom pragmas),
`VFSNAME`, `ZIPVFS`, `BEGIN_ATOMIC_WRITE` / `COMMIT_ATOMIC_WRITE` (F2FS batch atomic
writes), `DATA_VERSION`, `RESERVE_BYTES`.

Device characteristics (`xDeviceCharacteristics`):
`SQLITE_IOCAP_ATOMIC`, `ATOMIC512..ATOMIC64K`, `SAFE_APPEND`, `SEQUENTIAL`,
`UNDELETABLE_WHEN_OPEN`, `POWERSAFE_OVERWRITE`, `IMMUTABLE`, `BATCH_ATOMIC`.

## The POSIX locking workaround

```
unixInodeInfo {          /* one per (dev, inode) per process, in a global list */
  struct unixFileId fileId;   /* device + inode */
  int nShared;                /* # of SHARED locks held by this process */
  unsigned char eFileLock;    /* highest lock held by this process */
  int nRef;                   /* # of unixFile pointing here */
  UnixUnusedFd *pUnused;      /* fds queued for close (closing would drop locks!) */
  ...
}
```

Facts to remember:
- Closing any fd on a file drops **all** POSIX locks that process holds on it ⇒ SQLite
  defers closes via `pUnused`.
- POSIX locks are per-process, not per-fd ⇒ intra-process contention must be tracked in
  `unixInodeInfo`, not by the kernel.
- `fork()` + open database = undefined behaviour. Never carry a connection across a fork.
- NFS/SMB advisory locking is unreliable ⇒ `unix-dotfile` or don't do it.

## Journal modes: what the commit point actually is

| Mode | Commit point operation | Why |
|---|---|---|
| DELETE | `unlink(journal)` (+ dir sync if `synchronous=EXTRA`) | directory entry removal is atomic |
| TRUNCATE | `ftruncate(journal, 0)` | avoids directory metadata sync |
| PERSIST | overwrite the 8-byte magic with zeros (+ sync) | avoids create/unlink entirely |
| MEMORY | journal never existed on disk | fast, unsafe across process death |
| OFF | none | no atomicity at all |
| WAL | write the commit frame (dbsize field non-zero) | Phase 7 |

## Gotchas

1. `PRAGMA journal_mode=WAL` is persistent (stored in the db header bytes 18/19); the
   others are per-connection.
2. `PRAGMA journal_mode=MEMORY` + a crash = corrupt database. Never in production.
3. A cache spill promotes you to EXCLUSIVE early: long transactions with a small cache
   block readers far more than you'd expect.
4. `SQLITE_BUSY` vs `SQLITE_BUSY_SNAPSHOT`: the latter means "your read snapshot is too
   old to upgrade" — retrying without restarting the transaction is futile.
5. The busy handler is **not** called for a lock upgrade that would deadlock.
6. `synchronous=NORMAL` in rollback mode can corrupt on power loss; in WAL mode it cannot
   corrupt, it can only lose recent transactions. Different guarantees, same pragma name.
7. `sqlite3_close()` on a connection with unfinalized statements returns `SQLITE_BUSY`;
   use `sqlite3_close_v2()` for zombie-safe close.
8. Temp files (sorter, statement journals) go through the VFS too — a custom VFS must
   handle `zName == NULL`.
9. Setting `mmap_size` > 0 makes I/O errors into `SIGBUS`.
10. `PRAGMA locking_mode=EXCLUSIVE` keeps the EXCLUSIVE lock across transactions: big
    speedup for single-writer apps, and it lets WAL run without an `-shm` file.

## Further reading (canonical)
`sqlite.org/atomiccommit.html` — the definitive account of §2.3.
`sqlite.org/lockingv3.html` — the lock state machine.
`sqlite.org/vfs.html`, `sqlite.org/c3ref/vfs.html` — VFS contract.
`sqlite.org/howtocorrupt.html` — every way people have destroyed a database. Read it twice.
