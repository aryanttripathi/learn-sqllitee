# Phase 3 — Implementation Labs: Build a B-Tree, Then Watch SQLite's

A working, verified miniature B+tree is checked in at **`labs/phase03/minibtree.c`**
(builds clean with `-Wall`, all self-checks pass). Read it, run it, then break it.

---

## Lab 3.1 — ⭐ Run and read `minibtree.c`

```sh
cd ~/Desktop/sqllite/labs/phase03
cc -Wall -O0 -g -o minibtree minibtree.c
./minibtree seq 20        # small tree, full dump
./minibtree seq 2000
./minibtree rand 2000
./minibtree seq 50000
./minibtree rand 50000
```

### Verified output on this machine

```
page 1 LEAF     nCell=20 free= 37  keys: 1 2 3 4 5 6 7 8 9 10 ...
mode=sequential inserted=20  distinct=20  | pages=1  depth=1 ... avg leaf fill=85.1% splits=0

mode=sequential inserted=2000  distinct=2000  | pages=228  depth=3 inner=18  leaves=210
                | avg leaf fill=47.8%  splits=225
mode=random     inserted=2000  distinct=1810  | pages=143  depth=3 inner=9   leaves=134
                | avg leaf fill=70.0%  splits=140
mode=sequential inserted=50000 distinct=50000 | pages=6653 depth=5 inner=554 leaves=6099
                | avg leaf fill=45.5%  splits=6648
mode=random     inserted=50000 distinct=44277 | pages=4037 depth=4 inner=234 leaves=3803
                | avg leaf fill=67.8%  splits=4033
checks: lookups missing=0  cells==distinct? YES  keys in order? YES
```

### ⚠ The result that teaches the most

**Sequential inserts produce a *worse* fill factor (45%) than random inserts (68%).**

That is not a bug — it is what a naive midpoint split does to an append-only workload:
every split leaves the left page permanently half-full and never revisits it, so the tree
is a chain of half-empty pages.

Real SQLite is the opposite way round (sequential ≈ 100% fill) because of
**`balance_quick`**: when the rightmost leaf of an integer-key tree overflows by exactly
one cell, it does not split — it allocates a fresh page for that one cell and leaves the
full page alone.

**Exercise 3.1a (do this):** implement `balance_quick` in `minibtree.c`.
```
in insertInto(), leaf branch, when the page is full:
   if (key > every key on the page)        /* i.e. i == nCell(n) */
       && (this page is the rightmost leaf on its path)
   then: allocate a new page, put ONLY the new cell there,
         set *pDiv = max key of the current page, return the new page
```
Re-run `./minibtree seq 50000` and report the new fill factor. You should jump from ~45%
to ~95%+ and cut the page count roughly in half. Write the before/after numbers in
`labs/phase03/balance-quick.md`.

**Exercise 3.1b:** implement 3-sibling redistribution (`balance_nonroot`-lite): before
splitting, try to move cells to the left or right sibling if either has room. Measure the
effect on random inserts.

**Exercise 3.1c:** implement `delete` with `freeSpace()`/freeblock reuse and the
"balance only when `nFree > usableSize*2/3`" rule. Then verify with the order check that
the tree stays sorted after deleting every third key.

---

## Lab 3.2 — Prove the same effect in real SQLite

```sh
cd ~/Desktop/sqllite/labs/phase03

# sequential rowids
sqlite3 seq.db <<'SQL'
PRAGMA page_size=4096;
CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<200000)
INSERT INTO t SELECT i, randomblob(60) FROM c;
COMMIT;
SQL

# random rowids, same data volume
sqlite3 rnd.db <<'SQL'
PRAGMA page_size=4096;
CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<200000)
INSERT INTO t SELECT abs(random()%1000000000), randomblob(60) FROM c;
COMMIT;
SQL

ls -l seq.db rnd.db
sqlite3 seq.db 'PRAGMA page_count;'
sqlite3 rnd.db 'PRAGMA page_count;'
```

Now measure fill with `dbstat` (needs a build with `SQLITE_ENABLE_DBSTAT_VTAB`; the
`--enable-all` debug build from Phase 0 has it):

