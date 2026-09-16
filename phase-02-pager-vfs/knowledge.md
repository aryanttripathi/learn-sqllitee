# Phase 2 — The Pager and the VFS: How SQLite Refuses to Lose Your Data

The b-tree layer says "write page 7". The pager's job is to make that survive a power cut,
a `kill -9`, a full disk, and three other processes doing the same thing. This phase is
where ACID actually lives.

---

## 2.1 The pager's contract

The pager offers the b-tree an illusion:

```
   b-tree sees:  an array of pages it can read and write,
                 with transactions that are all-or-nothing.

   reality:      a page cache in RAM, a journal or WAL on disk,
                 byte-range file locks, and fsync() at exactly the right moments.
```

Pager responsibilities, in order of subtlety:

1. **Caching** — keep hot pages in memory (`pcache1.c`), evict cold ones.
2. **Atomicity** — either all page writes of a transaction land, or none.
3. **Durability** — after commit returns, the data survives power loss.
4. **Isolation** — concurrent readers/writers see consistent snapshots.
5. **Recovery** — if we crashed, the *next* process to open the file must repair it.

Point 5 is the one that surprises people: there is no recovery daemon. Recovery is
performed opportunistically by whoever opens the database next.

---

## 2.2 The pager state machine

From the (excellent) comment block at the top of `pager.c`:

```
        ┌──────────────┐
        │  PAGER_OPEN  │  no locks, no transaction, cache empty-ish
        └──────┬───────┘
               │ sqlite3PagerSharedLock()  → SHARED lock, hot-journal check
        ┌──────▼────────┐
        │ PAGER_READER  │  read transaction open; snapshot is stable
        └──────┬────────┘
               │ sqlite3PagerBegin()       → RESERVED lock
        ┌──────▼─────────────┐
        │ PAGER_WRITER_LOCKED│  write txn started, nothing changed yet
        └──────┬─────────────┘
               │ first sqlite3PagerWrite() → journal header written
        ┌──────▼──────────────┐
        │ PAGER_WRITER_CACHEMOD│ dirty pages in cache, db file untouched
        └──────┬───────────────┘
               │ cache spill or commit → EXCLUSIVE lock, db file writes begin
        ┌──────▼─────────────┐
        │ PAGER_WRITER_DBMOD │  db file now contains uncommitted data
        └──────┬─────────────┘
               │ journal synced + db synced
        ┌──────▼──────────────┐
        │ PAGER_WRITER_FINISHED│ commit point reached
        └──────┬───────────────┘
               │ journal deleted, locks dropped
        ┌──────▼───────┐        any I/O or malloc error at a bad moment
        │ PAGER_READER │  ◄──── ┌──────────────┐
        └──────────────┘        │ PAGER_ERROR  │ → must roll back, reset cache
                                └──────────────┘
```

Memorize the three "WRITER" states — the distinction *cache modified* vs. *db file
modified* is exactly what decides whether a crash needs journal playback.

---

## 2.3 Rollback journal: the classic algorithm

### The idea
Before overwriting a page, save its **original** content in a side file. If we crash,
copy the originals back.

```
  BEFORE:  db page 7 = "OLD"                  journal: (empty)
  WRITE :  journal ← page7="OLD" ; fsync(journal)
           db page 7 = "NEW"     ; fsync(db)
  COMMIT:  delete journal            ← THE COMMIT POINT
```

A crash before the journal delete ⇒ next opener sees a *hot journal* and restores "OLD".
A crash after ⇒ the new data is already durable. There is no in-between, because a file
delete (or header zeroing) is atomic on the filesystem.

### Journal file format

```
 ┌───────────────────────────────────────────────────────────────┐
 │ JOURNAL HEADER (padded to sector size)                        │
 │   8 bytes magic:  d9 d5 05 f9 20 a1 63 d7                     │
 │   4 bytes nRec     : # of page records (0xffffffff = "unknown")│
 │   4 bytes cksumInit: random nonce seeding record checksums     │
 │   4 bytes dbSize   : db size in pages BEFORE this transaction  │
 │   4 bytes sectorSz : assumed sector size                       │
 │   4 bytes pageSz   : page size                                 │
 ├───────────────────────────────────────────────────────────────┤
 │ RECORD: 4-byte page number | pageSz bytes of ORIGINAL data |   │
 │         4-byte checksum                                       │
 │ RECORD: ...                                                   │
 ├───────────────────────────────────────────────────────────────┤
 │ (optionally another JOURNAL HEADER + records — multi-segment   │
 │  journals happen when the cache spills mid-transaction)        │
 └───────────────────────────────────────────────────────────────┘
```

