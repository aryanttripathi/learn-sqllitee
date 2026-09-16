# Phase 1 — The On-Disk File Format, Byte by Byte

This is the phase that makes everything else concrete. After it you can open any `.db`
in a hex editor and read it like source code.

**Golden rule: all multi-byte integers in the file format are BIG-ENDIAN.** (Inside
memory SQLite uses native order; on disk, never.)

---

## 1.1 The file is an array of pages

```
byte 0                                                        EOF
  ├────────┬────────┬────────┬────────┬─────── … ───┬────────┤
  │ page 1 │ page 2 │ page 3 │ page 4 │             │ page N │
  └────────┴────────┴────────┴────────┴─────── … ───┴────────┘
     ^ contains the 100-byte database header
```

- Page size: power of two, **512 … 65536**. Stored at header offset 16 as a 2-byte
  big-endian value; the value `1` means 65536 (because 65536 doesn't fit in 2 bytes).
- Pages are numbered from **1**. Page 0 does not exist; page number 0 is used as a NULL
  pointer throughout the format.
- File size is always an exact multiple of the page size.
- **usable size** = page size − `reserved bytes` (header offset 20, usually 0). Every
  formula in this document uses *usable size*, written `U`.

Page roles:

| Role | Notes |
|---|---|
| b-tree page | table interior/leaf, index interior/leaf |
| overflow page | continuation of a payload too big for one page |
| freelist trunk page | holds pointers to free pages |
| freelist leaf page | content irrelevant (free) |
| pointer map (ptrmap) page | only with auto-vacuum |
| lock-byte page | the page containing byte offset 0x40000000; never used for data |

---

## 1.2 The 100-byte database header (page 1, offset 0)

```
off  size  field
───────────────────────────────────────────────────────────────────────────
  0    16  Magic: "SQLite format 3\0"
 16     2  Page size in bytes (power of 2; value 1 ⇒ 65536)
 18     1  File format WRITE version  (1=legacy/rollback journal, 2=WAL)
 19     1  File format READ  version  (1=legacy, 2=WAL)
 20     1  Bytes of unused "reserved" space at end of every page (usually 0)
 21     1  Max embedded payload fraction. MUST be 64
 22     1  Min embedded payload fraction. MUST be 32
 23     1  Leaf payload fraction.         MUST be 32
 24     4  File change counter
 28     4  Size of the database file in pages ("in-header database size")
 32     4  Page number of first freelist trunk page (0 = none)
 36     4  Total number of freelist pages
 40     4  Schema cookie (bumped on every schema change)
 44     4  Schema format number (1,2,3,4)
 48     4  Default page cache size (PRAGMA default_cache_size)
 52     4  Page number of largest root b-tree page when auto/incremental-vacuum, else 0
 56     4  Text encoding: 1=UTF-8, 2=UTF-16le, 3=UTF-16be
 60     4  User version (PRAGMA user_version)
 64     4  Incremental-vacuum mode flag (non-zero ⇒ incremental)
 68     4  Application ID (PRAGMA application_id)
 72    20  Reserved. Must be zero.
 92     4  version-valid-for number (change counter value when 96..99 was written)
 96     4  SQLITE_VERSION_NUMBER of the library that last wrote
```

Things that trip people up:

- **Bytes 21/22/23 are fossils.** They are fixed constants; the code asserts them.
- **File change counter (24) + version-valid-for (92)**: a reader that has the file
  mmap'd/cached uses the change counter to detect "someone else wrote this". In WAL mode
  the change counter is *not* incremented per transaction — WAL has its own mechanism.
- **In-header database size (28)** is only trusted if it is non-zero **and** bytes 92–95
  equal bytes 24–27. Otherwise SQLite falls back to the actual file size. This is the
  "legacy file" compatibility dance.
- **Schema format numbers**: 1 = original; 2 = `ALTER TABLE ADD COLUMN` supported (3.1.3);
  3 = added columns may have non-NULL defaults (3.1.4); 4 = descending indexes, and serial
  types 8 and 9 allowed (3.3.0). Writing format 4 makes a file unreadable by pre-2006
  libraries, which is why `PRAGMA legacy_file_format` existed.

```
   Reading the header, visually:

   00000000  53 51 4c 69 74 65 20 66  6f 72 6d 61 74 20 33 00  |SQLite format 3.|
   00000010  10 00 01 01 00 40 20 20  00 00 00 02 00 00 00 03  |.....@  ........|
             ^^^^^ pagesize=0x1000=4096       ^^^^^^^^^^^ change counter = 2
                   ^^ write=1  ^^ read=1              ^^^^^^^^^^^ db size = 3 pages
                         ^^ reserved=0
                            ^^ 0x40=64  ^^ 0x20=32  ^^ 0x20=32
```

---

## 1.3 B-tree page layout

Every b-tree page (page 1 included, just offset by 100) has this shape:

```
 ┌─────────────────────────────────────────────────────────────┐
 │ (page 1 only) 100-byte database header                      │
 ├─────────────────────────────────────────────────────────────┤
 │ b-tree page header:  8 bytes (leaf) or 12 bytes (interior)  │
 ├─────────────────────────────────────────────────────────────┤
 │ cell pointer array: nCell × 2-byte offsets, KEY-SORTED      │
 ├─────────────────────────────────────────────────────────────┤
 │                                                             │
 │              unallocated space (the "gap")                  │
 │                                                             │
 ├─────────────────────────────────────────────────────────────┤
 │ cell content area — cells grow DOWNWARD from end of page    │
 │   cell N ... cell 2, cell 1  (physical order is arbitrary)  │
 ├─────────────────────────────────────────────────────────────┤
 │ reserved region (usually 0 bytes)                           │
 └─────────────────────────────────────────────────────────────┘
```

**Two arrays growing toward each other**: pointers from the top, cell bodies from the
bottom. The gap between them is the free space. This is the classic *slotted page*.

### B-tree page header

```
off size  meaning
 0    1   page type: 0x02 interior index, 0x05 interior table,
                     0x0a leaf index,     0x0d leaf table
 1    2   byte offset of the first freeblock (0 = none)
 3    2   number of cells on this page
 5    2   start of the cell content area (0 means 65536)
 7    1   number of fragmented free bytes within the content area
 8    4   ONLY on interior pages: right-most child page number
```

Mnemonic for the type byte: bit `0x08` = leaf, bit `0x04` = table, bit `0x02` = index,
bit `0x01` = "has key data"… informally: 2/10 are index (interior/leaf), 5/13 are table.

```
     0x02 = 0b00000010   interior index
     0x05 = 0b00000101   interior table
     0x0a = 0b00001010   leaf index      (0x08 leaf-bit set)
     0x0d = 0b00001101   leaf table      (0x08 leaf-bit set)
```

### Free space inside a page

Three kinds of free bytes, and you must not confuse them:

1. **The gap** — between the end of the cell pointer array and the content area start.
2. **Freeblocks** — a singly-linked list of holes ≥ 4 bytes inside the content area.
   Each freeblock: `2-byte next freeblock offset` + `2-byte size of this block`.
3. **Fragments** — holes of 1–3 bytes, too small to link. Total counted in header byte 7.
   If fragments exceed 60 bytes, `defragmentPage()` is triggered.

```
 content area:
   ┌────────┬───────────────┬────────┬─────┬────────────────┬──────┐
   │ cell C │  freeblock    │ cell B │ frag│     cell A     │ ...  │
   └────────┴───────────────┴────────┴─────┴────────────────┴──────┘
               next|size                     ↑ 2 wasted bytes counted in byte 7
```

---

## 1.4 Cells: the four formats

```
TABLE LEAF (0x0d)   — the actual rows
 ┌──────────────┬──────────────┬───────────────────┬────────────────────┐
 │ varint: P    │ varint: rowid│ payload (first K) │ 4-byte overflow pg │
 │ total payload│   (the key)  │                   │  (only if P > K)   │
 └──────────────┴──────────────┴───────────────────┴────────────────────┘

TABLE INTERIOR (0x05) — pure navigation, NO payload
 ┌─────────────────────┬────────────────┐
 │ 4-byte left child   │ varint: rowid  │   "keys ≤ rowid live in left child"
 └─────────────────────┴────────────────┘

INDEX LEAF (0x0a)
 ┌──────────────┬───────────────────┬────────────────────┐
 │ varint: P    │ payload (key cols │ 4-byte overflow pg │
 │              │  + rowid appended)│                    │
 └──────────────┴───────────────────┴────────────────────┘

INDEX INTERIOR (0x02)
 ┌───────────────────┬───────────┬──────────────┬─────────────────┐
 │ 4-byte left child │ varint: P │   payload    │ 4-byte overflow │
 └───────────────────┴───────────┴──────────────┴─────────────────┘
```

Note the asymmetry that matters for performance: **table interior pages carry no payload**,
so a single interior page holds hundreds of (child, rowid) pairs ⇒ table b-trees are very
shallow. Index interior pages *do* carry keys, so index trees are deeper.

### Interior page navigation model

```
                 interior table page (0x05), nCell = 3
   ┌──────────────────────────────────────────────────────────┐
   │ cell0:(child=7 , key=10)  cell1:(child=9, key=20)        │
   │ cell2:(child=11, key=30)          right-most child = 14  │
   └──────────────────────────────────────────────────────────┘
        │                │                │              │
     rowids ≤10      11..20           21..30          ≥31
      page 7          page 9          page 11        page 14
```

Key rule: cell *i*'s child holds keys **≤** cell *i*'s key; the right-most pointer holds
keys greater than the last cell's key.

---

## 1.5 Varints — the 1–9 byte integer

Big-endian base-128. Each byte contributes 7 bits; the high bit means "more bytes follow".
**Exception:** if you reach a 9th byte, all 8 of its bits are used (7×8 + 8 = 64 bits).

```
 value 0x00000000000000C8 (200):
   byte0: 1 0000001   (high bit set → continue, 7 bits = 0000001)
   byte1: 0 1001000   (high bit clear → stop,   7 bits = 1001000)
   => 0000001 1001000 = 0xC8 = 200          (2 bytes)

 value 127  -> 0x7F                          (1 byte)
 value 128  -> 0x81 0x00                     (2 bytes)
 value -1   -> 0xFF×8 + 0xFF  = 9 bytes (two's complement, always maximal)
```

Reference decoder (this is essentially `sqlite3GetVarint()` in `util.c`, simplified):

```c
/* Decode a varint at p; store value in *v; return bytes consumed (1..9). */
static int getVarint(const unsigned char *p, sqlite3_uint64 *v){
  sqlite3_uint64 x = 0;
  int i;
  for(i=0; i<8; i++){
    x = (x << 7) | (p[i] & 0x7f);
    if( (p[i] & 0x80)==0 ){ *v = x; return i+1; }
  }
  x = (x << 8) | p[8];          /* 9th byte contributes all 8 bits */
  *v = x;
  return 9;
}
```

The real implementation is unrolled by hand for the 1- and 2-byte cases because varint
decoding is one of the hottest paths in the entire library.

---

## 1.6 The record format (a.k.a. "the row")

A payload is a **record**: a header of serial-type codes, then the values.

```
 ┌───────────────────────────────────────────┬─────────────────────────────┐
 │ HEADER                                    │ BODY                        │
 │ varint: header size (incl. itself)        │ value1 value2 value3 ...    │
 │ varint: serial type col1                  │ (raw bytes, no separators)  │
 │ varint: serial type col2 ...              │                             │
 └───────────────────────────────────────────┴─────────────────────────────┘
```

### Serial type codes

| Code | Size | Meaning |
|---|---|---|
| 0 | 0 | NULL |
| 1 | 1 | big-endian signed int |
| 2 | 2 | big-endian signed int |
| 3 | 3 | big-endian signed int |
| 4 | 4 | big-endian signed int |
| 5 | 6 | big-endian signed int |
| 6 | 8 | big-endian signed int |
| 7 | 8 | IEEE-754 double, big-endian |
| 8 | 0 | integer constant **0** (schema format ≥ 4) |
| 9 | 0 | integer constant **1** (schema format ≥ 4) |
| 10, 11 | — | reserved / internal use only |
| N ≥ 12, even | (N−12)/2 | BLOB of that length |
| N ≥ 13, odd | (N−13)/2 | TEXT of that length (in the db's encoding, **not** NUL-terminated) |

Two elegant consequences:
- Storing `0` or `1` costs **zero body bytes** — great for boolean columns.
- Integers are stored in the smallest width that fits, per row. A column is not a fixed
  width; `age INT` costs 1 byte for 36 and 8 bytes for 2^40.

### Worked example

```sql
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);
INSERT INTO users VALUES (1, 'ada', 36);
```

The record stored in the table b-tree is:

```
 header size = 4
 serial types: [0]  (id → NULL, because INTEGER PRIMARY KEY is an alias for the rowid!)
               [19] (text of length (19-13)/2 = 3  → 'ada')
               [1]  (1-byte int → 36)

 bytes:  04 00 13 01 | 61 64 61 | 24
         ^^ hdrsize
            ^^ NULL
               ^^ 0x13 = 19 → TEXT(3)
                  ^^ int8
                       'a''d''a'  ^^ 0x24 = 36
 and the cell is:  varint(P=8) varint(rowid=1) <the 8 bytes above>
   => 08 01 04 00 13 01 61 64 61 24
```

**This is the single most important example in Phase 1.** Reproduce it in a hexdump
yourself (Lab 1.3). The `id` column being stored as NULL is the classic "wait, what?"
moment: the value lives in the cell's rowid field, not in the record.

### Index records

An index entry's payload is: **indexed columns, then the rowid appended** (for rowid
tables). There is no separate "value"; index b-trees store the key only.

```sql
CREATE INDEX i_age ON users(age);
-- entry for (age=36, rowid=1) has record: serial types [1][1], body: 0x24 0x01
```

For `WITHOUT ROWID` tables, the table itself *is* an index b-tree keyed by the PRIMARY KEY,
and the remaining columns are stored in the same record after the key columns.

---

## 1.7 Overflow pages

If the payload doesn't fit in a page, the tail spills into a chain of overflow pages.

```
  cell on leaf page                   overflow page chain
 ┌────────────────────────┐          ┌───────────────────────────┐
 │ ... first K bytes ...  │   ┌─────►│ 4-byte next page number   │
 │ 4-byte first overflow ─┼───┘      │ U-4 bytes of payload      │
 └────────────────────────┘          └────────────┬──────────────┘
                                                  │ next (0 = end)
                                                  ▼
                                     ┌───────────────────────────┐
                                     │ 4-byte next (0)           │
                                     │ remaining payload         │
                                     └───────────────────────────┘
```

**The spill formulas** (memorize the shape, not the constants):

```
U = usable page size
X = maximum payload stored locally
     table leaf   :  X = U - 35
     index pages  :  X = ((U - 12) * 64 / 255) - 23
M = minimum payload stored locally = ((U - 12) * 32 / 255) - 23
K = M + ((P - M) % (U - 4))

if P <= X            → store all P bytes locally
else if K <= X       → store K bytes locally, rest overflows
else                 → store M bytes locally, rest overflows
```

Why the weird `K`? To avoid an overflow page that holds only a few bytes: the modulo
chooses a local size such that the last overflow page is as full as possible. And the
`M` floor guarantees every cell keeps a minimum locally so interior pages keep a
reasonable fan-out.

With `U = 4096`: table leaf `X = 4061`, `M = 489`. So a 5000-byte row stores
`K = 489 + ((5000-489) % 4092) = 489 + 419 = 908`… since `908 ≤ 4061`, 908 bytes stay
local and 4092 go to one overflow page.

---

## 1.8 The freelist

Deleted pages go on a freelist, a linked list of **trunk** pages, each holding an array of
**leaf** page numbers.

```
header[32] = first trunk page
 ┌────────────────────────────┐
 │ TRUNK page                 │
 │  4 bytes: next trunk (0=∅) │
 │  4 bytes: L = #leaves      │
 │  4×L bytes: leaf page nums │──► those pages are free, contents ignored
 └─────────────┬──────────────┘
               ▼ next trunk
 ┌────────────────────────────┐
 │ TRUNK page …               │
 └────────────────────────────┘

header[36] = total free page count (trunks + leaves)
```

Allocation prefers a leaf from the first trunk (LIFO-ish); if the freelist is empty, the
file grows by one page. This is why `DELETE` does not shrink the file — you need `VACUUM`
(rebuild) or `PRAGMA auto_vacuum`.

`PRAGMA secure_delete=ON` zeroes freed content; by default deleted bytes remain readable
in the file. Forensically important.

---

## 1.9 Pointer map pages (auto-vacuum only)

To move a page during auto-vacuum you must update whoever points at it. Scanning is too
slow, so auto-vacuum databases keep a reverse index: **ptrmap pages**.

```
page 1  : header + sqlite_schema root
page 2  : ptrmap page  ← covers pages 3 .. 3+(U/5)-1
page 3..: data
... next ptrmap page appears every (U/5)+1 pages
```

Each entry is 5 bytes: `1-byte type` + `4-byte parent page number`.

| type | meaning |
|---|---|
| 1 | root page of a b-tree (no parent) |
| 2 | freelist page |
| 3 | first page of an overflow chain (parent = page holding the cell) |
| 4 | subsequent overflow page (parent = previous overflow page) |
| 5 | non-root b-tree page (parent = its b-tree parent) |

Cost: ~0.2% space and extra writes on every page allocation. Benefit: `PRAGMA
incremental_vacuum` can return free pages to the OS without a full rebuild.

---

## 1.10 The schema table

Page 1 is always the root of `sqlite_schema` (historically `sqlite_master`):

```sql
CREATE TABLE sqlite_schema(
  type text,       -- 'table' | 'index' | 'view' | 'trigger'
  name text,       -- object name
  tbl_name text,   -- table it belongs to
  rootpage integer,-- page number of its b-tree root (0/NULL for view & trigger)
  sql text         -- the original CREATE statement text
);
```

**Bootstrapping is the beautiful part:** to read the schema, SQLite needs a table
definition; the definition of `sqlite_schema` is hard-coded in C, its root page is
hard-coded to 1, and everything else is then read *through* normal b-tree machinery.
`sqlite3InitOne()` in `prepare.c` literally runs `SELECT ... FROM sqlite_schema` and
re-parses each `sql` string to build in-memory `Table`/`Index` objects.

Internal tables you may meet: `sqlite_sequence` (AUTOINCREMENT), `sqlite_stat1` /
`sqlite_stat4` (ANALYZE), `sqlite_autoindex_<table>_<N>` (implicit UNIQUE indexes).

---

## 1.11 Complete picture of a small database

```sql
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);
CREATE INDEX i_age ON users(age);
INSERT INTO users VALUES (1,'ada',36),(2,'linus',54);
```

```
 page 1  ┌─────────────────────────────────────────────────────┐
         │ 100-byte db header                                  │
         │ btree hdr 0x0d (leaf table)  nCell=2                │
         │ cell ptrs → two sqlite_schema rows                  │
         │  row: ('table','users','users',2,'CREATE TABLE …')  │
         │  row: ('index','i_age','users',3,'CREATE INDEX …')  │
         └─────────────────────────────────────────────────────┘
 page 2  ┌─────────────────────────────────────────────────────┐
         │ btree hdr 0x0d (leaf table) nCell=2   users data    │
         │  cell(rowid=1): 04 00 13 01 'ada'  0x24             │
         │  cell(rowid=2): 04 00 17 01 'linus' 0x36            │
         └─────────────────────────────────────────────────────┘
 page 3  ┌─────────────────────────────────────────────────────┐
         │ btree hdr 0x0a (leaf index) nCell=2   i_age data    │
         │  key (36,1), key (54,2)   ← sorted by age           │
         └─────────────────────────────────────────────────────┘
```

Everything else in SQLite — the pager, the b-tree balancer, the VDBE — exists to
manipulate exactly this structure without ever leaving it inconsistent.

---

## 1.12 Where this format is implemented in C

| Concern | Code |
|---|---|
| Header read/write | `btree.c`: `lockBtree()`, `sqlite3BtreeUpdateMeta()`, `zeroPage()` |
| Page header parse | `btree.c`: `btreeInitPage()`, `decodeFlags()` |
| Cell parse | `btree.c`: `btreeParseCell()`, `btreeParseCellPtr()`, `cellSizePtr()` |
| Payload/overflow | `btree.c`: `accessPayload()`, `fetchPayload()`, `ptrmapPutOvflPtr()` |
| Varints | `util.c`: `sqlite3PutVarint()`, `sqlite3GetVarint()`, `sqlite3GetVarint32()` |
| Record build | `vdbe.c` `OP_MakeRecord`, `sqlite3VdbeSerialType()`, `sqlite3VdbeSerialPut()` |
| Record read | `vdbe.c` `OP_Column`, `sqlite3VdbeSerialGet()` |
| Freelist | `btree.c`: `allocateBtreePage()`, `freePage2()` |
| Ptrmap | `btree.c`: `ptrmapPut()`, `ptrmapGet()`, `ptrmapPageno()` |
| Integrity check | `btree.c`: `sqlite3BtreeIntegrityCheck()` — reads like a format spec |
