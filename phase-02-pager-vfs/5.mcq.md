# Phase 2 — MCQ (25 questions)

1. The rollback journal stores:
   a) the new page content  b) the original page content
   c) the SQL statements  d) a redo log

2. In rollback mode the commit point is:
   a) the fsync of the database file  b) deleting/truncating/invalidating the journal
   c) taking the EXCLUSIVE lock  d) releasing the SHARED lock

3. The journal header's `nRec` is written as 0xffffffff while a transaction is in flight because:
   a) it is a checksum  b) the final count is unknown until the transaction ends
   c) it marks WAL mode  d) it is a version number

4. `cksumInit` is random per transaction in order to:
   a) encrypt the journal  b) prevent stale records from a previous journal from validating
   c) speed up checksums  d) seed the page allocator

5. `dbSize` in the journal header is used to:
   a) preallocate the journal  b) truncate the database back on rollback
   c) compute the checksum  d) choose the page size

6. The five lock states in order are:
   a) NONE, SHARED, RESERVED, PENDING, EXCLUSIVE
   b) NONE, READ, WRITE, COMMIT, DONE
   c) NONE, SHARED, EXCLUSIVE, PENDING, RESERVED
   d) OPEN, READER, WRITER, DBMOD, FINISHED

7. The PENDING lock exists to:
   a) allow more readers  b) stop *new* readers so a waiting writer can make progress
   c) implement savepoints  d) support WAL

8. Byte-range locks are taken on:
   a) the whole file  b) a region at offset 0x40000000 that holds no data
   c) page 1  d) the journal file

9. Multiple readers and one writer can coexist in rollback mode when the writer holds:
   a) EXCLUSIVE  b) PENDING  c) RESERVED  d) never

10. A journal is "hot" when:
    a) it exists, no process holds RESERVED, and its header is valid
    b) it is larger than the database  c) it is in WAL mode  d) its checksum fails

11. Who performs crash recovery?
    a) a background daemon  b) the next process that opens the database
    c) `VACUUM`  d) the OS

12. A checksum failure during journal playback causes SQLite to:
    a) abort with corruption  b) stop playback at that record (the rest was never committed)
    c) skip that page and continue  d) delete the database

13. `PRAGMA journal_mode=MEMORY` is unsafe because:
    a) it uses more RAM  b) a process crash leaves the db file partially written with no way back
    c) it disables locking  d) it disables the page cache

14. `PRAGMA synchronous=OFF` in rollback mode risks:
    a) slower commits  b) database corruption on power loss
    c) only losing the last transaction  d) nothing

15. Cache spill means:
    a) reading past EOF  b) writing dirty pages to the db file before commit because the cache is full
    c) evicting clean pages  d) a memory leak

16. `PGHDR_NEED_SYNC` means:
    a) the page is dirty  b) the page's journal record is not yet synced, so the page must not be written to the db
    c) the page is memory-mapped  d) the page is in the WAL

17. In a VFS shim, `sqlite3_file base` must be the first struct member because:
    a) alignment  b) SQLite casts your struct pointer to `sqlite3_file*`
    c) it is smaller  d) C requires it

18. `szOsFile` must be set to:
    a) the page size  b) `sizeof(your struct) + wrapped VFS's szOsFile`
    c) 0  d) `sizeof(sqlite3_file)`

19. `xSectorSize` influences:
    a) the page size  b) journal header padding and write batching
    c) the cache size  d) lock granularity

20. `SQLITE_IOCAP_SAFE_APPEND` allows SQLite to:
    a) skip a journal sync because appended data lands before the size grows
    b) skip locking  c) use mmap  d) skip checksums

21. Closing a second file descriptor on the same file under POSIX:
    a) is harmless  b) drops all of the process's advisory locks on that file
    c) flushes the cache  d) is forbidden

22. `unixInodeInfo` is keyed by:
    a) filename  b) (device, inode)  c) fd number  d) pathname hash

23. A super-journal (master journal) is needed when:
    a) a transaction spans multiple attached databases  b) the db is over 1 GiB
    c) WAL is enabled  d) savepoints are used

24. The statement journal exists to:
    a) log SQL text  b) undo a single statement's partial work without aborting the transaction
    c) replicate changes  d) speed up prepares

25. `PRAGMA locking_mode=EXCLUSIVE` allows:
    a) more concurrency  b) keeping the EXCLUSIVE lock across transactions, and WAL without an -shm file
    c) row-level locking  d) nothing in WAL mode

---

## Answers

1. **b** — it is an *undo* log; WAL is the redo log.
2. **b** — a single atomic filesystem operation; which one depends on journal_mode.
3. **b** — records are appended first; the count is patched in afterwards, with a sync
   between, so a crash can never see a count that claims more records than exist.
4. **b** — a leftover journal from an earlier transaction would otherwise checksum-validate
   and be replayed, corrupting the database.
5. **b** — rollback must restore the original file length too.
6. **a**.
7. **b** — anti-starvation: without PENDING a stream of readers could block a writer forever.
8. **b** — the lock-byte page at 1 GiB; that is why it is excluded from page numbering.
9. **c** — RESERVED blocks other writers but not readers; the writer only escalates at
   commit (or at cache spill).
10. **a** — all four conditions in `hasHotJournal()`.
11. **b** — opportunistic recovery; there is no daemon.
12. **b** — a torn record means the commit point was never reached for that record.
13. **b**.
14. **b** — without the journal sync ordering, the db can be written before the originals
    are durable.
15. **b** — and it forces an early EXCLUSIVE lock.
16. **b**.
17. **b**.
18. **b**.
19. **b** — SQLite must never let two journal records share a sector in a way that a torn
    sector write could corrupt both.
20. **a**.
21. **b** — the notorious POSIX defect; SQLite defers closes in `pUnused`.
22. **b**.
23. **a** — two-phase commit across files; deleting the super-journal is the global commit
    point.
24. **b** — e.g. an UPDATE that hits a constraint on row 500 of 1000.
25. **b** — fewer syscalls, and the wal-index can live in heap memory.