Why the nonce + checksum? Because after a crash the journal tail may be garbage that
*happens* to look like a valid record. `cksumInit` is random per transaction, so stale
records from an older journal cannot be mistaken for current ones.

Why `dbSize`? So rollback can truncate the database back to its pre-transaction length —
otherwise a rolled-back `INSERT` that grew the file would leave junk pages.

### The full atomic commit dance (rollback mode)

```
 1. SHARED lock                       ; may read
 2. read pages into cache
 3. RESERVED lock                     ; "I intend to write" (readers may continue)
 4. create journal file, write header
 5. modify pages in the CACHE only
 6. write original pages to journal; fsync(journal)     ◄── must happen before 8
 7. PENDING → EXCLUSIVE lock          ; block new readers, wait for existing ones
 8. write modified pages to the db file
 9. fsync(db)                                            ◄── durability of new data
10. delete (or truncate/zero) the journal                ◄── THE COMMIT POINT
11. release locks → back to SHARED/UNLOCKED
```

Every one of steps 6, 9, 10 is a barrier. Removing any of them breaks durability in a
specific, demonstrable way — Lab 2.6 makes you break each one and observe the result.

### Journal modes

| Mode | Commit does | Notes |
|---|---|---|
| `DELETE` (default) | unlink the journal | commit point = directory entry removal (dir must be synced) |
| `TRUNCATE` | truncate journal to 0 bytes | avoids directory sync; faster on some filesystems |
| `PERSIST` | zero the journal header | avoids file create/delete churn entirely |
| `MEMORY` | journal lives in RAM | **not crash-safe**; process kill = corruption |
| `OFF` | no journal at all | no rollback, no atomicity; `ROLLBACK` is undefined |
| `WAL` | write-ahead log | different algorithm entirely — Phase 7 |

### `PRAGMA synchronous`

| Level | fsync behavior (rollback mode) | risk |
|---|---|---|
| `OFF` (0) | never syncs | power loss can corrupt |
| `NORMAL` (1) | syncs journal, not always the db at commit | small risk on some filesystems |
| `FULL` (2, default) | syncs journal and db | safe |
| `EXTRA` (3) | also syncs the directory after journal delete | safest |

In **WAL** mode the meanings differ (NORMAL is safe against crashes, only risky for power
loss of the *last* transactions) — see Phase 7.

---

## 2.4 Locking: five states on one byte range

SQLite implements locking with POSIX advisory byte-range locks on a region of the database
file that contains **no data**:

```
  PENDING_BYTE   = 0x40000000 (1 GiB)
  RESERVED_BYTE  = PENDING_BYTE + 1
  SHARED_FIRST   = PENDING_BYTE + 2
  SHARED_SIZE    = 510              (a 510-byte range used for read locks)
```

The page containing these bytes is the **lock-byte page** and is never used for data —
that is the hole in page numbering you met in Phase 1.

```
   state        how it is taken                      meaning
 ─────────────────────────────────────────────────────────────────────────────
   UNLOCKED     (no locks)                            nothing
   SHARED       read lock on a byte in SHARED range   may read; many allowed
   RESERVED     write lock on RESERVED_BYTE           intend to write; one only;
                                                      readers still allowed
   PENDING      write lock on PENDING_BYTE            no NEW readers may start;
                                                      waiting for readers to drain
   EXCLUSIVE    write lock on whole SHARED range      may write the db file; alone
```

State transitions:

```
   UNLOCKED ──► SHARED ──► RESERVED ──► PENDING ──► EXCLUSIVE
       ▲           │            │           │            │
       └───────────┴────────────┴───────────┴────────────┘  (unlock)
```

Why PENDING exists: without it, a writer that keeps yielding to a stream of arriving
readers would starve forever. PENDING blocks *new* readers while existing ones finish.

`SQLITE_BUSY` is returned when a lock can't be taken. `sqlite3_busy_timeout()` installs a
handler that sleeps and retries. Deadlock is possible when two connections both hold
SHARED and both want RESERVED→EXCLUSIVE — SQLite returns `SQLITE_BUSY` rather than
deadlock-detecting.

---

## 2.5 Hot journal detection and recovery

When a connection opens a database and takes a SHARED lock, `hasHotJournal()` asks:

```
 1. Does <db>-journal exist?
 2. Is there NO RESERVED lock on the db right now?      (⇒ no live writer owns it)
 3. Does the journal have a valid header (magic + nRec)?
 4. Is the db file non-empty?
 ──► if all yes: this journal is HOT. Recover it.
```

