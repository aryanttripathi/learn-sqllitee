# Phase 3 — Internals Reference: `btree.c`

## The public b-tree API (what the VDBE may call)

```c
/* lifecycle */
int  sqlite3BtreeOpen(sqlite3_vfs*, const char *zFile, sqlite3*, Btree**, int flags, int vfsFlags);
int  sqlite3BtreeClose(Btree*);

/* transactions */
int  sqlite3BtreeBeginTrans(Btree*, int wrflag, int *pSchemaVersion);
int  sqlite3BtreeCommitPhaseOne(Btree*, const char *zSuperJrnl);
int  sqlite3BtreeCommitPhaseTwo(Btree*, int bCleanup);
int  sqlite3BtreeRollback(Btree*, int tripCode, int writeOnly);
int  sqlite3BtreeBeginStmt(Btree*, int iStatement);
int  sqlite3BtreeSavepoint(Btree*, int op, int iSavepoint);

/* schema-level */
int  sqlite3BtreeCreateTable(Btree*, Pgno *piTable, int flags);
int  sqlite3BtreeDropTable(Btree*, int iTable, int *piMoved);
int  sqlite3BtreeClearTable(Btree*, int iTable, i64 *pnChange);
void sqlite3BtreeGetMeta(Btree*, int idx, u32 *pValue);
int  sqlite3BtreeUpdateMeta(Btree*, int idx, u32 iMeta);

/* cursors */
int  sqlite3BtreeCursor(Btree*, Pgno iTable, int wrFlag, KeyInfo*, BtCursor*);
int  sqlite3BtreeCloseCursor(BtCursor*);
int  sqlite3BtreeFirst(BtCursor*, int *pRes);
int  sqlite3BtreeLast(BtCursor*, int *pRes);
int  sqlite3BtreeNext(BtCursor*, int flags);
int  sqlite3BtreePrevious(BtCursor*, int flags);
int  sqlite3BtreeTableMoveto(BtCursor*, i64 intKey, int biasRight, int *pRes);
int  sqlite3BtreeIndexMoveto(BtCursor*, UnpackedRecord *pIdxKey, int *pRes);
i64  sqlite3BtreeIntegerKey(BtCursor*);
u32  sqlite3BtreePayloadSize(BtCursor*);
int  sqlite3BtreePayload(BtCursor*, u32 offset, u32 amt, void *pBuf);
const void *sqlite3BtreePayloadFetch(BtCursor*, u32 *pAmt);   /* zero-copy */
int  sqlite3BtreeInsert(BtCursor*, const BtreePayload *pX, int flags, int seekResult);
int  sqlite3BtreeDelete(BtCursor*, u8 flags);
int  sqlite3BtreeCount(sqlite3*, BtCursor*, i64*);
int  sqlite3BtreeIntegrityCheck(sqlite3*, Btree*, Pgno *aRoot, Mem*, int nRoot, int, int*, char**);
```

`BtreePayload` (the insert argument) is worth knowing:
```c
typedef struct BtreePayload {
  const void *pKey;   /* index key (NULL for table b-trees) */
  sqlite3_int64 nKey; /* rowid for table b-trees; key size for index b-trees */
  const void *pData;  /* the record, for table b-trees */
  sqlite3_value *aMem;/* or, the values to serialize (avoids one memcpy) */
  u16 nMem;
  int nData, nZero;   /* nZero: trailing zero bytes (zeroblob optimization) */
} BtreePayload;
```

Insert flags: `BTREE_SAVEPOSITION` (keep the cursor where it was),
`BTREE_APPEND` (hint: this key is larger than all others — enables `balance_quick`),
`BTREE_PREFORMAT` (payload already built in a scratch page, used by transfer-optimizations).

Delete flags: `BTREE_SAVEPOSITION`, `BTREE_AUXDELETE` (this is an index entry deletion
accompanying a table row deletion, not the primary one).