```sh
~/Desktop/sqllite/sqlite-src/build/sqlite3 seq.db \
  "SELECT name, sum(payload)*100.0/sum(pgsize) AS fill_pct, count(*) AS pages
   FROM dbstat GROUP BY name;"
```

Or use the official analyzer:
```sh
cd ~/Desktop/sqllite/sqlite-src/build && make sqlite3_analyzer
./sqlite3_analyzer ~/Desktop/sqllite/labs/phase03/seq.db | head -40
```

Expect the sequential database to be meaningfully smaller and denser. Record in
`labs/phase03/fill-report.md`, and connect it to `balance_quick`.

**Design conclusion to write down:** a monotonically increasing primary key (rowid,
autoincrement, ULID) is a storage-engine-level optimization, not a style preference. A
random TEXT primary key in a `WITHOUT ROWID` table is the pathological case.

---

## Lab 3.3 — Watch a real page split

```sh
cd ~/Desktop/sqllite/labs/phase03
rm -f split.db
sqlite3 split.db 'PRAGMA page_size=512; CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);'
for i in $(seq 1 40); do
  sqlite3 split.db "INSERT INTO t VALUES($i, 'payload-value-$i');"
  printf "%3d rows: pages=%s  " $i "$(sqlite3 split.db 'PRAGMA page_count;')"
  ~/Desktop/sqllite/labs/phase01/dbparse split.db 2 | head -1
done
```

You will see page 2 change from `leaf table` to `interior table` at the moment of the root
split (`balance_deeper`). Capture the exact row count where it happens, and the resulting
tree shape, in `labs/phase03/split-log.md`.

Then go deeper:
```sh
# dump the whole tree after 40 rows
~/Desktop/sqllite/labs/phase01/dbparse split.db 2
```

Questions to answer:
1. After the root split, how many cells does the root have? Why so few?
2. Why did the root page number stay 2 instead of a new page becoming the root?
3. With `page_size=512`, what is the maximum number of rows before depth 3?

---

## Lab 3.4 — Breakpoint tour of `btree.c`

```sh
lldb ~/Desktop/sqllite/sqlite-src/build/sqlite3
(lldb) b sqlite3BtreeInsert
(lldb) b balance
(lldb) b balance_nonroot
(lldb) b balance_quick
(lldb) b balance_deeper
(lldb) b sqlite3BtreeDelete
(lldb) run split.db
sqlite> INSERT INTO t VALUES(1000,'x');
```

At each stop, print:
```
(lldb) p pCur->iPage
(lldb) p pCur->pPage->pgno
(lldb) p pCur->pPage->nCell
(lldb) p pCur->pPage->nFree
(lldb) p pCur->pPage->nOverflow
(lldb) p pCur->eState
(lldb) bt
```

Deliverable `labs/phase03/balance-trace.md`: a table showing, for one insert that causes a
split, the sequence of balance functions called and the page numbers involved. Include the
`aiIdx`/`apPage` cursor path before and after.

**Then read `balance_nonroot` with these anchors in view:**
`nOld`, `nNew`, `apOld[]`, `apNew[]`, `apDiv[]`, `apCell[]`, `szCell[]`, `cntNew[]`,
`szNew[]`, the "pgno ordering" loop, and the final `ptrmapPut` loop. Write a 1-page summary
of what each array holds. If you can write that page, you understand the hardest function
in SQLite.

---

## Lab 3.5 — Cursor invalidation in action

```sh
sqlite3 cur.db <<'SQL'
CREATE TABLE t(a INTEGER PRIMARY KEY, b);
INSERT INTO t SELECT value, 'x' FROM generate_series(1,1000);
-- a statement that reads and writes the same b-tree:
UPDATE t SET b = (SELECT count(*) FROM t WHERE a < t.a) WHERE a % 100 = 0;
SELECT count(*) FROM t WHERE b != 'x';
SQL
```

Set a breakpoint on `saveAllCursors` and `btreeRestoreCursorPosition`, run the UPDATE, and
count how often each fires. Explain in `labs/phase03/cursor-notes.md` why
`CURSOR_REQUIRESEEK` is required for correctness here, and what would go wrong without it.

---

## Lab 3.6 — Overflow chains and `accessPayload`