Recovery (`pager_playback()`):

```
 a. take EXCLUSIVE lock
 b. read journal header: nRec, cksumInit, dbSize, pageSize
 c. for each record: verify checksum, write original page back into the db
 d. truncate db to dbSize pages
 e. fsync(db), delete journal
 f. downgrade to SHARED, continue as normal
```

Subtleties that make this correct:
- If `nRec == 0xffffffff` (the "unknown" marker used while a transaction is in flight),
  the number of records is computed from the journal file size.
- Checksum failures **truncate** the playback at that point — a partially written record is
  treated as "not part of the transaction", which is correct because the commit point was
  never reached.
- A journal whose `nRec == 0` and header zeroed is *not* hot (that's how PERSIST mode
  marks commit).

---

## 2.6 The page cache

```
   sqlite3PagerGet(pgno)
        │
        ├─► sqlite3PcacheFetch()   ── hit ──► return PgHdr*   (refcount++)
        │
        └─ miss ─► allocate PgHdr + page buffer
                   ├─ if cache full → recycle from LRU list
                   │     if the victim is DIRTY → must journal+write it first ("spill")
                   └─ read from disk (or WAL) via sqlite3OsRead()
```

Key structures:

```c
typedef struct PgHdr {
  sqlite3_pcache_page *pPage;
  void *pData;          /* page content */
  void *pExtra;         /* MemPage lives here — b-tree's per-page state */
  PCache *pCache;
  PgHdr *pDirtyNext, *pDirtyPrev;   /* dirty list */
  Pager *pPager;
  Pgno pgno;
  u16 flags;            /* PGHDR_DIRTY, PGHDR_NEED_SYNC, PGHDR_WRITEABLE, ... */
  i16 nRef;             /* pin count */
} PgHdr;
```

- `pExtra` is a neat memory trick: the b-tree's `MemPage` is allocated *inside* the cache
  entry, so a page and its parsed header are one allocation.
- **Cache spill**: if the cache fills mid-transaction, dirty pages must go to the db file
  *before* commit. That forces an early EXCLUSIVE lock and moves you to `WRITER_DBMOD`.
  `PRAGMA cache_spill` controls it. This is why a huge transaction can hold an EXCLUSIVE
  lock much longer than you expect.
- `PRAGMA cache_size = -N` means N kibibytes; positive N means N pages.

`pcache1.c` implements the default cache: per-connection hash table + global LRU, optional
memory-pool (`SQLITE_CONFIG_PAGECACHE`). You can replace it entirely via
`sqlite3_config(SQLITE_CONFIG_PCACHE2, ...)`.

---

## 2.7 The VFS: SQLite's only door to the OS

```c
struct sqlite3_vfs {
  int iVersion;
  int szOsFile;                /* size of the subclassed sqlite3_file */
  int mxPathname;
  sqlite3_vfs *pNext;
  const char *zName;           /* "unix", "unix-excl", "win32", "memdb", ... */
  void *pAppData;
  int (*xOpen)(sqlite3_vfs*, const char *zName, sqlite3_file*, int flags, int *pOutFlags);
  int (*xDelete)(sqlite3_vfs*, const char *zName, int syncDir);
  int (*xAccess)(sqlite3_vfs*, const char *zName, int flags, int *pResOut);
  int (*xFullPathname)(sqlite3_vfs*, const char *zName, int nOut, char *zOut);
  void *(*xDlOpen)(sqlite3_vfs*, const char *zFilename);
  ...
  int (*xRandomness)(sqlite3_vfs*, int nByte, char *zOut);
  int (*xSleep)(sqlite3_vfs*, int microseconds);
  int (*xCurrentTime)(sqlite3_vfs*, double*);
  int (*xGetLastError)(sqlite3_vfs*, int, char *);
  int (*xCurrentTimeInt64)(sqlite3_vfs*, sqlite3_int64*);
  /* version 3 adds xSetSystemCall/xGetSystemCall/xNextSystemCall */
};

struct sqlite3_io_methods {
  int iVersion;
  int (*xClose)(sqlite3_file*);
  int (*xRead)(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
  int (*xWrite)(sqlite3_file*, const void*, int iAmt, sqlite3_int64 iOfst);
  int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
  int (*xSync)(sqlite3_file*, int flags);
  int (*xFileSize)(sqlite3_file*, sqlite3_int64 *pSize);
  int (*xLock)(sqlite3_file*, int);
  int (*xUnlock)(sqlite3_file*, int);
  int (*xCheckReservedLock)(sqlite3_file*, int *pResOut);
  int (*xFileControl)(sqlite3_file*, int op, void *pArg);
  int (*xSectorSize)(sqlite3_file*);
  int (*xDeviceCharacteristics)(sqlite3_file*);
  /* v2: shared memory for WAL */
  int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
  int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
  void (*xShmBarrier)(sqlite3_file*);
  int (*xShmUnmap)(sqlite3_file*, int deleteFlag);
  /* v3: memory-mapped I/O */
  int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
  int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void *p);
};
```

