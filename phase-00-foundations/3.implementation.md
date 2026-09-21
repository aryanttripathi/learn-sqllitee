# Phase 0 — Implementation Labs: Build It, Break It, Trace It

All labs assume `~/Desktop/sqllite` as your root. Put outputs in `labs/phase00/`.

---

## Lab 0.1 — Get both forms of the source

```sh
cd ~/Desktop/sqllite
mkdir -p labs/phase00

# canonical tree (official GitHub mirror of the Fossil repo)
git clone --depth=1 https://github.com/sqlite/sqlite.git sqlite-src

cd sqlite-src
ls src | head -40
wc -l src/*.c | sort -n | tail -20     # the files that matter, by size
```

Write the top-10 list into `labs/phase00/biggest-files.txt`. You are going to live in
those files for the next two months.

---

## Lab 0.2 — Build a debug shell

```sh
cd ~/Desktop/sqllite/sqlite-src
mkdir -p build && cd build

../configure --enable-debug --enable-all CFLAGS="-g -O0 -DSQLITE_DEBUG=1 \
  -DSQLITE_ENABLE_EXPLAIN_COMMENTS -DSQLITE_ENABLE_SELECTTRACE \
  -DSQLITE_ENABLE_WHERETRACE -DSQLITE_ENABLE_DBSTAT_VTAB"

make -j8 sqlite3
./sqlite3 --version
```

If `configure` complains about TCL: `brew install tcl-tk`, or build only the amalgamation
path (Lab 0.3). On recent trees `--enable-all` pulls in FTS5, RTree, GEOPOLY, JSON, session.

Verify the debug hooks are live:

```sh
./sqlite3 :memory: '.testctrl' 2>&1 | head
# and inside the shell:
./sqlite3 :memory: <<'SQL'
.eqp full
CREATE TABLE t(a,b);
CREATE INDEX i ON t(a);
EXPLAIN QUERY PLAN SELECT b FROM t WHERE a=5;
SQL
```

---

## Lab 0.3 — The 30-second build (amalgamation)

Prove to yourself how small the delivery unit is:

```sh
cd ~/Desktop/sqllite
curl -sO https://sqlite.org/2025/sqlite-amalgamation-3510000.zip
unzip -q sqlite-amalgamation-*.zip && cd sqlite-amalgamation-3510000
time cc -O2 -o mysqlite shell.c sqlite3.c -lpthread -ldl -lm
./mysqlite :memory: 'select sqlite_version(), 1+1;'
```

Two `.c` files. That is the whole product. Note the compile time and binary size:

```sh
ls -lh mysqlite
size mysqlite
```

Record both in `labs/phase00/build-notes.md`.

---

## Lab 0.4 — Watch the code generators run

```sh
cd ~/Desktop/sqllite/sqlite-src/build
rm -f parse.c opcodes.h
make parse.c opcodes.h 2>&1 | tail -20
head -30 opcodes.h
grep -n "OP_OpenRead\|OP_Column\|OP_Next" opcodes.h
```

Now find where that number came from:

```sh
grep -n "Opcode: OpenRead" ../src/vdbe.c
sed -n '/Opcode: OpenRead/,/^case OP_OpenRead/p' ../src/vdbe.c | head -40
```

**The doc comment in `vdbe.c` is the source of truth for the opcode table.** Internalize
this: in SQLite, comments are sometimes compiled.

---

## Lab 0.5 — Your first end-to-end trace with a debugger

Create a tiny database, then single-step the read path.

```sh
cd ~/Desktop/sqllite/labs/phase00
~/Desktop/sqllite/sqlite-src/build/sqlite3 demo.db <<'SQL'
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);
INSERT INTO users VALUES (1,'ada',36),(2,'linus',54),(3,'grace',45);
SQL
```

Write a driver so you own `main()`:

```c
/* labs/phase00/driver.c  —  compile against the debug build */
#include <stdio.h>
#include "sqlite3.h"

int main(void) {
  sqlite3 *db;
  sqlite3_stmt *st;
  int rc = sqlite3_open("demo.db", &db);
  if (rc) { fprintf(stderr, "open: %s\n", sqlite3_errmsg(db)); return 1; }

  rc = sqlite3_prepare_v2(db, "SELECT name, age FROM users WHERE age > ?", -1, &st, 0);
  if (rc != SQLITE_OK) { fprintf(stderr, "prep: %s\n", sqlite3_errmsg(db)); return 1; }

  sqlite3_bind_int(st, 1, 40);

  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    printf("%s %d\n", sqlite3_column_text(st, 0), sqlite3_column_int(st, 1));
  }
  if (rc != SQLITE_DONE) fprintf(stderr, "step: %s\n", sqlite3_errmsg(db));

  sqlite3_finalize(st);
  sqlite3_close(db);
  return 0;
}
```

