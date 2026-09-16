# Phase 1 — Implementation Labs: Read the Bytes Yourself

Deliverables in `labs/phase01/`. The centerpiece is **Lab 1.5: write your own SQLite file
parser in C**. Do not skip it — it is the single highest-leverage exercise in this course.

---

## Lab 1.1 — Make a database and stare at the header

```sh
mkdir -p ~/Desktop/sqllite/labs/phase01 && cd ~/Desktop/sqllite/labs/phase01
sqlite3 mini.db <<'SQL'
PRAGMA page_size=4096;
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);
INSERT INTO users VALUES (1,'ada',36),(2,'linus',54),(3,'grace',45);
SQL
xxd -l 100 mini.db
```

Fill in this table by hand in `labs/phase01/header-decode.md` (no tools, read the hex):

| offset | bytes | decoded value | meaning |
|---|---|---|---|
| 0..15 | | | magic |
| 16..17 | | | page size |
| 18 | | | write version |
| 19 | | | read version |
| 20 | | | reserved |
| 24..27 | | | change counter |
| 28..31 | | | db size in pages |
| 32..35 | | | first freelist trunk |
| 40..43 | | | schema cookie |
| 44..47 | | | schema format |
| 56..59 | | | text encoding |
| 96..99 | | | writing library version |

Cross-check with pragmas:
```sh
sqlite3 mini.db 'PRAGMA page_size; PRAGMA schema_version; PRAGMA freelist_count; PRAGMA encoding; PRAGMA page_count;'
```

---

## Lab 1.2 — Find page 2 and decode its b-tree header

```sh
# page 2 starts at byte 4096
xxd -s 4096 -l 64 mini.db
```

Decode: type byte, first freeblock, nCell, cell content start, fragmented bytes. Then read
the cell pointer array (nCell × 2 bytes right after the 8-byte header) and jump to each
offset.

```sh
# example: if the first cell pointer is 0x0FD9 = 4057, the cell begins at 4096+4057
xxd -s $((4096+4057)) -l 24 mini.db
```

---

## Lab 1.3 — Decode one record by hand

Take the first cell of page 2 and write out, in `labs/phase01/record-decode.md`:

```
varint P      = ?        total payload bytes
varint rowid  = ?
header size   = ?
serial types  = [?, ?, ?]
body bytes    = ?
reconstructed row = (?, '?', ?)
```

Expected for `(1,'ada',36)`:
```
08 01 04 00 13 01 61 64 61 24
│  │  │  │  │  │  └──────┴── 'ada' then 0x24=36
│  │  │  │  │  └─ serial 1  → 1-byte int
│  │  │  │  └──── serial 0x13=19 → text len (19-13)/2 = 3
│  │  │  └─────── serial 0 → NULL  (id is the rowid alias!)
│  │  └────────── header size = 4
│  └───────────── rowid = 1
└──────────────── payload size = 8
```

**Checkpoint question:** why is `id` NULL in the record even though the row has `id=1`?

---

## Lab 1.4 — Prove the integer-width claim

```sh
sqlite3 sizes.db <<'SQL'
CREATE TABLE t(v);
INSERT INTO t VALUES (0),(1),(2),(1000),(100000),(10000000000),(3.14),('hi'),(x'deadbeef'),(NULL);
SELECT rowid, typeof(v), length(hex(v))/2 FROM t;
SQL
xxd -s 4096 -l 200 sizes.db
```

Find the records for `0` and `1`: they should use serial types **8** and **9** with a
zero-length body. Confirm `1000` uses serial type 2 (2 bytes) and `10000000000` uses type 6
(8 bytes).

Record findings in `labs/phase01/serial-types.md`.

---

## Lab 1.5 — ⭐ Write `dbparse.c`: your own SQLite file reader

Goal: a standalone C program (no linking against SQLite) that prints the header, walks the
b-tree of a table, and decodes every record. ~300 lines. This is the exercise that converts
"I read the spec" into "I understand the format".

Save as `labs/phase01/dbparse.c`:

