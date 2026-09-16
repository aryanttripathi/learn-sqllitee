# Phase 7 — Implementation Labs: WAL, Concurrency, Recovery

A working, verified WAL decoder/verifier is checked in at **`labs/phase07/walparse.c`**
(builds clean, validates real checksums, detects corruption).

---

## Lab 7.1 — ⭐ Decode and verify a WAL with your own code

```sh
cd ~/Desktop/sqllite/labs/phase07
cc -Wall -O0 -g -o walparse walparse.c

rm -f w7.db w7.db-wal w7.db-shm
sqlite3 w7.db <<SQL
PRAGMA journal_mode=WAL;
PRAGMA wal_autocheckpoint=0;     -- keep the WAL around so we can look at it
CREATE TABLE t(a,b);
INSERT INTO t VALUES(1,'one');
INSERT INTO t VALUES(2,'two');
.shell \$PWD/walparse \$PWD/w7.db-wal
SQL
```

> **Important:** you must inspect the WAL *inside* a live session. When the last connection
> closes cleanly, SQLite checkpoints and removes/zeroes the WAL — which is why a naive
> `sqlite3 ...; xxd db-wal` always shows an empty file.

### Verified output

```
WAL header
  magic          : 0x377f0682  (little-endian checksums)
  format version : 3007000 (ok)
  page size      : 4096
  ckpt sequence  : 0
  salt-1 / salt-2: 0x3fb8f141 / 0x39fecbbd
  header cksum   : 0x16d8bb52 / 0x8c8c9130  -> computed 0x16d8bb52 / 0x8c8c9130  VALID

frame  offset     pgno   dbsize  salt1/salt2 ok  cksum        status
    1         32       1       0  ok              0xd72e5f77   valid
    2       4152       2       2  ok              0x001a118d   valid   <<< COMMIT
    3       8272       2       2  ok              0x22671eeb   valid   <<< COMMIT
    4      12392       2       2  ok              0x0df251ec   valid   <<< COMMIT

4 frame(s); WAL file is 16512 bytes (= 32 + n*(24+4096))
```

Read it: the `CREATE TABLE` wrote two frames (page 1 = schema, page 2 = the new table's
root) and the second is the commit frame. Each `INSERT` then wrote one frame for page 2.

### The endianness trap (worth an hour if you get it wrong)

The magic number's **low bit** selects the byte order used for reading 32-bit words while
checksumming:

```c
pWal->hdr.bigEndCksum = (u8)(magic & 0x00000001);
walChecksumBytes(pWal->hdr.bigEndCksum==SQLITE_BIGENDIAN, ...);  /* native or byteswapped */
```

So `0x377f0682` (LSB 0) ⇒ little-endian words; `0x377f0683` (LSB 1) ⇒ big-endian. Note this
is the *opposite* of the database file, which is always big-endian. Verify empirically —
your parser either reproduces SQLite's checksum or it doesn't.

### The checksum, exactly

```c
/* Fibonacci-weighted running sum; nByte must be a multiple of 8.
 * Frame checksum = chain(previous frame cksum, frame-header[0..7] then page data). */
for(i=0; i<nByte; i+=8){
  s1 += word(a+i)   + s2;
  s2 += word(a+i+4) + s1;
}
```
The chain starts at the WAL header's own checksum. That chaining is what makes a partial
write unrecoverable *by design*: everything after a bad frame is ignored.

---

## Lab 7.2 — Corrupt a WAL and watch recovery stop

```sh
cd ~/Desktop/sqllite/labs/phase07
# inside a live session so the WAL exists:
sqlite3 w7.db <<SQL
PRAGMA journal_mode=WAL;
PRAGMA wal_autocheckpoint=0;
CREATE TABLE t2(a); INSERT INTO t2 VALUES(1); INSERT INTO t2 VALUES(2);
.shell /bin/cat \$PWD/w7.db-wal > \$PWD/broken.wal
.shell printf '\xff' | dd of=\$PWD/broken.wal bs=1 seek=8300 count=1 conv=notrunc 2>/dev/null
.shell \$PWD/walparse \$PWD/broken.wal
SQL
```

Verified output tail:
```
    1         32       1       0  ok              0x9a9024cc   valid
    2       4152       2       2  ok              0x612dd996   valid   <<< COMMIT
    3       8272       2       2  ok              0x910873b8   INVALID   <<< COMMIT

Recovery would STOP here: frames 3..end are ignored.
```

**Deliverable `labs/phase07/wal-corruption.md`:** repeat with corruptions in
1. the WAL header magic
2. the WAL header page-size field
3. a frame's salt
4. a frame's page number
5. the last frame's data (a torn final write — the realistic case)

For each, state what SQLite would do on recovery, then actually verify: kill the session
with the corrupted WAL in place, reopen, and compare row counts and
`PRAGMA integrity_check`.

---

## Lab 7.3 — Readers and writers really do run concurrently

