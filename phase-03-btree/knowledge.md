# Phase 3 — `btree.c`: The Ordered Map That Everything Sits On

The b-tree layer turns "pages" into "a sorted key→value map with cursors". It knows nothing
about SQL, types, or affinity. Its whole API is about 40 functions, and the VDBE uses
maybe 25 of them.

---

## 3.1 What kind of tree is it, exactly?

SQLite uses a **B+-tree for tables** and a **B-tree for indexes** — with a twist:

| | Table b-tree | Index b-tree |
|---|---|---|
| key | 64-bit signed rowid | a record (the indexed columns + rowid) |
| data | the row record | none (key only) |
| interior pages hold | child pointers + separator rowids, **no payload** | child pointers + **full keys** |
| leaves hold | all the data | all the keys |
| comparison | integer compare | `sqlite3VdbeRecordCompare` with a `KeyInfo` (collations, DESC flags) |

Because table interior pages carry no payload, fan-out is huge:
with a 4 KiB page, an interior table page holds ~(4096−12)/(4+~2) ≈ **650 children**.
Three levels ⇒ ~275 million rows. **Table b-trees are essentially always ≤ 4 levels deep.**

```
                      TABLE B-TREE (B+ style)
                    ┌────────────────────────┐
     interior       │ (c=2,k=100) (c=3,k=200)│  right=4
                    └───┬──────────┬──────┬──┘
              ┌─────────┘          │      └────────┐
        ┌─────▼─────┐        ┌─────▼─────┐   ┌─────▼─────┐
 leaf   │rowid 1..100│       │101..200   │   │201..∞     │
        │ + payloads │       │+ payloads │   │+ payloads │
        └───────────┘        └───────────┘   └───────────┘
        (all data lives in leaves; interior = navigation only)
```

Note what SQLite does **not** have: sibling pointers between leaves. `BtreeNext()` walks
back up to the parent instead. That keeps updates cheap (no pointer maintenance during
splits) at the cost of an occasional parent hop during scans — and the parent is almost
always in cache.

---

## 3.2 The objects

```c
struct Btree {            /* per-connection handle */
  sqlite3 *db;
  BtShared *pBt;          /* shared, per-file state */
  u8 inTrans;             /* TRANS_NONE / TRANS_READ / TRANS_WRITE */
  u8 sharable, locked;
  int wantToLock, nBackup;
  u32 iBDataVersion;
  Btree *pNext, *pPrev;   /* list of all Btrees on this connection */
};

struct BtShared {         /* one per open database file (per process) */
  Pager *pPager;
  sqlite3 *db;
  BtCursor *pCursor;      /* ALL open cursors on this file, linked */
  MemPage *pPage1;        /* page 1, held pinned while a transaction is open */
  u8 openFlags, autoVacuum, incrVacuum, bDoTruncate;
  u8 inTransaction;       /* TRANS_NONE/READ/WRITE */
  u8 max1bytePayload;
  u16 btsFlags;           /* BTS_READ_ONLY, BTS_SECURE_DELETE, BTS_OVERWRITE, ... */
  u16 maxLocal, minLocal, maxLeaf, minLeaf;   /* the spill formula, precomputed */
  u32 pageSize, usableSize;
  int nTransaction;       /* # of open transactions (read + write) */
  u32 nPage;              /* db size in pages */
  void *pSchema;          /* the Schema object for this file */
  ...
};

struct BtCursor {
  u8 eState;              /* VALID / INVALID / SKIPNEXT / REQUIRESEEK / FAULT */
  u8 curFlags;            /* BTCF_WriteFlag, BTCF_ValidNKey, BTCF_ValidOvfl, ... */
  u8 curPagerFlags;
  u8 hints;               /* BTREE_BULKLOAD, BTREE_SEEK_EQ */
  int nKey;               /* size of pKey, or the rowid for table cursors */
  Pgno pgnoRoot;
  BtCursor *pNext;
  CellInfo info;          /* parsed cell at the current position */
  void *pKey;             /* saved key when eState==REQUIRESEEK */
  Btree *pBtree; BtShared *pBt;
  KeyInfo *pKeyInfo;      /* collations etc. for index cursors; NULL for table cursors */
  i8 iPage;               /* current depth: index into apPage[]/aiIdx[] */
  MemPage *pPage;         /* current page */
  MemPage *apPage[BTCURSOR_MAX_DEPTH];  /* the path from root */
  u16 aiIdx[BTCURSOR_MAX_DEPTH];        /* cell index at each level */
  u16 ix;                 /* cell index on the current page */
};
```

