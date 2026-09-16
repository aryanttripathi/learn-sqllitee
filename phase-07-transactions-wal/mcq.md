# Phase 7 — MCQ (25 questions)

1. In WAL mode, a commit is marked by:
   a) deleting the WAL  b) a frame whose "database size" field is non-zero
   c) an fsync  d) a lock release

2. The WAL frame header is:
   a) 8 bytes  b) 24 bytes  c) 32 bytes  d) page-size dependent

3. The WAL file header is:
   a) 24 bytes  b) 32 bytes  c) 100 bytes  d) 512 bytes

4. The WAL magic's low bit selects:
   a) the page size  b) the byte order used for checksum word reads
   c) the format version  d) compression

5. WAL checksums are:
   a) CRC32  b) a chained Fibonacci-weighted 32-bit sum  c) SHA-1  d) none

6. Salt values in the WAL exist to:
   a) encrypt  b) ensure frames from an earlier WAL generation cannot be mistaken for current ones
   c) seed the hash  d) identify the process

7. The `-shm` file contains:
   a) the WAL data  b) the wal-index: headers, checkpoint info, and page→frame hash tables
   c) a copy of the database  d) lock files

8. Deleting the `-shm` file while no connection is open:
   a) corrupts the database  b) is harmless; it is rebuilt by recovery
   c) loses committed data  d) is impossible

9. Deleting the `-wal` file of a live database:
   a) is harmless  b) loses committed transactions  c) forces a checkpoint  d) is a no-op

10. How many reader "read marks" does the wal-index have?
    a) 1  b) 3  c) 5  d) unlimited

11. `aReadMark[0]` is special because:
    a) it is the writer's  b) it is always 0 — readers using only the database file
    c) it is the checkpointer's  d) it stores mxFrame

12. A checkpoint cannot backfill past:
    a) mxFrame  b) the minimum in-use read mark  c) nBackfill  d) the page size

13. WAL growth without bound is usually caused by:
    a) too many writers  b) a long-running read transaction pinning a read mark
    c) small page size  d) synchronous=FULL

14. `PRAGMA wal_checkpoint(TRUNCATE)`:
    a) deletes the database  b) checkpoints, restarts, and truncates the WAL to zero
    c) truncates the database  d) only works in rollback mode

15. In WAL mode with `synchronous=NORMAL`, a power failure can:
    a) corrupt the database  b) lose recent transactions but not corrupt
    c) do nothing  d) lose the whole database

16. In WAL mode, readers and one writer:
    a) block each other  b) run concurrently; readers see a snapshot
    c) require shared cache  d) require threads

17. Multiple simultaneous writers in WAL mode are:
    a) allowed  b) not allowed — `WAL_WRITE_LOCK` serializes them
    c) allowed with BEGIN CONCURRENT in mainline SQLite  d) allowed per table

18. `SQLITE_BUSY_SNAPSHOT` means:
    a) the disk is full  b) your read snapshot is too old to upgrade to a write; restart the transaction
    c) the WAL is full  d) a checkpoint is running

19. WAL recovery:
    a) rewrites the database file  b) only rebuilds the wal-index by verifying the checksum chain
    c) deletes the WAL  d) requires an exclusive database lock forever

20. `BEGIN IMMEDIATE` is recommended for write transactions because:
    a) it is faster  b) it takes the write lock up front, so `SQLITE_BUSY` is retryable
    c) it disables the journal  d) it avoids checkpointing

21. `ON CONFLICT FAIL` differs from `ABORT` because it:
    a) rolls back the transaction  b) leaves the statement's prior changes in place
    c) ignores the row  d) replaces the row

22. A statement journal is opened when:
    a) always  b) a statement might need to be undone partway without aborting the transaction
    c) in WAL mode only  d) for SELECTs

23. `PRAGMA foreign_keys`:
    a) is ON by default  b) is OFF by default and cannot be changed inside a transaction
    c) applies only to new tables  d) requires WAL

24. `PRAGMA locking_mode=EXCLUSIVE` in WAL mode:
    a) blocks readers  b) allows the wal-index to live in heap memory — no `-shm` file
    c) is ignored  d) disables checkpointing

25. The safe ways to copy a live database are:
    a) `cp db db.bak`  b) `VACUUM INTO`, `.backup`, or the `sqlite3_backup` API
    c) copying db and -wal separately  d) `tar` the directory

---

## Answers

1. **b** — verified in real bytes: the frame header's second field.
2. **b**.
3. **b**.
4. **b** — and it is the *opposite* convention from the database file, which is always big-endian.
5. **b** — cheap, chained, and sufficient to detect torn/reordered writes.
6. **b**.
7. **b**.
8. **b**.
9. **b** — the WAL holds committed data not yet checkpointed.
10. **c** — five slots, shm lock slots 3..7.
11. **b**.
12. **b** — otherwise a reader's snapshot would be destroyed.
13. **b**.
14. **b**.
15. **b** — the key WAL safety property; the same setting in rollback mode *can* corrupt.
16. **b**.
17. **b** — `BEGIN CONCURRENT` exists only on a branch, not in mainline releases.
18. **b** — and the busy handler is not called for it.
19. **b** — recovery never modifies the database file.
20. **b**.
21. **b** — the only conflict mode that leaves a statement half-applied.
22. **b**.
23. **b**.
24. **b**.
25. **b**.