**Device characteristics** matter more than they look:

| Flag | Meaning |
|---|---|
| `SQLITE_IOCAP_ATOMIC512 … ATOMIC64K` | a write of that size is atomic ⇒ journal can sometimes be skipped |
| `SQLITE_IOCAP_SAFE_APPEND` | appended data lands before the file size grows ⇒ fewer syncs |
| `SQLITE_IOCAP_SEQUENTIAL` | writes complete in order ⇒ fewer syncs |
| `SQLITE_IOCAP_POWERSAFE_OVERWRITE` | writing a sector does not damage neighbouring bytes |

`xSectorSize` drives journal header padding: SQLite must never write a journal record that
shares a sector with a *later* record, or a torn sector could corrupt both.

### The POSIX locking disaster (and unixInodeInfo)

POSIX advisory locks have a notorious flaw: **closing *any* file descriptor for a file
releases *all* of that process's locks on it.** SQLite works around it with a global,
mutex-protected table of `unixInodeInfo` structs keyed by `(device, inode)`:

```
   open("x.db") twice in one process
        │
        ▼
   unixInodeInfo{ dev, ino, nRef, eFileLock, nShared, pUnused[] }
        │              ▲
        └─ both unixFile structs point here;
           closing one only queues the fd in pUnused for later real close
```

This is also why SQLite tracks locks *within* the process itself rather than trusting the
kernel for intra-process contention. Alternate unix VFSes exist for hostile environments:
`unix-dotfile` (lock = a lock file, for NFS), `unix-flock`, `unix-none`, `unix-excl`
(takes an EXCLUSIVE lock once and keeps it — enables shared-memory WAL without an
`-shm` file).

**Never put a SQLite database on a network filesystem** unless you use `unix-dotfile`: NFS
advisory locking is unreliable, and silent corruption follows.

---

## 2.8 Super-journals (multi-database transactions)

`ATTACH` + a transaction touching two files needs a two-phase commit:

```
 1. each database writes its own rollback journal, which now ALSO records
    the name of a shared "super-journal" (a.k.a. master journal) file
 2. the super-journal file is created listing every participating journal; fsync
 3. each db file is written and synced
 4. DELETE THE SUPER-JOURNAL  ◄── the single global commit point
 5. delete the individual journals
```

Recovery rule: a journal that names a super-journal is hot **only if** that super-journal
still exists and still lists this database. That one rule makes multi-file commits atomic.

---

## 2.9 Statement journals and savepoints

A single statement that fails halfway (e.g. an `UPDATE` violating a constraint on row 500)
must undo its own partial work without aborting the enclosing transaction. That is the
**statement journal** — a sub-journal opened on demand (often in memory,
`memjournal.c`) recording pages changed by the current statement.

`SAVEPOINT` generalizes this: each savepoint records a point in the (sub)journal to which
it can rewind. `PagerSavepoint` holds the journal offset, the WAL frame count, and a
`Bitvec` of pages written since the savepoint.

`bitvec.c` deserves a look: a sparse bitmap of page numbers built as a hybrid of a bit
array and a hash tree, designed to use O(1) memory for the common case of few pages.

---

## 2.10 Memory-mapped I/O

`PRAGMA mmap_size=N` maps the first N bytes of the db file; `xFetch`/`xUnfetch` then hand
the pager pointers straight into the mapping, skipping a memcpy. Reads get faster; the
cost is that an I/O error becomes a `SIGBUS` rather than an error code, and writes still go
through `xWrite`. Off by default on most platforms.

---

## 2.11 What to remember

- The commit point is a **single atomic filesystem operation** (journal delete / header
  zero / WAL commit-frame write). Everything before it is undoable; everything after it is
  durable.
- The pager is a state machine; every I/O error must move it to a state from which the
  next operation is still safe.
- Locking is coarse: one writer, many readers, no row locks, no MVCC in rollback mode.
- The VFS is the seam you can replace — encryption, compression, in-memory, network, test
  fault-injection all live there.