Terminal 1:
```sh
cd ~/Desktop/sqllite/labs/phase07
sqlite3 conc.db "PRAGMA journal_mode=WAL; CREATE TABLE t(a INTEGER PRIMARY KEY,b);"
sqlite3 conc.db
sqlite> BEGIN;                 -- a long read transaction
sqlite> SELECT count(*) FROM t;
```

Terminal 2 (while terminal 1 sits there):
```sh
sqlite3 conc.db "INSERT INTO t VALUES(1,'x');"     # succeeds — WAL!
sqlite3 conc.db "SELECT count(*) FROM t;"          # sees the new row
```

Back in terminal 1:
```sql
sqlite> SELECT count(*) FROM t;    -- STILL the old count: snapshot isolation
sqlite> COMMIT;
sqlite> SELECT count(*) FROM t;    -- now sees it
```

Repeat the whole experiment with `journal_mode=DELETE` and record the differences.

Then demonstrate that writers still serialize:
```sh
# terminal 1
sqlite3 conc.db
sqlite> BEGIN IMMEDIATE; INSERT INTO t VALUES(99,'a');
# terminal 2
sqlite3 conc.db "BEGIN IMMEDIATE; INSERT INTO t VALUES(98,'b');"   # SQLITE_BUSY
```

`labs/phase07/concurrency.md`: a matrix of (mode × reader/writer combination) → blocked or
not, with your reproductions.

---

## Lab 7.4 — Trace WAL I/O with the Phase 2 VFS shim

```sh
cd ~/Desktop/sqllite/labs/phase02
./tracevfs wal | head -60
```

Verified excerpt:
```
OPEN    trace.db-wal
SHMMAP  trace.db     pg=0 sz=32768 extend=0     ← the wal-index
SHMLOCK trace.db     ofst=0 n=1 flags=0xa       ← WAL_WRITE_LOCK
SHMLOCK trace.db     ofst=1 n=2 flags=0xa       ← CKPT + RECOVER
SHMLOCK trace.db     ofst=4..7 n=1              ← the five READ-MARK slots
WRITE   trace.db-wal off=0  amt=32              ← the 32-byte WAL header
SYNC    trace.db-wal flags=0x3
```

Map every `SHMLOCK ofst=N` to its name (0 write, 1 ckpt, 2 recover, 3..7 read marks) and
every `flags=0x..` to `SQLITE_SHM_LOCK/UNLOCK/SHARED/EXCLUSIVE`. Write the annotated trace
into `labs/phase07/wal-trace.md`, and compare the number of writes and syncs per commit
against rollback mode from Phase 2.

---

## Lab 7.5 — Checkpoint behaviour and WAL growth

```sh
cd ~/Desktop/sqllite/labs/phase07
rm -f ck.db ck.db-wal ck.db-shm
sqlite3 ck.db "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;
               CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB);"

# grow the WAL
sqlite3 ck.db <<'SQL'
PRAGMA wal_autocheckpoint=0;
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<50000)
INSERT INTO t SELECT i, randomblob(100) FROM c;
COMMIT;
.shell ls -l ck.db ck.db-wal
PRAGMA wal_checkpoint(PASSIVE);
.shell ls -l ck.db ck.db-wal
PRAGMA wal_checkpoint(TRUNCATE);
.shell ls -l ck.db ck.db-wal
SQL
```

Now reproduce the **WAL-growth pathology**:

```sh
# terminal 1: hold a read transaction open
sqlite3 ck.db
sqlite> PRAGMA wal_autocheckpoint=0;
sqlite> BEGIN; SELECT count(*) FROM t;

# terminal 2: write a lot, then try to checkpoint
sqlite3 ck.db "PRAGMA wal_autocheckpoint=0;
  BEGIN; WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<50000)
  INSERT INTO t SELECT i+1000000, randomblob(100) FROM c; COMMIT;
  PRAGMA wal_checkpoint(PASSIVE);"      # note the busy flag / low nCheckpointed
ls -l ck.db-wal                          # huge, and it will not shrink
```

`PRAGMA wal_checkpoint` returns `(busy, nLog, nCheckpointed)`. Record the numbers before
and after releasing the reader in `labs/phase07/checkpoint.md`, and answer:
1. Why can't the checkpointer backfill past the oldest read mark?
2. What does `RESTART` add over `FULL`, and when do you need it?
3. What is a reasonable production checkpoint policy, and how would you implement it with
   `sqlite3_wal_hook()`?

---

## Lab 7.6 — Measure the WAL vs rollback trade-off

```sh
cd ~/Desktop/sqllite/labs/phase07
bench() {
  MODE=$1; SYNC=$2; N=$3
  rm -f bench.db bench.db-wal bench.db-shm bench.db-journal
  sqlite3 bench.db "PRAGMA journal_mode=$MODE; PRAGMA synchronous=$SYNC;
                    CREATE TABLE t(a INTEGER PRIMARY KEY,b);" >/dev/null
  START=$(python3 -c 'import time;print(time.time())')
  for i in $(seq 1 $N); do
    sqlite3 bench.db "PRAGMA journal_mode=$MODE; PRAGMA synchronous=$SYNC;
                      INSERT INTO t VALUES($i,'x');" >/dev/null
  done
  END=$(python3 -c 'import time;print(time.time())')
  python3 -c "print(f'$MODE/$SYNC: {$END-$START:.2f}s for $N txns')"
}
bench delete full   200
bench delete normal 200
bench wal    full   200
bench wal    normal 200
bench wal    off    200
```

