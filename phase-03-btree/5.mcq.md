# Phase 3 — MCQ (25 questions)

1. SQLite table b-trees are:
   a) B-trees with data in interior nodes  b) B+-trees: data only in leaves
   c) LSM trees  d) hash indexes

2. Interior **table** page cells contain:
   a) child pointer + rowid, no payload  b) child pointer + full row
   c) rowid + payload  d) key only

3. Which is deeper for the same row count, and why?
   a) index b-tree, because interior pages carry keys  b) table b-tree, because rows are big
   c) they are identical  d) depends on page size only

4. A cursor's `apPage[]`/`aiIdx[]` arrays store:
   a) the free page list  b) the descent path from the root and the cell chosen at each level
   c) an LRU list  d) overflow pages

5. `CURSOR_REQUIRESEEK` means:
   a) the cursor is closed  b) the tree changed; the saved key must be re-sought before use
   c) the row was updated in place  d) the cursor is on an interior page

6. `saveAllCursors()` is called:
   a) at commit  b) before a structural change, to protect other cursors
   c) when opening a cursor  d) on rollback only

7. `BTCURSOR_MAX_DEPTH` is:
   a) unlimited  b) 20 — deeper is treated as corruption  c) 4  d) page-size dependent

8. When a cell does not fit on a page, `insertCell()`:
   a) fails with SQLITE_FULL  b) stores it in `pPage->apOvfl[]` and sets `nOverflow`
   c) creates an overflow page  d) splits immediately inside insertCell

9. `balance_quick` applies when:
   a) any leaf overflows  b) one overflow cell, integer-key leaf, rightmost position
   c) the root overflows  d) a delete under-fills a page

10. `balance_deeper` exists because:
    a) deep trees are faster  b) the root page number must not change
    c) it saves memory  d) auto-vacuum requires it

11. `balance_nonroot` redistributes cells across:
    a) exactly 2 pages  b) up to 3 input siblings into up to 5 output pages
    c) all pages of the level  d) only the overflowing page

12. Divider cells in a **table** b-tree:
    a) carry the full payload  b) carry only (child, key)
    c) are copies of the leaf cell  d) do not exist

13. SQLite's minimum page fill invariant is:
    a) 50% like a textbook B-tree  b) none; balance runs only when nFree > usable*2/3 or on overflow
    c) 33% strictly enforced  d) 100%

14. Deleting an entry stored on an interior page:
    a) is impossible  b) rotates the predecessor leaf cell up into the divider slot, then deletes from the leaf
    c) deletes the whole subtree  d) marks it as a tombstone

15. `DELETE FROM t;` with no WHERE clause uses:
    a) row-by-row delete  b) `sqlite3BtreeClearTable` — the truncate optimization
    c) VACUUM  d) DROP + CREATE

16. `sqlite3BtreePayloadFetch()` returns:
    a) a copy of the payload  b) a zero-copy pointer into the page buffer, valid only briefly
    c) an overflow page number  d) the rowid

17. Sequential rowid inserts in real SQLite produce page fill near:
    a) 50%  b) 100%, thanks to `balance_quick`  c) 65%  d) it is random

18. Random primary keys cause:
    a) better fill  b) splits throughout the tree, ~65–75% fill, more I/O
    c) no difference  d) overflow pages

19. `BTREE_APPEND` is:
    a) a required flag  b) a performance hint; correctness must not depend on it
    c) a lock mode  d) a page type

20. `MemPage.nFree == -1` means:
    a) the page is corrupt  b) free space has not been computed yet
    c) the page is full  d) the page is a root

21. A covering index is faster because:
    a) it is stored first  b) it avoids the second b-tree descent into the table
    c) it is always in cache  d) it skips the WAL

22. `KeyInfo` is needed for:
    a) table cursors  b) index cursors — collations and DESC flags for record comparison
    c) overflow pages  d) freelist pages

23. In auto-vacuum mode, `relocatePage()` can fix every pointer to a moved page in O(1) because:
    a) pages are contiguous  b) the pointer map records each page's parent
    c) it scans the file  d) it uses the WAL

24. `assert( CORRUPT_DB || X )` in `btree.c` means:
    a) X is optional  b) X holds unless the database file is corrupt — i.e. X is part of the format spec
    c) debug-only logging  d) X is a performance hint

25. A cursor positioned at the last cell of a leaf, on `Next()`:
    a) follows a sibling pointer  b) walks up to the parent and descends into the next child
    c) returns an error  d) reloads the root

---

## Answers

1. **b** — all payload lives in leaves; interior table pages are pure navigation.
2. **a** — hence enormous fan-out (~600+ children per 4 KiB page).
3. **a** — index interior pages store full keys, so fewer children per page ⇒ deeper.
4. **b** — the cursor *is* the path; that is how `Next()` can climb back up.
5. **b** — the lazy re-seek mechanism that makes read-and-write-same-tree statements safe.
6. **b**.
7. **b**.
8. **b** — a page may be temporarily over-full in memory; `balance()` fixes it right after.
9. **b** — the append fast path; it is why bulk sequential loads are dense and fast.
10. **b** — `sqlite_schema.rootpage` points at the root; so the root's *contents* move down.
11. **b** — NB=3 siblings in, up to 5 pages out.
12. **b** — no payload, which is why table trees are shallow.
13. **b** — deliberately lazy; `VACUUM` is the cleanup path.
14. **b**.
15. **b**.
16. **b** — the pointer is invalidated by the next b-tree operation.
17. **b**.
18. **b**.
19. **b**.
20. **b** — `btreeComputeFreeSpace()` recomputes it.
21. **b** — one tree walk instead of two, and no random row fetches.
22. **b** — table b-trees compare integers and need no KeyInfo.
23. **b** — that is the entire reason ptrmap pages exist.
24. **b** — read those asserts as the file-format specification.
25. **b** — SQLite leaves have no sibling pointers, which keeps splits cheap.