Cursor hints: `BTREE_BULKLOAD`, `BTREE_SEEK_EQ` (the cursor is only used for equality
seeks, so `moveToRoot` can skip work), and `BTREE_FORDELETE` (a write cursor whose only job
is deletion — allows skipping some balance work).

## Internal functions, grouped

### Page mechanics
`zeroPage`, `btreeInitPage`, `decodeFlags`, `btreeGetPage`, `getAndInitPage`,
`releasePage`, `btreeComputeFreeSpace`, `allocateSpace`, `freeSpace`, `defragmentPage`,
`pageFindSlot`, `cellSizePtr`, `btreeParseCell`, `btreeParseCellPtr`,
`btreeParseCellPtrIndex`, `btreeParseCellPtrNoPayload`.

### Payload/overflow
`fillInCell`, `accessPayload`, `sqlite3BtreePayloadFetch`, `getOverflowPage`,
`clearCell`, `clearCellOverflow`, `ptrmapPutOvflPtr`.

### Navigation
`moveToRoot`, `moveToChild`, `moveToParent`, `moveToLeftmost`, `moveToRightmost`,
`btreeNext`, `btreePrevious`, `saveCursorPosition`, `saveAllCursors`,
`btreeRestoreCursorPosition`, `sqlite3BtreeCursorHasMoved`, `btreeCursorHasMoved`.

### Modification
`insertCell`, `dropCell`, `balance`, `balance_quick`, `balance_deeper`, `balance_nonroot`,
`copyNodeContent`, `rebuildPage`, `editPage`, `pageInsertArray`, `pageFreeArray`.

### Space management
`allocateBtreePage`, `freePage`, `freePage2`, `btreeGetUnusedPage`.

### Auto-vacuum
`ptrmapPageno`, `ptrmapPut`, `ptrmapGet`, `ptrmapPutOvflPtr`, `relocatePage`,
`incrVacuumStep`, `sqlite3BtreeIncrVacuum`, `autoVacuumCommit`.

### Shared cache (when enabled)
`querySharedCacheTableLock`, `setSharedCacheTableLock`, `clearAllSharedCacheTableLocks`,
`downgradeAllSharedCacheTableLocks`, `hasSharedCacheTableLock` (assert-only).

## `balance_nonroot` array glossary (read this while reading the function)

| Name | Meaning |
|---|---|
| `pParent` | the parent page being rebalanced through |
| `iParentIdx` | index of the divider cell for the overflowing child |
| `nOld` | number of sibling pages taken (1..3) |
| `apOld[]` | those sibling `MemPage*` |
| `apDiv[]` | pointers to the divider cells in the parent that separate them |
| `szNew[]`, `cntNew[]` | target byte size and cumulative cell count per output page |
| `nNew` | number of output pages (1..5) |
| `apNew[]` | output pages (reused old ones + newly allocated) |
| `b.apCell[]`, `b.szCell[]` | the flattened pool of ALL cells (siblings + dividers + overflow) |
| `b.ixNx[]` | index boundaries used by `editPage`/`pageInsertArray` |
| `aSpace1` | scratch memory for divider cells that must be rebuilt |
| `aPgOrder[]`, `aPgno[]` | used to assign new pages in increasing page-number order |
| `aPgFlags[]` | pager flags per page |

Reading order suggestion:
1. the sibling-selection block (choose `nOld`, fill `apOld`/`apDiv`)
2. the cell-gathering loop (note how table-b-tree dividers are *not* copied into the pool,
   because they carry no payload — `nDiv`/`leafData` logic)
3. the packing loop computing `cntNew`/`szNew`
4. the "even out the sizes" adjustment loop
5. the page allocation + page-number ordering block
6. `editPage()` / `rebuildPage()` writing cells into pages
7. the parent fixup (insert new dividers)
8. the `ptrmapPut` loop

## Where the cursor lives