```c
/* dbparse.c — a minimal, dependency-free SQLite database file reader.
 * Build: cc -Wall -O0 -g -o dbparse dbparse.c
 * Usage: ./dbparse file.db            (dumps header + every b-tree page)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *g_file;      /* whole database in memory */
static long           g_size;
static int            g_pagesz;
static int            g_usable;

/* ---------- big-endian readers ---------- */
static uint32_t be16(const unsigned char *p){ return (p[0]<<8) | p[1]; }
static uint32_t be32(const unsigned char *p){
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* ---------- varint ---------- */
static int getVarint(const unsigned char *p, uint64_t *v){
  uint64_t x = 0;
  int i;
  for(i=0; i<8; i++){
    x = (x<<7) | (p[i] & 0x7f);
    if( (p[i] & 0x80)==0 ){ *v = x; return i+1; }
  }
  x = (x<<8) | p[8];
  *v = x;
  return 9;
}

/* ---------- page access (1-based page numbers) ---------- */
static unsigned char *page(int pgno){
  return g_file + (long)(pgno-1) * g_pagesz;
}

/* ---------- serial type helpers ---------- */
static int serialSize(uint64_t t){
  static const int sz[] = {0,1,2,3,4,6,8,8,0,0};
  if( t<10 ) return sz[t];
  return (int)((t - 12 - (t&1)) / 2);
}

static void printValue(uint64_t stype, const unsigned char *p){
  int n = serialSize(stype);
  int i;
  int64_t v = 0;
  switch( stype ){
    case 0: printf("NULL"); return;
    case 8: printf("0");    return;
    case 9: printf("1");    return;
    case 7: {                       /* IEEE754 big-endian double */
      uint64_t u = 0;
      double d;
      for(i=0;i<8;i++) u = (u<<8) | p[i];
      memcpy(&d, &u, 8);
      printf("%g", d);
      return;
    }
    default:
      if( stype>=1 && stype<=6 ){
        v = (p[0] & 0x80) ? -1 : 0;           /* sign extend */
        for(i=0;i<n;i++) v = (v<<8) | p[i];
        printf("%lld", (long long)v);
        return;
      }
      if( stype>=12 && (stype%2)==0 ){        /* BLOB */
        printf("x'");
        for(i=0;i<n;i++) printf("%02x", p[i]);
        printf("'");
        return;
      }
      printf("'%.*s'", n, (const char*)p);    /* TEXT (assumes UTF-8) */
  }
}

/* ---------- record decoder ---------- */
static void printRecord(const unsigned char *rec, int nRec){
  uint64_t hdrSize;
  int n = getVarint(rec, &hdrSize);
  const unsigned char *pHdr = rec + n;
  const unsigned char *pEnd = rec + hdrSize;
  const unsigned char *pBody = rec + hdrSize;
  int col = 0;
  (void)nRec;
  printf("(");
  while( pHdr < pEnd ){
    uint64_t stype;
    pHdr += getVarint(pHdr, &stype);
    if( col++ ) printf(", ");
    printValue(stype, pBody);
    pBody += serialSize(stype);
  }
  printf(")");
}

/* ---------- local payload size (spill formula) ---------- */
static int localPayload(uint64_t P, int isTableLeaf){
  int U = g_usable;
  int X = isTableLeaf ? (U - 35) : (((U-12)*64/255) - 23);
  int M = ((U-12)*32/255) - 23;
  int K;
  if( (int64_t)P <= X ) return (int)P;
  K = M + (int)((P - M) % (U - 4));
  return (K <= X) ? K : M;
}

/* ---------- b-tree walker ---------- */
static void walkPage(int pgno, int depth){
  unsigned char *pg = page(pgno);
  int hdrOff = (pgno==1) ? 100 : 0;
  unsigned char *h = pg + hdrOff;
  int type    = h[0];
  int nCell   = be16(h+3);
  int cellTop = be16(h+5);
  int nFrag   = h[7];
  int isLeaf  = (type==0x0a || type==0x0d);
  int cellHdr = isLeaf ? 8 : 12;
  unsigned char *ptrs = h + cellHdr;
  int i;
  const char *tname =
      type==0x02 ? "interior index" :
      type==0x05 ? "interior table" :
      type==0x0a ? "leaf index"     :
      type==0x0d ? "leaf table"     : "??? NOT A BTREE PAGE";

  printf("%*spage %d: %s  nCell=%d cellTop=%d frag=%d",
         depth*2, "", pgno, tname, nCell, cellTop?cellTop:65536, nFrag);
  if( !isLeaf ) printf(" rightChild=%u", be32(h+8));
  printf("\n");
  if( type!=0x02 && type!=0x05 && type!=0x0a && type!=0x0d ) return;

  for(i=0; i<nCell; i++){
    int off = be16(ptrs + i*2);
    unsigned char *c = pg + off;
    uint64_t P, rowid;
    int nv, local;

    if( type==0x05 ){                       /* interior table: child + key */
      uint32_t child = be32(c);
      getVarint(c+4, &rowid);
      printf("%*s  cell[%d] child=%u key<=%llu\n",
             depth*2, "", i, child, (unsigned long long)rowid);
      walkPage((int)child, depth+1);
      continue;
    }
    if( type==0x02 ){                       /* interior index */
      uint32_t child = be32(c);
      nv = getVarint(c+4, &P);
      local = localPayload(P, 0);
      printf("%*s  cell[%d] child=%u P=%llu ", depth*2, "", i, child,
             (unsigned long long)P);
      printRecord(c+4+nv, local);
      printf("%s\n", (int64_t)P>local ? "  [+overflow]" : "");
      walkPage((int)child, depth+1);
      continue;
    }
    if( type==0x0d ){                       /* leaf table: P, rowid, payload */
      nv  = getVarint(c, &P);
      nv += getVarint(c+nv, &rowid);
      local = localPayload(P, 1);
      printf("%*s  cell[%d] off=%d P=%llu rowid=%llu ",
             depth*2, "", i, off, (unsigned long long)P,
             (unsigned long long)rowid);
      printRecord(c+nv, local);
      if( (int64_t)P > local ){
        printf("  [overflow -> page %u]", be32(c+nv+local));
      }
      printf("\n");
    } else {                                /* 0x0a leaf index */
      nv = getVarint(c, &P);
      local = localPayload(P, 0);
      printf("%*s  cell[%d] off=%d P=%llu key=", depth*2, "", i, off,
             (unsigned long long)P);
      printRecord(c+nv, local);
      printf("%s\n", (int64_t)P>local ? "  [+overflow]" : "");
    }
  }
  if( !isLeaf ) walkPage((int)be32(h+8), depth+1);
}

static void dumpHeader(void){
  unsigned char *h = g_file;
  printf("magic          : %.15s\n", h);
  printf("page size      : %d\n", g_pagesz);
  printf("write/read ver : %d / %d  (%s)\n", h[18], h[19],
         h[18]==2 ? "WAL" : "rollback journal");
  printf("reserved bytes : %d   (usable = %d)\n", h[20], g_usable);
  printf("change counter : %u\n", be32(h+24));
  printf("db size (pages): %u   (file has %ld)\n", be32(h+28), g_size/g_pagesz);
  printf("freelist trunk : %u   count=%u\n", be32(h+32), be32(h+36));
  printf("schema cookie  : %u   format=%u\n", be32(h+40), be32(h+44));
  printf("largest root pg: %u   (auto-vacuum if non-zero)\n", be32(h+52));
  printf("text encoding  : %u   (1=utf8 2=utf16le 3=utf16be)\n", be32(h+56));
  printf("user_version   : %u\n", be32(h+60));
  printf("incr-vacuum    : %u\n", be32(h+64));
  printf("application_id : %u\n", be32(h+68));
  printf("version-valid  : %u\n", be32(h+92));
  printf("written by lib : %u\n", be32(h+96));
  printf("\n");
}

int main(int argc, char **argv){
  FILE *f;
  int rootpg = 1;
  if( argc<2 ){ fprintf(stderr,"usage: %s file.db [rootpage]\n", argv[0]); return 1; }
  if( argc>2 ) rootpg = atoi(argv[2]);

  f = fopen(argv[1], "rb");
  if( !f ){ perror("open"); return 1; }
  fseek(f, 0, SEEK_END); g_size = ftell(f); fseek(f, 0, SEEK_SET);
  g_file = malloc(g_size);
  if( fread(g_file, 1, g_size, f)!=(size_t)g_size ){ perror("read"); return 1; }
  fclose(f);

  g_pagesz = be16(g_file+16);
  if( g_pagesz==1 ) g_pagesz = 65536;
  g_usable = g_pagesz - g_file[20];

  dumpHeader();
  printf("=== walking b-tree rooted at page %d ===\n", rootpg);
  walkPage(rootpg, 0);
  return 0;
}
```

