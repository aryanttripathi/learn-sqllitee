# Phase 1 — Internals Reference: Format Constants, Structs, Functions

## Constant cheat sheet

```
Page size          512 .. 65536, power of 2        (hdr[16..17], value 1 ⇒ 65536)
Usable size U      pagesize - hdr[20]
Page numbers       1-based; 0 == NULL pointer
Endianness         BIG-endian for every on-disk multi-byte integer
Magic              "SQLite format 3\000"  (16 bytes)
Page types         0x02 interior index | 0x05 interior table
                   0x0a leaf index     | 0x0d leaf table
Btree hdr size     8 (leaf) / 12 (interior)
Cell ptr entry     2 bytes, big-endian, sorted by key
Min freeblock      4 bytes (smaller ⇒ fragment, counted in hdr[7])
Defrag trigger     fragmented bytes > 60
Varint             1..9 bytes; 9th byte uses all 8 bits
Max payload local  table leaf: U-35 ; index: ((U-12)*64/255)-23
Min payload local  M = ((U-12)*32/255)-23
Overflow page      4-byte next pointer + (U-4) data bytes
Freelist trunk     4-byte next trunk + 4-byte count L + L × 4-byte leaf page numbers
Ptrmap entry       5 bytes: 1-byte type + 4-byte parent
Ptrmap coverage    U/5 pages per ptrmap page
Lock-byte page     the page containing byte 0x40000000 (1 GiB); unused
Schema root page   always page 1
Schema formats     1 orig | 2 ADD COLUMN | 3 non-NULL defaults | 4 desc idx + serial 8/9
Text encodings     1 UTF-8 | 2 UTF-16le | 3 UTF-16be
```

### Serial types (memorize)
```
0  NULL          1 int8      2 int16     3 int24     4 int32
5  int48         6 int64     7 float64   8 const 0   9 const 1
10,11 reserved   N>=12 even → BLOB len (N-12)/2      N>=13 odd → TEXT len (N-13)/2
```

## Structs (`btreeInt.h`)

```c
struct MemPage {
  u8 isInit;            /* True if previously initialized */
  u8 intKey;            /* True if table b-tree (key = integer rowid) */
  u8 intKeyLeaf;        /* True if leaf of an intKey tree */
  u8 leaf;              /* True if a leaf page */
  u8 hdrOffset;         /* 100 for page 1, else 0 */
  u8 childPtrSize;      /* 0 if leaf, 4 if interior */
  u8 max1bytePayload;
  u16 maxLocal, minLocal;   /* X and M from the spill formula, precomputed */
  u16 cellOffset;       /* offset of the cell pointer array */
  int nFree;            /* bytes of free space on the page (-1 = not computed) */
  u16 nCell;
  u16 maskPage;         /* mask for page offset (pagesize-1) */
  u16 aiOvfl[4]; u8 *apOvfl[4]; u8 nOverflow;   /* cells that didn't fit yet */
  BtShared *pBt;
  u8 *aData;            /* the raw page bytes */
  u8 *aDataEnd, *aCellIdx, *aDataOfst;
  DbPage *pDbPage;      /* pager page handle */
  u32 pgno;
  ...
};

struct CellInfo {
  i64 nKey;        /* rowid for table b-trees; payload size for index b-trees */
  u8 *pPayload;    /* pointer to the start of payload */
  u32 nPayload;    /* total payload size (incl. overflow) */
  u16 nLocal;      /* bytes stored on this page */
  u16 nSize;       /* total cell size on this page */
};
```

`maxLocal`/`minLocal` being precomputed per page is a nice trick: the spill formulas are
evaluated once in `btreeInitPage()`, never in the hot path.

## Functions to read (in this order)

| Function | File | Why |
|---|---|---|
| `zeroPage()` | btree.c | writes a fresh b-tree page header — the format in 20 lines |
| `btreeInitPage()` / `decodeFlags()` | btree.c | parses a page header; computes maxLocal/minLocal |
| `btreeParseCellPtr()` | btree.c | cell → `CellInfo`; note every corruption check |
| `cellSizePtr()` | btree.c | size of a cell (needed by every insert/delete) |
| `btreeComputeFreeSpace()` | btree.c | walks the freeblock list; the canonical free-space definition |
| `allocateSpace()` / `freeSpace()` | btree.c | slotted-page allocator |
| `defragmentPage()` | btree.c | compaction when fragments/holes pile up |
| `accessPayload()` | btree.c | read/write across the overflow chain |
| `allocateBtreePage()` / `freePage2()` | btree.c | freelist management |
| `ptrmapPut()` / `ptrmapGet()` / `ptrmapPageno()` | btree.c | auto-vacuum reverse map |
| `sqlite3BtreeIntegrityCheck()` | btree.c | a machine-checkable restatement of this whole document |
| `sqlite3VdbeSerialType/Put/Get/Len` | vdbe*.c | record encode/decode |
| `sqlite3GetVarint()` / `sqlite3PutVarint()` / `sqlite3GetVarint32()` | util.c | varints |
| `sqlite3InitOne()` | prepare.c | schema bootstrap from page 1 |