**The cursor *is* the path from the root.** `apPage[]`/`aiIdx[]` record which cell you
descended through at each level, so `BtreeNext()` and `BtreePrev()` can walk back up.
`BTCURSOR_MAX_DEPTH` is 20 — a hard limit on tree depth, and a corruption check.

### Cursor states

| State | Meaning |
|---|---|
| `CURSOR_VALID` | points at a real entry; `pPage`/`ix` are meaningful |
| `CURSOR_INVALID` | points nowhere (before first / after last / empty table) |
| `CURSOR_SKIPNEXT` | the entry it pointed at was deleted; the next `Next()`/`Prev()` is a no-op that just re-validates |
| `CURSOR_REQUIRESEEK` | the tree changed under it; the key was saved in `pKey`, and the cursor must re-seek before use |
| `CURSOR_FAULT` | an unrecoverable error occurred; `skipNext` holds the error code |

`CURSOR_REQUIRESEEK` is the mechanism that makes concurrent cursors safe on the same tree:
before any structural change, `saveAllCursors()` converts every *other* cursor into
REQUIRESEEK, storing its key. `restoreCursorPosition()` re-seeks lazily on next use. This
is why `UPDATE t SET x=... WHERE ...` (which reads and writes the same b-tree) works at
all.

---

## 3.3 Search: `sqlite3BtreeTableMoveto` / `sqlite3BtreeIndexMoveto`

```
moveToRoot()
  ↓
loop:
  binary search the cell pointer array of the current page   ← keys are sorted
  if leaf:   done. *pRes = comparison result (0 = exact match)
  else:      moveToChild(childPointerOfChosenCell)           ← descend
```

Binary search inside a page:

```c
/* Simplified from sqlite3BtreeTableMoveto() */
lwr = 0; upr = pPage->nCell - 1;
while( lwr <= upr ){
  int idx = (lwr + upr) / 2;
  i64 nCellKey = <rowid varint of cell idx>;
  if( nCellKey < intKey )      lwr = idx + 1;
  else if( nCellKey > intKey ) upr = idx - 1;
  else { /* exact hit */ pCur->ix = idx; ... return SQLITE_OK; }
}
```

Two important optimizations you will see in the real code:

1. **The rowid varint fast path.** For table b-trees the code reads the rowid varint
   directly from the cell without calling `btreeParseCell()` — a hand-unrolled loop for
   1-byte and 2-byte varints, because this is the hottest loop in the library.
2. **The "already there" check.** `sqlite3BtreeTableMoveto` first checks whether the
   cursor is already positioned at, or adjacent to, the target (`pCur->curFlags &
   BTCF_ValidNKey`). Sequential inserts and scans hit this constantly.

For index b-trees the comparison goes through
`sqlite3VdbeRecordCompare(nKey, pKey, pUnpacked)`, which respects each column's collating
sequence and DESC flag from `KeyInfo`. There are specialized variants
(`vdbeRecordCompareInt`, `vdbeRecordCompareString`) selected at prepare time when the first
key column has a known simple type — a nice example of SQLite's "specialize the hot path"
style.

---

## 3.4 Insert

```
sqlite3BtreeInsert(pCur, pX, flags)
  1. if not already positioned: seek to the key            (Moveto)
  2. build the cell into a scratch buffer                  (fillInCell)
       - compute local payload size via the spill formula
       - allocate overflow pages and chain them if needed
  3. if an entry with this key exists (loc==0):
        - if the new cell is the same size → OVERWRITE in place (fast path)
        - else dropCell(old) then insertCell(new)
     else insertCell(new)
  4. insertCell():
        - if there is room in the page's free space → allocateSpace(), memcpy,
          shift the cell pointer array, nCell++
        - if NOT → record the cell in pPage->apOvfl[] and set nOverflow
  5. if pPage->nOverflow: balance(pCur)
```