Then the same test with all inserts inside **one** transaction. Put both tables in
`labs/phase07/wal-bench.md`, plus a paragraph on when WAL is *not* the right choice.

---

## Lab 7.7 — Savepoints and partial rollback

```sh
sqlite3 :memory: <<'SQL'
CREATE TABLE t(a INTEGER PRIMARY KEY, b);
BEGIN;
  INSERT INTO t VALUES(1,'one');
  SAVEPOINT s1;
    INSERT INTO t VALUES(2,'two');
    SAVEPOINT s2;
      INSERT INTO t VALUES(3,'three');
    ROLLBACK TO s2;
    SELECT 'after rollback to s2:', group_concat(a) FROM t;
  RELEASE s1;
  SELECT 'after release s1:', group_concat(a) FROM t;
ROLLBACK;
SELECT 'after outer rollback:', count(*) FROM t;
SQL
```

Then explore conflict resolution — construct a case that proves `OR FAIL` leaves a
statement half-applied while `OR ABORT` does not:

```sh
sqlite3 :memory: <<'SQL'
CREATE TABLE t(a INTEGER PRIMARY KEY, b UNIQUE);
INSERT INTO t VALUES(1,'x'),(2,'y'),(3,'z');
CREATE TABLE u(a,b);
INSERT INTO u VALUES(10,'p'),(11,'y'),(12,'q');   -- 'y' will collide
BEGIN;
  INSERT OR FAIL INTO t SELECT a,b FROM u ORDER BY a;
SELECT 'OR FAIL left behind:', count(*) FROM t WHERE a>=10;
ROLLBACK;
BEGIN;
  INSERT OR ABORT INTO t SELECT a,b FROM u ORDER BY a;
SELECT 'OR ABORT left behind:', count(*) FROM t WHERE a>=10;
ROLLBACK;
SQL
```

`labs/phase07/savepoints.md`: results, plus where the statement journal appears in a
`tracevfs` run of the `OR ABORT` case.

---

## Lab 7.8 — Backups done right (and wrong)

```sh
cd ~/Desktop/sqllite/labs/phase07
# WRONG: copying the db while a writer is active
sqlite3 ck.db "PRAGMA wal_autocheckpoint=0;" &
/bin/cp ck.db bad-backup.db            # missing the WAL entirely!
sqlite3 bad-backup.db "SELECT count(*) FROM t;"     # stale or broken

# RIGHT (three correct methods)
sqlite3 ck.db "VACUUM INTO 'good1.db';"
sqlite3 ck.db ".backup good2.db"
# or the C API: sqlite3_backup_init/step/finish
```

Write `labs/phase07/backup.md`: what each method does about the WAL, which ones are safe
with concurrent writers, and the size/time trade-offs. Include a demonstration that
`VACUUM INTO` also defragments (compare page counts and fill factor with Phase 3's tools).

---

## Lab 7.9 — Read `wal.c`

Answer with `file:line` citations in `labs/phase07/source-questions.md`:

1. In `walTryBeginRead()`, what is the retry loop protecting against, and why is the
   `WalIndexHdr` read twice?
2. Where is `aReadMark[0]` special-cased, and what does a reader using it see?
3. In `walCheckpoint()`, how is `mxSafeFrame` computed and what stops it from advancing?
4. Find where the WAL is reset (salts changed) — under exactly what conditions?
5. What does `walHash()` / `walIndexAppend()` do, and how big is one index block?
6. Find the code path that handles a `-shm` file that exists but is stale. How does SQLite
   decide to run recovery?
7. What is `WAL_HEAPMEMORY_MODE` and when is the wal-index kept in heap memory instead of
   a shared file? (Hint: `locking_mode=EXCLUSIVE`, `unix-excl`.)
8. How does `sqlite3WalSnapshotOpen()` differ from a normal read transaction?

---

## Lab 7.10 — Crash-recovery matrix

Extend the Phase 2 crash harness to cover WAL:

```sh
./crash_loop.sh wal full     30
./crash_loop.sh wal normal   30
./crash_loop.sh wal off      30
./crash_loop.sh delete full  30
./crash_loop.sh memory full  30
```

For each run record: integrity_check result, rows surviving, and whether the `-wal`/`-shm`
files were left behind. Then re-open each surviving database with your `walparse` tool
*before* letting SQLite touch it, and predict what recovery will keep — then verify.

That prediction-then-verification loop is exactly how SQLite's own crash tests work, and
it is the skill that lets you debug a real corruption report.