```
BtCursor
  ├ iPage = 2                       depth in the tree (0 = root)
  ├ apPage[0] = root MemPage        │ path from the root
  ├ apPage[1] = interior MemPage    │
  ├ pPage     = leaf MemPage        ┘ (apPage[iPage] == pPage)
  ├ aiIdx[0]  = 3                   cell index chosen at each level
  ├ aiIdx[1]  = 17
  └ ix        = 42                  cell index on the current (leaf) page
```

`BTCURSOR_MAX_DEPTH` = 20. A file claiming deeper is rejected as corrupt.

## Insert/delete decision tables

**Insert, page has room**
```
exists?  same size?    action
 no      -            insertCell (allocateSpace, maybe defragment)
 yes     yes          overwrite in place (fast path, BTREE_SAVEPOSITION-friendly)
 yes     no           dropCell + insertCell
```

**Insert, page full** → cell goes to `apOvfl[]`, `nOverflow++`, then `balance()`:
```
iPage==0 (root)                              → balance_deeper
nOverflow==1 && intKeyLeaf && rightmost      → balance_quick
otherwise                                    → balance_nonroot (up to 3 siblings)
loop upward while the parent now overflows
```

**Delete**
```
entry on interior page → rotate the predecessor leaf cell up into the divider slot,
                         then delete from the leaf
dropCell → freeSpace() → clearCell() frees overflow pages
balance() only if nOverflow>0 or nFree > usableSize*2/3
```

## Invariants you can assert in a debugger

1. `pCur->apPage[pCur->iPage] == pCur->pPage`
2. `pPage->nCell*2 + pPage->hdrOffset + hdrSize <= cellTop`
3. For interior pages, `nCell >= 1`
4. `nFree == sum(freeblocks) + fragments + gap` (recomputed by `btreeComputeFreeSpace`)
5. Keys strictly increase across the cell pointer array
6. A cursor with `eState==CURSOR_VALID` has `ix < pPage->nCell`
7. With auto-vacuum on, `ptrmapGet(child) == (PTRMAP_BTREE, parent)` for every child

## Gotchas

1. **`sqlite3BtreePayloadFetch()` returns a pointer into the page buffer.** It is invalid
   after any b-tree call that may move/modify pages. `OP_Column` copies before it can move.
2. **Root page numbers never change** (except via auto-vacuum relocation, which then
   updates `sqlite_schema` via `BTREE_ROOT_PAGE_MOVED`). That is why `balance_deeper`
   copies the root's content down rather than promoting a new page.
3. `nFree == -1` means "not computed yet"; call `btreeComputeFreeSpace()`. Forgetting this
   is a classic patch bug.
4. `saveAllCursors()` must be called *before* any structural change, or another cursor may
   read a page that has moved.
5. In shared-cache mode a `BtShared` may be used by several connections — every entry point
   takes `sqlite3BtreeEnter()`.
6. `BTREE_APPEND` is a *hint*; correctness must not depend on it, only performance.
7. `sqlite3BtreeInsert` with `seekResult != 0` means "the cursor is already positioned;
   don't seek again" — the VDBE passes the result of a preceding `OP_NotExists`/`OP_SeekGE`.
   Passing a stale value is a source of subtle corruption; the code asserts heavily here.
8. `DELETE FROM t` without WHERE uses `sqlite3BtreeClearTable` (the *truncate optimization*)
   and therefore does not fire per-row triggers and does not count rows unless
   `SQLITE_ENABLE_UPDATE_DELETE_LIMIT`/`changes()` requires it. That's why `changes()`
   after a bare `DELETE FROM` historically returned 0 (fixed by counting in the pager).

## Performance model (memorize)

```
point lookup by rowid      : depth page reads (≈3–4), all cached after warmup
index lookup + row fetch   : index depth + table depth reads  ⇒ ~2× a covering index
full table scan            : pages(table) sequential reads
index scan (non-covering)  : pages(index) + one random table read PER ROW  ← the
                             reason the planner sometimes prefers a full scan
insert (sequential rowid)  : ~1 page write, balance_quick, ~100% fill
insert (random key)        : ~depth page writes, splits, ~65–75% fill
delete                     : like insert, plus freelist maintenance
```