```sh
sqlite3 ovfl.db <<'SQL'
PRAGMA page_size=512;
CREATE TABLE t(id INTEGER PRIMARY KEY, blob BLOB);
INSERT INTO t VALUES(1, randomblob(10000));
SQL
~/Desktop/sqllite/labs/phase01/dbparse ovfl.db 2
sqlite3 ovfl.db 'PRAGMA page_count;'
```

Extend your Phase 1 `dbparse` to follow the whole chain and verify the byte count equals
the payload size. Then, in lldb, break on `accessPayload` and watch it walk the chain
during `SELECT length(blob) FROM t;` — note how `length()` on a blob still has to read
the overflow pages, but `sqlite3_blob_read()` can seek directly.

Bonus: time `SELECT id FROM t` vs `SELECT id, blob FROM t` on a 10k-row table of 10 KB
blobs, and explain the difference in terms of pages touched.

---

## Lab 3.7 — Integrity check as a spec

```sh
sqlite3 mini.db 'PRAGMA integrity_check;'
sqlite3 mini.db 'PRAGMA quick_check;'
```

Read `sqlite3BtreeIntegrityCheck()` in `btree.c` and list every distinct error message it
can produce. For at least **five** of them, construct a corrupted database that triggers
exactly that message (build on your Phase 1 corruption lab: edit bytes with `dd`, or use
`PRAGMA writable_schema=ON` for schema-level damage).

Deliverable: `labs/phase03/integrity-messages.md` mapping message → corruption → the
invariant that was violated.

---

## Lab 3.8 — Measure tree depth vs. page size

```sh
for ps in 512 1024 4096 16384 65536; do
  rm -f d.db
  sqlite3 d.db "PRAGMA page_size=$ps;
    CREATE TABLE t(id INTEGER PRIMARY KEY, v);
    BEGIN;
    WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<500000)
    INSERT INTO t SELECT i, i*2 FROM c;
    COMMIT;"
  echo -n "page_size=$ps  pages=$(sqlite3 d.db 'PRAGMA page_count;')  size=$(stat -f%z d.db)  "
  echo -n "point-lookup: "
  /usr/bin/time -p sqlite3 d.db "SELECT v FROM t WHERE id=417283;" 2>&1 | grep real
done
```

Compute the theoretical tree depth for each page size (fan-out ≈ usable/6 for interior
table pages) and check it against measured page counts. Write up in
`labs/phase03/depth-report.md`.

---

## Lab 3.9 — `WITHOUT ROWID` vs rowid table

```sh
sqlite3 wr.db <<'SQL'
CREATE TABLE a(k TEXT PRIMARY KEY, v);                 -- rowid table + autoindex
CREATE TABLE b(k TEXT PRIMARY KEY, v) WITHOUT ROWID;   -- index b-tree IS the table
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<200000)
INSERT INTO a SELECT hex(randomblob(16)), i FROM c;
INSERT INTO b SELECT k, v FROM a;
COMMIT;
SQL
sqlite3 wr.db "SELECT name, count(*) pages, sum(pgsize) bytes FROM dbstat GROUP BY name;"
```

Answer in `labs/phase03/without-rowid.md`:
1. How many b-trees does table `a` have? How many does `b` have?
2. How many page reads does a PK lookup need in each?
3. When is `WITHOUT ROWID` a bad idea? (Hint: secondary indexes must store the full PK.)

---

## Lab 3.10 — Read the code, answer precisely

Answer each from the source, citing `file:line`, in `labs/phase03/source-questions.md`:

1. In `btreeInitPage()`, how are `maxLocal` and `minLocal` derived, and where do they come
   from for leaf vs. interior pages?
2. In `insertCell()`, what happens when the cell does not fit? Which field records it?
3. What exactly is `MemPage.nFree == -1` used for, and which function recomputes it?
4. In `balance_nonroot()`, why are new pages allocated in increasing page-number order?
5. What does `BTREE_FORDELETE` do, and which optimization depends on it?
6. Where does `sqlite3BtreeDelete()` handle deleting from an interior page, and what is the
   "rotate a predecessor up" code?
7. What does `sqlite3BtreeClearTable()` skip that a row-by-row delete would do?
8. Find three distinct `SQLITE_CORRUPT_BKPT` sites and state the invariant each protects.