The **overflow cell array** is the trick that keeps the code simple: a page is allowed to
be temporarily over-full, holding cells in memory that have no room on disk. `balance()`
then fixes it immediately. (Check `btreeInt.h` in your tree for how many slots `apOvfl[]`
has — it has shrunk over the years as balancing became more eager.)

### `insertCell` in pictures

```
 before                                     after insert at index 1
 ┌────┬────────────────┬────────────┐        ┌────┬──────────────────┬───────────┐
 │hdr │ ptr0 ptr1 ptr2 │  cells ... │        │hdr │ptr0 ptrNEW ptr1 ptr2│cells+NEW│
 └────┴────────────────┴────────────┘        └────┴──────────────────┴───────────┘
   the pointer array shifts right by 2 bytes;  the cell body is allocated
   from free space (freeblock reuse first, then the gap)
```

`allocateSpace()` policy: try the freeblock list first (best fit-ish, splitting only if the
remainder ≥ 4 bytes, else the remainder becomes fragments); if that fails, take from the
gap; if the gap is too small but total free space is enough, `defragmentPage()` first.

---

## 3.5 `balance()` — the heart of the module

```c
static int balance(BtCursor *pCur){
  const int nMin = pCur->pBt->usableSize * 2 / 3;
  do {
    MemPage *pPage = pCur->pPage;
    if( NEVER(pPage->nFree<0) && btreeComputeFreeSpace(pPage) ) break;
    if( pPage->nOverflow==0 && pPage->nFree<=nMin ){
      break;                                  /* page is fine; nothing to do */
    } else if( pCur->iPage==0 ){
      rc = balance_deeper(pPage, &pCur->apPage[1]);   /* root is full → grow height */
      ...
    } else {
      /* try the cheap append case first */
      if( pPage->nOverflow==1 && pPage->intKeyLeaf && <cursor at rightmost> ){
        rc = balance_quick(pParent, pPage, aBalanceQuickSpace);
      } else {
        rc = balance_nonroot(pParent, iIdx, aBalanceQuickSpace, ...);
      }
      pCur->iPage--;                           /* move up and repeat */
    }
  } while( rc==SQLITE_OK );
  return rc;
}
```

Three cases, in increasing cost:

### (a) `balance_quick` — the append fast path
When you insert monotonically increasing rowids (the overwhelmingly common case:
`INSERT` without an explicit rowid), the rightmost leaf overflows by exactly one cell.
Instead of redistributing, allocate a brand-new page and put the single overflow cell
there:

```
 parent  ... [c=7,k=900]  right=8              ... [c=7,k=900][c=8,k=1000] right=9
              ┌──────┐   ┌──────┐                      ┌──────┐  ┌──────┐   ┌──────┐
              │ p7   │   │ p8   │  overflow    ──►     │ p7   │  │ p8   │   │ p9   │
              │ full │   │ FULL │  +1 cell             │ full │  │ full │   │1 cell│
              └──────┘   └──────┘                      └──────┘  └──────┘   └──────┘
```
Result: pages stay ~100% full, and bulk-loading sorted data is near-optimal. This is why
inserting in rowid order is dramatically faster and denser than random order.

### (b) `balance_deeper` — root split
The root page number must never change (it is recorded in `sqlite_schema`). So when the
root fills up, SQLite allocates a **new child**, copies the root's contents into it, and
turns the root into an interior page with a single right-child pointer. Then it balances
that child normally.

```
 root(pgno=2) FULL                root(pgno=2) interior, right=9
 [cells...]             ──►       └──► page 9 = old root contents (then balanced)
```

### (c) `balance_nonroot` — the real algorithm

Take the overflowing page plus **up to 2 siblings** (NB = 3), pull *all* their cells into
one array, and redistribute evenly across as many pages as needed (up to 5 output pages).