Build and run:

```sh
cd ~/Desktop/sqllite/labs/phase01
cc -Wall -O0 -g -o dbparse dbparse.c
./dbparse mini.db            # page 1 = sqlite_schema
./dbparse mini.db 2          # the users table
```

**Extensions (do at least two):**
1. Walk the **freelist** from header[32] and print every free page number.
2. Follow an **overflow chain** and reassemble the full payload.
3. Detect and decode **ptrmap** pages in an auto-vacuum database.
4. Print per-page free-space stats (gap + freeblocks + fragments) and compare against
   `sqlite3_analyzer`.
5. Add `-t <tablename>`: read `sqlite_schema` from page 1, find the table's rootpage, and
   dump just that table.

---

## Lab 1.6 — Overflow pages for real

```sh
cd ~/Desktop/sqllite/labs/phase01
sqlite3 big.db <<'SQL'
PRAGMA page_size=4096;
CREATE TABLE docs(id INTEGER PRIMARY KEY, body TEXT);
INSERT INTO docs VALUES (1, hex(randomblob(3000)));   -- 6000 chars → overflows
INSERT INTO docs VALUES (2, 'small');
SQL
./dbparse big.db 2
sqlite3 big.db 'PRAGMA page_count;'
```

Compute by hand what the local payload size should be (`U=4096`, table leaf) and check that
your parser agrees. Write the arithmetic in `labs/phase01/overflow-math.md`.