## Reading `OP_MakeRecord` and `OP_Column`

`OP_MakeRecord` (in `vdbe.c`) builds the record from registers:
```
 pass 1: for each value, compute serial type + size, accumulate header size
 pass 2: write header varints, then write bodies
```
Watch how it handles the header-size varint chicken-and-egg (header size affects its own
varint length — the code does a fixup when crossing the 127-byte boundary).

`OP_Column` decodes lazily: it parses only as many header entries as needed to reach
column N, and caches the offsets in `VdbeCursor.aType`/`aOffset`. That cache is why
`SELECT last_column` from a wide table is slower than `SELECT first_column` — a real,
measurable effect you can benchmark.

## Invariants (`sqlite3BtreeIntegrityCheck` enforces these)

1. Every page is referenced exactly once: as a b-tree page, an overflow page, a freelist
   page, a ptrmap page, page 1, or the lock-byte page.
2. Cell pointers are in increasing key order and each points inside the content area.
3. The sum of (cell sizes + freeblocks + fragments + gap + header + pointer array) equals
   the usable page size, exactly.
4. Cell content start ≥ end of the cell pointer array.
5. Every child page number is ≤ the database size and ≠ the lock-byte page.
6. Keys in a subtree lie within the bounds implied by the parent's separator keys.
7. On an interior page, nCell ≥ 1 (an interior page with zero cells is corruption).
8. Overflow chains terminate, don't loop, and have the exact length implied by `nPayload`.
9. In auto-vacuum mode, every page's ptrmap entry matches its true parent.

## Gotchas / trivia that show up in bug reports

- `cellTop == 0` means 65536.
- A page can legitimately have `nCell == 0` **only** if it is the root of an empty tree.
- The 100-byte header exists only on page 1, but *page 1 is also a b-tree page* — its
  usable content area starts at 100, which is why `hdrOffset` exists.
- `hdr[28]` (db size in pages) is authoritative **only if** `hdr[92..95] == hdr[24..27]`.
- Freeing a page in auto-vacuum mode requires a ptrmap update ⇒ extra page writes ⇒ that
  is the real cost of auto-vacuum, not the space.
- `PRAGMA secure_delete` changes whether freed cell bytes are zeroed — default OFF.
- Reserved bytes (hdr[20]) are non-zero for SEE/checksum VFS shims and on Apple's system
  SQLite (12 bytes). All formulas must use `usable`, never `pagesize`.
- The lock-byte page is skipped in page numbering — databases larger than 1 GiB have a
  "hole". `sqlite3PagerPagecount` and `ptrmapPageno` both special-case `PENDING_BYTE_PAGE`.
- Serial types 10 and 11 are used internally by `vdbesort.c`/index keys and must never
  appear in a stored record; seeing them is corruption.

## Format-adjacent pragmas
```sql
PRAGMA page_size;            PRAGMA page_count;      PRAGMA freelist_count;
PRAGMA schema_version;       PRAGMA user_version;    PRAGMA application_id;
PRAGMA auto_vacuum;          PRAGMA incremental_vacuum(N);
PRAGMA secure_delete;        PRAGMA encoding;        PRAGMA integrity_check;
PRAGMA quick_check;          PRAGMA cell_size_check; PRAGMA writable_schema;
SELECT * FROM dbstat;        -- per-page stats (needs SQLITE_ENABLE_DBSTAT_VTAB)
SELECT * FROM sqlite_dbpage; -- raw page access vtab (needs SQLITE_ENABLE_DBPAGE_VTAB)
```
`PRAGMA writable_schema=ON` lets you hand-edit `sqlite_schema` — the "break it deliberately"
switch used by `.recover` and by every corruption test.

## Official spec
`https://sqlite.org/fileformat2.html` — the single most useful page on the site.
Keep it open next to `btree.c`; they agree line for line.
