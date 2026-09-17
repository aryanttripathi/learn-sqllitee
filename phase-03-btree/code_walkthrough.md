# Phase 3 — Code Walkthrough: `btree.c`

Source: [`src/btree.c`](https://github.com/sqlite/sqlite/blob/master/src/btree.c) ·
[`src/btreeInt.h`](https://github.com/sqlite/sqlite/blob/master/src/btreeInt.h)

---

## Mental model for this phase

> **A `BtCursor` is a path from the root, and every b-tree operation is: fix the path, then
> edit the page at the end of it, then repair the tree upward.**

Three sub-rules that make `btree.c` readable:

1. **Search descends; balance ascends.** Every modifying function ends by walking *up*
   (`pCur->iPage--`) repairing pages.
2. **A page may be temporarily over-full in memory.** `pPage->apOvfl[]` holds cells with no
   room on disk. That is why insert code has no split logic in it — it just records
   overflow and calls `balance()`.
3. **Nothing here knows what a column is.** Keys are integers or opaque byte strings
   compared by a callback. If you catch yourself thinking about SQL, you are in the wrong
   file.

Read `btree.c` in this order: header comment → `MemPage`/`BtCursor` in `btreeInt.h` →
`moveToChild`/`moveToParent` → `sqlite3BtreeTableMoveto` → `insertCell` → `balance` →
`balance_nonroot` last, and only with a pencil.

---

## 1. `sqlite3BtreeTableMoveto()` — search, with two shortcuts before it starts

### Part A: don't search at all if you can avoid it

```c
SQLITE_PRIVATE int sqlite3BtreeTableMoveto(
  BtCursor *pCur,          /* The cursor to be moved */
  i64 intKey,              /* The table key */
  int biasRight,           /* If true, bias the search to the high end */
  int *pRes                /* Write search results here */
){
  assert( pCur->pKeyInfo==0 );          /* table b-tree ⇒ no collation info */
  assert( pCur->eState!=CURSOR_VALID || pCur->curIntKey!=0 );

  /* If the cursor is already positioned at the point we are trying
  ** to move to, then just return without doing any work */
  if( pCur->eState==CURSOR_VALID && (pCur->curFlags & BTCF_ValidNKey)!=0 ){
    if( pCur->info.nKey==intKey ){
      *pRes = 0;
      return SQLITE_OK;
    }
    if( pCur->info.nKey<intKey ){
      if( (pCur->curFlags & BTCF_AtLast)!=0 ){
        *pRes = -1;
        return SQLITE_OK;
      }
      /* If the requested key is one more than the previous key, then
      ** try to get there using sqlite3BtreeNext() rather than a full
      ** binary search.  This is an optimization only.  The correct answer
      ** is still obtained without this case, only a little more slowly. */
      if( pCur->info.nKey+1==intKey ){
        *pRes = 0;
        rc = sqlite3BtreeNext(pCur, 0);
        ...
      }
    }
  }
```

| Shortcut | Why it exists |
|---|---|
| `pCur->info.nKey==intKey` | already there. Happens constantly: `OP_SeekRowid` then `OP_Column` then `OP_Insert` all touch the same row. |
| `BTCF_AtLast` and key is larger | past the end; answer without touching a page. This is what makes appending rows O(1) at the cursor level. |
| `pCur->info.nKey+1==intKey` | **sequential access detection.** One `BtreeNext()` instead of a root-to-leaf descent. The comment is explicit that this is *only* an optimization — correctness never depends on it. |

That last comment is the pattern for reading optimizations anywhere in SQLite: they are
written so that deleting them changes speed, never answers.

### Part B: the descent and the binary search

```c
  rc = moveToRoot(pCur);
  ...
  for(;;){                                   /* one iteration per LEVEL of the tree */
    int lwr, upr, idx, c;
    MemPage *pPage = pCur->pPage;
    u8 *pCell;

    lwr = 0;
    upr = pPage->nCell-1;
    idx = upr>>(1-biasRight); /* idx = biasRight ? upr : (lwr+upr)/2; */

    for(;;){                                 /* binary search WITHIN the page */
      i64 nCellKey;
      pCell = findCellPastPtr(pPage, idx);
      if( pPage->intKeyLeaf ){
        while( 0x80 <= *(pCell++) ){         /* skip the payload-size varint */
          if( pCell>=pPage->aDataEnd ){
            return SQLITE_CORRUPT_PAGE(pPage);
          }
        }
      }
      getVarint(pCell, (u64*)&nCellKey);     /* now read the rowid varint */
      if( nCellKey<intKey ){
        lwr = idx+1;
        if( lwr>upr ){ c = -1; break; }
      }else if( nCellKey>intKey ){
        upr = idx-1;
        if( lwr>upr ){ c = +1; break; }
      }else{
        pCur->ix = (u16)idx;
        if( !pPage->leaf ){
          lwr = idx;
          goto moveto_table_next_layer;      /* interior hit ⇒ keep descending */
        }else{
          pCur->curFlags |= BTCF_ValidNKey;
          pCur->info.nKey = nCellKey;
          pCur->info.nSize = 0;
          *pRes = 0;
          return SQLITE_OK;                  /* leaf hit ⇒ found */
        }
      }
      idx = (lwr+upr)>>1;
    }
    ...descend into the chosen child...
  }
```

The lines worth stopping on:

- **`idx = upr>>(1-biasRight);`** — `biasRight` is 1 when the caller expects a high key
  (an append). Then the first probe is the *last* cell instead of the middle, so an
  append-heavy workload finds its slot in one comparison. The comment spells out the
  arithmetic because the shift is unreadable otherwise. `biasRight` ultimately comes from
  `OPFLAG_APPEND` in the bytecode — a hint that travels from the code generator, through
  the VDBE, into a bit shift here.
- **The `while( 0x80 <= *(pCell++) )` loop** is a hand-inlined "skip one varint". It does
  not decode the payload size — it only needs to *step over* it to reach the rowid. Not
  calling `btreeParseCell` here is worth real time: this loop runs `log₂(nCell)` times per
  page, per level, per seek.
- **`if( pCell>=pPage->aDataEnd ) return SQLITE_CORRUPT_PAGE(pPage);`** — the bounds check
  that makes a corrupt varint harmless. Note it is inside the hot loop, and SQLite still
  keeps it. Corruption safety is never traded for speed here.
- **`goto moveto_table_next_layer;` on an interior exact match** — in a B+tree the interior
  key is a *separator*, not the data, so an exact hit means "descend the left child", not
  "found".
- `pCur->info.nSize = 0;` — invalidate the cached parsed-cell size while keeping `nKey`.
  This kind of partial cache invalidation is everywhere in the cursor code; getting it wrong
  is a classic patch bug.

---

## 2. `balance()` — the whole repair policy in one loop

This is the function that decides which of the three balancing strategies runs. Real code,
trimmed of asserts but not of comments:

```c
static int balance(BtCursor *pCur){
  int rc = SQLITE_OK;
  u8 aBalanceQuickSpace[13];
  u8 *pFree = 0;

  do {
    int iPage;
    MemPage *pPage = pCur->pPage;

    if( NEVER(pPage->nFree<0) && btreeComputeFreeSpace(pPage) ) break;
    if( pPage->nOverflow==0 && pPage->nFree*3<=(int)pCur->pBt->usableSize*2 ){
      /* No rebalance required as long as:
      **   (1) There are no overflow cells
      **   (2) The amount of free space on the page is less than 2/3rds of
      **       the total usable space on the page. */
      break;
    }else if( (iPage = pCur->iPage)==0 ){
      if( pPage->nOverflow && (rc = anotherValidCursor(pCur))==SQLITE_OK ){
        /* The root page of the b-tree is overfull. ... call balance_deeper()
        ** to create a new child for the root-page and copy the current
        ** contents of the root-page to it. The next iteration of the do-loop
        ** will balance the child page. */
        rc = balance_deeper(pPage, &pCur->apPage[1]);
        if( rc==SQLITE_OK ){
          pCur->iPage = 1;
          pCur->ix = 0;
          pCur->aiIdx[0] = 0;
          pCur->apPage[0] = pPage;
          pCur->pPage = pCur->apPage[1];
        }
      }else{
        break;
      }
    }else if( sqlite3PagerPageRefcount(pPage->pDbPage)>1 ){
      /* The page being written is not a root page, and there is currently
      ** more than one reference to it. This only happens if the page is one
      ** of its own ancestor pages. Corruption. */
      rc = SQLITE_CORRUPT_PAGE(pPage);
    }else{
      MemPage * const pParent = pCur->apPage[iPage-1];
      int const iIdx = pCur->aiIdx[iPage-1];

      rc = sqlite3PagerWrite(pParent->pDbPage);
      ...
      if( pPage->intKeyLeaf
       && pPage->nOverflow==1
       && pPage->aiOvfl[0]==pPage->nCell
       && pParent->pgno!=1
       && pParent->nCell==iIdx
      ){
        rc = balance_quick(pParent, pPage, aBalanceQuickSpace);
      }else{
        u8 *pSpace = sqlite3PageMalloc(pCur->pBt->pageSize);
        rc = balance_nonroot(pParent, iIdx, pSpace, iPage==1,
                             pCur->hints&BTREE_BULKLOAD);
        if( pFree ) sqlite3PageFree(pFree);
        pFree = pSpace;
      }
      pPage->nOverflow = 0;

      /* The next iteration of the do-loop balances the parent page. */
      releasePage(pPage);
      pCur->iPage--;
      pCur->pPage = pCur->apPage[pCur->iPage];
    }
  }while( rc==SQLITE_OK );

  if( pFree ) sqlite3PageFree(pFree);
  return rc;
}
```

### The exit condition is the fill policy

```c
if( pPage->nOverflow==0 && pPage->nFree*3<=(int)pCur->pBt->usableSize*2 ) break;
```
Rearranged: stop unless there are overflow cells **or** `nFree > (2/3)·usable`. That single
line is SQLite's entire minimum-fill rule: **a page is allowed to sit at just over one-third
full.** There is no textbook 50% invariant. Fewer structural writes, slightly looser packing,
`VACUUM` as the cleanup path.

### The `balance_quick` condition, decoded

```c
pPage->intKeyLeaf          /* a table b-tree leaf (rowid keys, payload present) */
&& pPage->nOverflow==1     /* exactly one cell didn't fit */
&& pPage->aiOvfl[0]==pPage->nCell   /* and it belongs AFTER every existing cell */
&& pParent->pgno!=1        /* the parent is not the root */
&& pParent->nCell==iIdx    /* this page is the parent's RIGHT-MOST child */
```
Read as prose: *"one new cell, larger than everything, on the rightmost leaf."* That is
`INSERT` with an ascending rowid — the most common write in any SQLite application. It gets
a dedicated path that allocates one page for one cell and leaves the full page untouched,
which is why real SQLite reaches ~100% page fill on sequential loads while your
`minibtree.c` midpoint split reached 45% (Phase 3 Lab 3.1).

### The upward loop

```c
      pPage->nOverflow = 0;
      releasePage(pPage);
      pCur->iPage--;
      pCur->pPage = pCur->apPage[pCur->iPage];
    }
  }while( rc==SQLITE_OK );
```
Both `balance_quick` and `balance_nonroot` insert a divider cell into the parent, which may
itself overflow. Rather than recursing, `balance()` pops one level off the cursor path and
loops. **The cursor's `apPage[]`/`aiIdx[]` arrays are the recursion stack** — one more
reason the cursor is the central object in this file.

### The `pFree` dance

```c
u8 *pSpace = sqlite3PageMalloc(pCur->pBt->pageSize);
rc = balance_nonroot(pParent, iIdx, pSpace, ...);
if( pFree ) sqlite3PageFree(pFree);
pFree = pSpace;
```
A divider cell promoted into the parent may still point into the scratch buffer from the
*previous* level's balance. So each buffer is freed only after the *next* balance has
copied its contents somewhere permanent. The comment in the original source spends a
paragraph on this; it is the sort of lifetime subtlety that assertions cannot catch.

Note also `VVA_ONLY( int balance_quick_called = 0 );` plus
`assert( balance_quick_called==0 );` — a debug-only counter proving `balance_quick` runs at
most once per call, because `aBalanceQuickSpace[13]` is a single stack buffer that a second
call would clobber. The assert *is* the documentation of that constraint.

---

## 3. `balance_nonroot()` — mechanism only, do not read it first

~700 lines. [Source](https://github.com/sqlite/sqlite/blob/master/src/btree.c) — search for
`static int balance_nonroot(`. What it does, in eight steps:

```
1. Pick siblings: the overflowing page plus up to 2 neighbours (NB=3), using the
   parent's divider cells to find them.  → nOld, apOld[], apDiv[]
2. Flatten: copy pointers to EVERY cell of those pages, plus the overflow cells,
   plus (for index b-trees) the divider cells themselves, into one array.
                                          → b.apCell[], b.szCell[]
   For TABLE b-trees the dividers carry no payload, so they are regenerated rather
   than copied down — this is the `leafData` branch you will see everywhere.
3. Pack: walk the array deciding how many output pages (1..5) are needed and where
   the split points fall.                 → cntNew[], szNew[], nNew
4. Even out: shift the split points left/right so page sizes are balanced rather
   than "fill each page to the brim then start a new one".
5. Allocate/free pages so that the output pages are in INCREASING page-number order
   (keeps siblings physically ordered, which helps sequential scans).
6. Write cells into the pages with editPage()/rebuildPage(); the last cell of each
   non-final page becomes a divider promoted into the parent.
7. Fix up the parent: replace the old dividers with the new ones. The parent may now
   overflow — balance()'s do-loop handles that.
8. ptrmapPut() for every moved page, every child whose parent changed, and every
   overflow-chain head that moved (auto-vacuum only).
```

Read it with that list beside you, in the order 1→8, and skip everything else on the first
pass. The variable glossary is in `internals_important.md` §"balance_nonroot array glossary".

**The one question to answer while reading:** why must step 5 allocate in increasing
page-number order? (Hint: `ptrmap` updates and sequential-read locality — and the code says
so in a comment.)

---

## 4. Other large functions: what they do, when to open them

| Function | Lines | Mechanism |
|---|---|---|
| `sqlite3BtreeInsert()` | ~250 | Seek (unless `seekResult` says the cursor is already placed) → `fillInCell()` builds the cell, allocating overflow pages → if an entry with that key exists and is the same size, **overwrite in place**; else `dropCell` + `insertCell` → `balance()` if overflow. The overwrite fast path is why updating a fixed-width column is far cheaper than changing a row's length. |
| `sqlite3BtreeDelete()` | ~200 | `saveAllCursors()` first. If the entry is on an *interior* page, it is a divider: find the largest key in the left subtree and rotate that leaf cell up into the divider slot, then delete from the leaf instead. Then `dropCell` → `clearCell` (frees the overflow chain) → `balance()`. |
| `insertCell()` | ~90 | If the cell fits: `allocateSpace()` (freeblock first, then the gap, `defragmentPage()` if fragmented), memcpy the body, shift the 2-byte pointer array, `nCell++`. If it does not fit: record it in `apOvfl[]`, `nOverflow++`, and return — no split logic lives here. |
| `allocateSpace()` / `freeSpace()` | ~80 each | The slotted-page allocator: freeblock list walk, 4-byte minimum block, remainders under 4 bytes become fragments counted in header byte 7, `defragmentPage()` when fragments exceed 60. |
| `accessPayload()` | ~150 | Read/write across the overflow chain, with an optional direct-from-disk path. |
| `sqlite3BtreeIntegrityCheck()` | ~500 | The invariant list, executable. |

---

## 5. Cursor invalidation: the three functions that make concurrency safe

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^static int saveCursorPosition(BtCursor \*pCur){/,/^}/p' $A/sqlite3.c
sed -n '/^static int saveAllCursors(/,/^}/p' $A/sqlite3.c
```

`saveAllCursors(pBt, iRoot, pExcept)` walks **every** cursor open on this file, and for each
one that is `CURSOR_VALID` on the affected tree: copies its key into `pCur->pKey`, sets
`eState = CURSOR_REQUIRESEEK`, and releases its pages. Any later use calls
`btreeRestoreCursorPosition()`, which re-seeks by that saved key.

Why it must exist: `UPDATE t SET b=... WHERE a>5` has one cursor reading and one writing the
same b-tree. The writer's `balance()` can move the reader's page out from under it. Rather
than pinning pages or forbidding the pattern, SQLite demotes the reader to "I know my key,
I'll find it again". Cheap, and it is why cursors store keys rather than page pointers when
idle.

---

## 6. Exercises against real source

Answer in `labs/phase03/source-questions.md` with `src/btree.c` + function citations:

1. In `sqlite3BtreeTableMoveto`, what is `biasRight` and where does its value originate?
   Trace it back through `sqlite3BtreeInsert` to a bytecode flag.
2. Why does the in-page binary search skip the payload varint with a hand-written `while`
   loop instead of calling `btreeParseCell`? Estimate how many times that loop runs for a
   single seek in a 1M-row table.
3. State `balance()`'s no-op condition in plain English, then compute: for a 4096-byte page
   with 12 reserved bytes, how many bytes of free space trigger a rebalance?
4. Write out `balance_quick`'s five conditions as prose, then write a single SQL statement
   that satisfies all five, and one that fails exactly one of them.
5. In `balance()`, why is `aBalanceQuickSpace[13]` exactly 13 bytes? (Hint: what is the
   largest possible table-interior cell?)
6. Find `anotherValidCursor()`. Why must the root-overflow case check it before calling
   `balance_deeper`?
7. Find `dropCell()` and `freeSpace()`. What happens to a 3-byte hole, and where is it
   counted?
8. In `sqlite3BtreeDelete()`, find the "rotate a predecessor up" code. Why can't SQLite
   simply remove the divider cell from the interior page?