---

## Lab 1.7 — Freelist behaviour

```sh
sqlite3 free.db <<'SQL'
PRAGMA page_size=4096;
CREATE TABLE t(a,b);
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<20000)
INSERT INTO t SELECT i, randomblob(100) FROM c;
SQL
ls -l free.db; sqlite3 free.db 'PRAGMA page_count; PRAGMA freelist_count;'

sqlite3 free.db 'DELETE FROM t WHERE a % 2 = 0;'
ls -l free.db; sqlite3 free.db 'PRAGMA page_count; PRAGMA freelist_count;'   # file same size!

sqlite3 free.db 'VACUUM;'
ls -l free.db; sqlite3 free.db 'PRAGMA page_count; PRAGMA freelist_count;'   # now it shrank
```

Then repeat with `PRAGMA auto_vacuum=FULL` set *before* creating the table and observe the
difference. Note in `labs/phase01/freelist-notes.md`: why must auto_vacuum be set before
the first table is created (or require a VACUUM to change)?

---

## Lab 1.8 — Deleted data is still there (secure_delete)

```sh
sqlite3 secrets.db "CREATE TABLE s(x); INSERT INTO s VALUES('SUPER_SECRET_TOKEN_42'); DELETE FROM s;"
strings secrets.db | grep SUPER      # ← still present

sqlite3 secrets2.db "PRAGMA secure_delete=ON; CREATE TABLE s(x); INSERT INTO s VALUES('SUPER_SECRET_TOKEN_42'); DELETE FROM s;"
strings secrets2.db | grep SUPER     # ← gone
```

This is a real-world security finding you can explain to any team.

---

## Lab 1.9 — Use the official tools to check your work

```sh
cd ~/Desktop/sqllite/sqlite-src
cc -I. -Isrc -o /tmp/showdb tool/showdb.c 2>/dev/null || \
  (cd build && make showdb)

/tmp/showdb ~/Desktop/sqllite/labs/phase01/mini.db dbheader
/tmp/showdb ~/Desktop/sqllite/labs/phase01/mini.db 2
/tmp/showdb ~/Desktop/sqllite/labs/phase01/mini.db 2b     # page 2, decoded cells
```

Also:
```sh
sqlite3 mini.db 'PRAGMA integrity_check;'
sqlite3 mini.db 'SELECT * FROM dbstat;'       # needs SQLITE_ENABLE_DBSTAT_VTAB build
```

`dbstat` gives you per-page payload/unused stats — your parser's output should match it.

---

## Lab 1.10 — Corrupt a database on purpose, then diagnose

```sh
cp mini.db broken.db
# flip the page-type byte of page 2 (offset 4096) from 0x0d to 0x0c
printf '\x0c' | dd of=broken.db bs=1 seek=4096 count=1 conv=notrunc
sqlite3 broken.db 'PRAGMA integrity_check; SELECT * FROM users;'
```

