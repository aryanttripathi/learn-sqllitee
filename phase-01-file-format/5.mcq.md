# Phase 1 — MCQ (25 questions)

1. Multi-byte integers in the SQLite file format are stored:
   a) little-endian  b) big-endian  c) native-endian  d) varint-only

2. Header bytes 16–17 contain the value `0x0001`. The page size is:
   a) 1  b) 256  c) 65536  d) invalid

3. "Usable size" equals:
   a) page size − 100  b) page size − hdr[20]  c) page size − 8  d) page size − 35

4. A page whose first byte is `0x05` is:
   a) a leaf table page  b) an interior table page  c) a leaf index page  d) an overflow page

5. The right-most child pointer lives at:
   a) page offset 8 on interior pages  b) page offset 8 on all pages
   c) the end of the cell array  d) header byte 5

6. Cells within a b-tree page grow:
   a) upward from the header  b) downward from the end of the page
   c) in insertion order from offset 0  d) into a separate page

7. The cell pointer array is sorted by:
   a) physical offset  b) key  c) insertion time  d) cell size

8. A gap of 3 free bytes inside the content area is:
   a) a freeblock  b) a fragment counted in header byte 7
   c) an error  d) merged into the gap

9. `defragmentPage()` is triggered when:
   a) nCell > 100  b) fragmented free bytes exceed 60
   c) the page is a leaf  d) a transaction commits

10. A varint encoding of 128 is:
    a) `0x80`  b) `0x81 0x00`  c) `0x00 0x80`  d) `0x82`

11. The maximum number of bytes in a varint is:
    a) 8  b) 9  c) 10  d) 4

12. In a table-leaf cell, the field order is:
    a) rowid, payload size, payload  b) payload size, rowid, payload
    c) payload, rowid  d) child pointer, rowid, payload

13. An interior **table** page cell contains:
    a) child pointer + rowid only  b) child pointer + full payload
    c) rowid + payload  d) payload only

14. Serial type 9 means:
    a) 9-byte integer  b) the integer constant 1 with zero body bytes
    c) TEXT of length 0  d) reserved

15. Serial type 23 means:
    a) BLOB of 5 bytes  b) TEXT of 5 bytes  c) TEXT of 10 bytes  d) invalid

16. For `CREATE TABLE t(id INTEGER PRIMARY KEY, x)`, the `id` column in a stored record is:
    a) serial type 1  b) serial type 0 (NULL) — the value is the cell's rowid
    c) serial type 6 always  d) stored twice

17. Text stored on disk is:
    a) NUL-terminated  b) length-prefixed by the serial type, not terminated
    c) always UTF-16  d) preceded by a 4-byte length

18. For a table leaf page with U = 4096, the maximum local payload X is:
    a) 4061  b) 4088  c) 1000  d) 489

19. An overflow page's first 4 bytes are:
    a) a checksum  b) the page number of the next overflow page (0 = last)
    c) the payload length  d) the parent page number

20. A freelist trunk page contains:
    a) next trunk pointer, leaf count, leaf page numbers
    b) a b-tree header and cells  c) only zeros  d) a ptrmap

21. After `DELETE FROM t`, the file size:
    a) shrinks immediately  b) stays the same; pages move to the freelist
    c) doubles  d) shrinks only in WAL mode

22. Pointer map (ptrmap) pages exist:
    a) always  b) only in auto-vacuum / incremental-vacuum databases
    c) only in WAL mode  d) only for indexes

23. A ptrmap entry is:
    a) 4 bytes  b) 5 bytes: type byte + 4-byte parent  c) 8 bytes  d) a varint pair

24. `sqlite_schema.rootpage` for a VIEW is:
    a) the page of its definition  b) 0 (views have no b-tree)
    c) 1  d) NULL always, even for tables

25. The in-header database size (bytes 28–31) may be trusted only if:
    a) it is non-zero and bytes 92–95 equal bytes 24–27
    b) the file is in WAL mode  c) always  d) the schema cookie is 1

---

## Answers

1. **b** — big-endian, so files are byte-identical across architectures.
2. **c** — `1` is the special encoding for 65536, which doesn't fit in 2 bytes.
3. **b** — reserved bytes are subtracted; Apple's build uses 12, upstream default 0.
4. **b** — 0x05 interior table, 0x0d leaf table, 0x02 interior index, 0x0a leaf index.
5. **a** — only interior pages have the 12-byte header with the right child at offset 8.
6. **b** — slotted page: pointers up from the header, content down from the end.
7. **b** — key order; physical order is arbitrary, which is why `defragmentPage` can freely
   rearrange bodies.
8. **b** — freeblocks need ≥4 bytes for their next/size fields; 1–3 byte holes are fragments.
9. **b** — >60 fragmented bytes (also when `allocateSpace` can't find room).
10. **b** — 7 bits per byte: `0x81 0x00` = `0000001 0000000` = 128.
11. **b** — 9 bytes; the 9th contributes 8 bits for a full 64-bit range.
12. **b** — payload size varint, rowid varint, payload, optional 4-byte overflow pointer.
13. **a** — no payload at all, which is why table b-trees are shallow.
14. **b** — types 8 and 9 encode constants 0 and 1 in zero body bytes (schema format 4+).
15. **b** — odd and ≥13 ⇒ TEXT of (23−13)/2 = 5 bytes.
16. **b** — INTEGER PRIMARY KEY is a rowid alias; storing it twice would be waste.
17. **b** — length comes from the serial type; no terminator.
18. **a** — X = U − 35 = 4061. (489 is M, the minimum local payload.)
19. **b** — next-page pointer; the remaining U−4 bytes are data.
20. **a**.
21. **b** — freed pages are recycled, not returned to the OS; use `VACUUM` or auto_vacuum.
22. **b**.
23. **b** — types: 1 root, 2 freelist, 3 first overflow, 4 later overflow, 5 non-root btree.
24. **b** — views and triggers have rootpage 0.
25. **a** — the version-valid-for dance protects against old libraries that didn't maintain
    the in-header size.