```
 STEP 1: choose siblings                        parent
                                    ┌──────────────────────────────┐
                                    │ ... d1 d2 d3 ...  (dividers) │
                                    └───┬─────┬─────┬──────────────┘
                                        ▼     ▼     ▼
                                     [ A ] [ B* ] [ C ]      B* = overflowing page

 STEP 2: gather all cells, including the divider cells from the parent
         apCell[] = A.cells + d1 + B.cells(+overflow) + d2 + C.cells
         (divider cells come down into the pool: for index b-trees they are real keys;
          for table b-trees the divider's payload-free form is regenerated)

 STEP 3: compute how many output pages k are needed, packing cells greedily
         then RE-BALANCE the split points so page sizes are even
         (the code walks the split points left, moving cells to equalize)

 STEP 4: allocate new pages / free surplus pages, in PAGE NUMBER ORDER
         (important: pages are renumbered so that siblings stay roughly ordered on disk)

 STEP 5: write cells into the new pages; the last cell of each non-final page
         becomes a DIVIDER cell that is promoted into the parent
         for table b-trees the divider is (child, key) with no payload;
         for index b-trees the whole key is copied up

 STEP 6: fix up the parent: replace the old divider cells with new ones
         (this may overflow the parent → the do-loop in balance() moves up a level)

 STEP 7: update the pointer map (auto-vacuum) for every moved page,
         every overflow-chain head whose parent changed, and every child
         whose parent changed
```

Why up to 3 siblings instead of 2? Redistributing over 3 pages postpones splits: a page
that would split in a textbook B-tree often just shifts a few cells to a neighbour here.
The practical result is **higher average page fill (~70-90% for random inserts)**.

The function is ~700 lines and is the densest code in SQLite. Read it with these anchors:
`apOld[]`, `apNew[]`, `apCell[]`, `szCell[]`, `cntNew[]`, `szNew[]`, `apDiv[]`, `pgno`
ordering loop, and the final `ptrmapPut` loop.

---

## 3.6 Delete

```
sqlite3BtreeDelete(pCur, flags)
  1. saveAllCursors()   — every other cursor on this tree becomes REQUIRESEEK
  2. if the entry is on an INTERIOR page:
       - it is a divider; we cannot just remove it
       - find the LARGEST entry in the left subtree (the predecessor leaf cell)
       - move that cell up to replace the divider          ← "rotate"
       - then delete from the leaf instead
  3. dropCell(leafPage, idx)   — free the cell's bytes, shrink the pointer array
       - freeSpace() adds the hole to the freeblock list (or grows the gap)
       - overflow chain pages, if any, are freed to the freelist
  4. balance(pCur)  — merges siblings if the page is now under-full
```

**SQLite does not enforce a minimum fill factor.** `balance()` only acts when
`nFree > usableSize*2/3` (i.e. the page is less than ~1/3 full) or there are overflow
cells. So a b-tree can legally contain very sparse pages. This is a deliberate trade:
fewer structural writes, at the cost of some space. `VACUUM` is the cleanup mechanism.