```sh
cc -g -O0 -I ~/Desktop/sqllite/sqlite-src -I ~/Desktop/sqllite/sqlite-src/build \
   driver.c ~/Desktop/sqllite/sqlite-src/build/sqlite3.c -o driver -lpthread -ldl -lm
./driver
```

Now trace (macOS uses `lldb`; commands for `gdb` in parentheses):

```
lldb ./driver
(lldb) b sqlite3VdbeExec          # gdb: break sqlite3VdbeExec
(lldb) b sqlite3BtreeNext
(lldb) b unixRead
(lldb) run
(lldb) bt                         # backtrace: see the whole layer cake in one screen
```

**Deliverable:** paste the backtrace from `unixRead` into
`labs/phase00/first-backtrace.txt`. You should see, bottom to top:
`main → sqlite3_step → sqlite3VdbeExec → sqlite3BtreeXxx → sqlite3PagerGet → unixRead`.
That single stack is the map of this entire course.

---

## Lab 0.6 — Read the bytecode of your query

```sh
cd ~/Desktop/sqllite/labs/phase00
~/Desktop/sqllite/sqlite-src/build/sqlite3 demo.db <<'SQL'
.explain on
EXPLAIN SELECT name, age FROM users WHERE age > 40;
SQL
```

Expected shape (numbers/opcodes vary by version):

```
addr  opcode         p1    p2    p3    p4             p5  comment
0     Init           0     10    0                    0   Start at 10
1     OpenRead       0     2     0     3              0   root=2 iDb=0; users
2     Rewind         0     9     0                    0
3       Column       0     2     1                    0   r[1]=users.age
4       Le            2     8     1    (BINARY)       0   if r[1]<=r[2] goto 8
5       Column       0     1     3                    0   r[3]=users.name
6       Column       0     2     4                    0   r[4]=users.age
7       ResultRow    3     2     0                    0   output=r[3..4]
8     Next           0     3     0                    1
9     Halt           0     0     0                    0
10    Transaction    0     0     1     0              1
11    Integer        40    2     0                    0   r[2]=40
12    Goto           0     1     0                    0
```

Answer these in `labs/phase00/bytecode-notes.md`:
1. Why does execution start at address 10 and jump back to 1?
2. What is `OpenRead P2=2`? (Hint: `SELECT rootpage FROM sqlite_schema WHERE name='users'`)
3. Which opcode does the actual disk read?

---

## Lab 0.7 — Hexdump the file you just made (Phase 1 teaser)

```sh
cd ~/Desktop/sqllite/labs/phase00
xxd -l 112 demo.db
```

```
00000000: 5351 4c69 7465 2066 6f72 6d61 7420 3300  SQLite format 3.
00000010: 1000 0101 0040 2020 0000 0002 0000 0003  .....@  ........
                ^^^^ page size 0x1000 = 4096
```

Just look. You will decode every byte in Phase 1.

---

## Lab 0.8 — Run a slice of the real test suite

```sh
cd ~/Desktop/sqllite/sqlite-src/build
make -j8 testfixture
./testfixture ../test/select1.test 2>&1 | tail -5
./testfixture ../test/btree01.test 2>&1 | tail -5
```

Then make a deliberate bug and watch the suite catch it:

```sh
# in src/select.c or src/where.c, flip a comparison, rebuild, rerun.
# Revert with: git checkout -- src/
```

**Deliverable:** `labs/phase00/test-notes.md` — which test file failed, and the exact
assertion text.

---

## Lab 0.9 — Set up your reading toolkit

```sh
# ctags/cscope over the tree makes jump-to-definition work in any editor
cd ~/Desktop/sqllite/sqlite-src
ctags -R src ext            # or: universal-ctags -R --languages=C src ext

# fast grep helpers you will use constantly
grep -rn "sqlite3BtreeInsert" src/ | head
grep -rn "struct BtCursor" src/btreeInt.h
```

Bookmark these upstream docs (they are unusually good):
`sqlite.org/arch.html`, `sqlite.org/fileformat2.html`, `sqlite.org/opcode.html`,
`sqlite.org/optoverview.html`, `sqlite.org/queryplanner-ng.html`, `sqlite.org/wal.html`,
`sqlite.org/atomiccommit.html`, `sqlite.org/vdbe.html`, `sqlite.org/testing.html`.