Then try other single-byte corruptions and classify the error message:
- nCell (offset 4096+3..4) set absurdly high
- cell content start pointing into the header
- a cell pointer pointing past the end of the page
- change counter (offset 24) mismatched with version-valid-for (92)

Record in `labs/phase01/corruption-lab.md`: which corruptions are caught by
`integrity_check`, which by `PRAGMA quick_check`, and which produce
`SQLITE_CORRUPT` only when the bad page is touched. This exercise teaches you how
defensive `btree.c` actually is — and it is exactly what fuzzers do.

---

## Verified reference output (from this machine)

Built with `cc -Wall -O0 -g -o dbparse dbparse.c` against a db created by the **Apple
system `sqlite3` 3.51**:

```
magic          : SQLite format 3
page size      : 4096
write/read ver : 1 / 1  (rollback journal)
reserved bytes : 12   (usable = 4084)
change counter : 3
db size (pages): 3   (file has 3)
freelist trunk : 0   count=0
schema cookie  : 2   format=4
text encoding  : 1   (1=utf8 2=utf16le 3=utf16be)
written by lib : 3051000

=== walking b-tree rooted at page 1 ===
page 1: leaf table  nCell=2 cellTop=3941 frag=0
  cell[0] off=3997 P=85 rowid=1 ('table','users','users',2,'CREATE TABLE users(...)')
  cell[1] off=3941 P=54 rowid=2 ('index','i_age','users',3,'CREATE INDEX i_age ON users(age)')

=== walking b-tree rooted at page 2 ===
page 2: leaf table  nCell=3 cellTop=4050 frag=0
  cell[0] off=4074 P=8  rowid=1 (NULL, 'ada',   36)
  cell[1] off=4062 P=10 rowid=2 (NULL, 'linus', 54)
  cell[2] off=4050 P=10 rowid=3 (NULL, 'grace', 45)

=== walking b-tree rooted at page 3 ===
page 3: leaf index  nCell=3 cellTop=4067 frag=0
  cell[0] off=4079 P=4 key=(36, 1)
  cell[1] off=4073 P=5 key=(45, 3)
  cell[2] off=4067 P=5 key=(54, 2)
```

### Read three real lessons out of that output

1. **`reserved bytes : 12`.** Upstream SQLite defaults to 0. Apple's build reserves 12
   bytes per page for its own extension. This is *exactly* why every format formula uses
   `usable = pagesize - hdr[20]` and never `pagesize`. If your parser hard-codes 4096 it
   will mis-locate overflow pointers on this machine. Compare:
   ```sh
   sqlite3 mini.db 'PRAGMA page_size;'      # 4096
   ./dbparse mini.db | grep usable          # usable = 4084
   ```
   Try the same test with a db created by *your own* build of upstream SQLite — reserved
   will be 0. Note both in `labs/phase01/header-decode.md`.

2. **`(NULL, 'ada', 36)`** — the `INTEGER PRIMARY KEY` column really is stored as NULL;
   the value is the cell's rowid. `OP_Column` in `vdbe.c` special-cases this
   (`VdbeCursor.nullRow` / the `pC->payloadSize` path plus `OP_Rowid`) so SQL sees `1`.

3. **Index page cells are `(age, rowid)`** in sorted order: `(36,1) (45,3) (54,2)`. The
   rowid is appended to make every index key unique, which is what lets a non-unique index
   be stored in a plain b-tree with no duplicate handling at all.

### Gotchas that will cost you an hour if you don't know them

- `cellTop = 0` **means 65536**, not 0 (only reachable with 64 KiB pages).
- Cell *pointers* are key-sorted; cell *bodies* are in arbitrary physical order. Do not
  assume `off` increases.
- Page 1's b-tree header starts at offset **100**, but cell pointer offsets are still
  measured from the start of the page (offset 0), not from 100.
- The 12-byte interior header's right-most child pointer is at offset 8 — do not read it
  on a leaf page.
- Text is **not NUL-terminated** on disk; length comes from the serial type only.
- A `varint` can encode a value that doesn't fit the field you're storing it into. Fuzzers
  exploit exactly this; that's why `btreeParseCellPtr()` is so paranoid.