Fast paths: `sqlite3BtreeClearTable()` (used by `DELETE FROM t` with no WHERE — "the
truncate optimization") frees every page of the tree to the freelist without touching
cells at all.

---

## 3.7 Cursor movement

| Function | What it does |
|---|---|
| `sqlite3BtreeFirst()` | `moveToRoot()` then `moveToLeftmost()` |
| `sqlite3BtreeLast()` | `moveToRoot()` then `moveToRightmost()` |
| `sqlite3BtreeNext()` | `ix++`; if past the end of the page, walk **up** via `moveToParent()` until a parent cell remains, then descend leftmost |
| `sqlite3BtreePrev()` | mirror image |
| `sqlite3BtreePayload()` / `PayloadFetch()` | read the record, following overflow pages |
| `sqlite3BtreeIntegerKey()` | the rowid at the current position |

```
   Next() walking off the end of a leaf:

        parent (interior)                      ▲ 3. descend leftmost into next child
        ┌────┬────┬────┐                       │
        │ c1 │ c2 │ c3 │ right                 │
        └─┬──┴─┬──┴─┬──┘                       │
          │    │    │                          │
       [leaf][leaf][leaf]                      │
          ▲ 1. ix == nCell-1                   │
          └─ 2. moveToParent(), aiIdx[iPage]++ ┘
```

`sqlite3BtreePayloadFetch()` returns a *direct pointer into the page buffer* when the whole
payload is local — zero-copy. That pointer is only valid until the next b-tree call, which
is the source of many subtle VDBE rules (`OP_Column` must not hold it across a `Next`).

---

## 3.8 Transactions at the b-tree level

```
sqlite3BtreeBeginTrans(p, wrflag, pSchemaVersion)
   wrflag 0 → read transaction: pager SHARED lock, pin page 1
   wrflag 1 → write transaction: pager RESERVED lock
   wrflag 2 → also take EXCLUSIVE immediately (BEGIN EXCLUSIVE)

sqlite3BtreeCommitPhaseOne(p, zSuperJrnl)   → pager commit phase one (+ autovacuum work)
sqlite3BtreeCommitPhaseTwo(p, bCleanup)     → pager commit phase two = commit point
sqlite3BtreeRollback(p, tripCode, writeOnly)→ playback + invalidate cursors
```

Note `sqlite3BtreeBeginTrans` also compares the schema cookie: if another connection
changed the schema, your prepared statements are invalidated (`SQLITE_SCHEMA` →
automatic re-prepare in `prepare_v2`).

**Table/index creation and destruction** are b-tree operations too:
`sqlite3BtreeCreateTable()` (allocate a root page, return its number),
`sqlite3BtreeDropTable()`, `sqlite3BtreeClearTable()`.

---

## 3.9 Auto-vacuum and page relocation

Incremental vacuum moves pages from the end of the file into free slots nearer the front,
then truncates:

```
incrVacuumStep()
  → find the last page in the file
  → if it is a free page, just truncate
  → else relocatePage(): copy it to a free page nearer the front,
      then fix EVERY pointer to it:
        - its parent b-tree page's child pointer (found via the PTRMAP)
        - or the previous overflow page's next pointer
        - or sqlite_schema.rootpage if it is a root page (via `BTREE_ROOT_PAGE_MOVED`)
      → update the ptrmap entries for the moved page and all its children
```

That "fix every pointer" step is precisely what the pointer map exists for; without it the
operation would be O(database size).

---

## 3.10 Concurrency inside one file: shared cache & table locks

With `sqlite3_enable_shared_cache()`, several connections in one process share a
`BtShared`. Then the b-tree layer must do its own **table-level** read/write locks
(`BtLock`, `querySharedCacheTableLock()`, `setSharedCacheTableLock()`), because the pager's
file locks can't distinguish them. Shared-cache mode is discouraged today (WAL is better),
but the code is instructive: it is a miniature lock manager inside `btree.c`.

---

## 3.11 Defensive programming: corruption handling

Every function that reads a cell can encounter a hostile file. The idioms:

```c
#define SQLITE_CORRUPT_BKPT   sqlite3CorruptError(__LINE__)
if( iCellOfst > pPage->pBt->usableSize ) return SQLITE_CORRUPT_PAGE(pPage);
assert( CORRUPT_DB || <invariant that must hold on a valid file> );
```

`assert( CORRUPT_DB || X )` is the signature SQLite pattern: "X is guaranteed *unless* the
file is corrupt". When you read `btree.c`, treat every such assert as a line of the file
format specification.

`PRAGMA cell_size_check=ON` turns on extra validation of cell sizes at insert time — used
by fuzzers to catch corruption earlier.

---

## 3.12 Performance facts worth memorizing

1. Sequential rowid inserts hit `balance_quick` ⇒ ~100% page fill, minimal rebalancing.
   Random inserts (e.g. UUID text primary keys) cause splits everywhere ⇒ ~65-75% fill and
   far more I/O. **This is the single biggest schema-design lever in SQLite.**
2. A covering index avoids the table b-tree entirely: the index leaf has everything.
3. `WITHOUT ROWID` turns the table into an index b-tree keyed by the PK — one less b-tree
   lookup for PK access, but the PK is repeated in every secondary index entry.
4. Large blobs go to overflow pages; scanning a table with big blobs is slow even when you
   don't select them, unless the column is late in the record (it still costs page reads
   only if touched — but the *row* cell is bigger, so fewer rows per page).
5. Tree depth is what you pay per lookup. Depth ≈ log_fanout(N). Increasing page size
   increases fan-out and lowers depth, at the cost of write amplification.
