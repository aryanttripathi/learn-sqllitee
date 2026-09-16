# Phase 4 — Implementation Labs: See the Tree

A working, verified tool is checked in at **`labs/phase04/tokdump.c`**. It dumps the token
stream *and* makes SQLite print its own parse-tree transformations.

The amalgamation is already downloaded at
`~/Desktop/sqllite/sqlite-amalgamation-3500400/` (262,899 lines).

---

## Lab 4.1 — ⭐ The `#include "sqlite3.c"` trick

SQLite's internal functions are `SQLITE_PRIVATE` (i.e. `static`) in the amalgamation, so
you cannot link against them. The cheapest workaround: **include the amalgamation into your
own translation unit.**

```c
#include "sqlite3.c"        /* yes, the .c file */
#include <stdio.h>

int main(int argc, char **argv){
  const char *zSql = argv[1];
  int i = 0, n, tokType;
  while( zSql[i] ){
    n = sqlite3GetToken((const unsigned char*)&zSql[i], &tokType);   /* internal! */
    if( tokType!=TK_SPACE ) printf("%-12.*s code=%d len=%d\n", n, &zSql[i], tokType, n);
    i += n;
  }
  return 0;
}
```

```sh
cd ~/Desktop/sqllite/labs/phase04
cc -O0 -g -DSQLITE_DEBUG=1 -DSQLITE_ENABLE_TREETRACE -DSQLITE_ENABLE_SELECTTRACE \
   -DSQLITE_ENABLE_WHERETRACE -I../../sqlite-amalgamation-3500400 \
   -o tokdump tokdump.c -lpthread -ldl -lm
./tokdump "SELECT name FROM users WHERE age > 40 ORDER BY name"
```

### Verified token output

```
=== TOKENS for: SELECT name FROM users WHERE age > 40 ORDER BY name
  TK_SELECT      SELECT        (code 139, len 6)
  TK_ID          name          (code 60,  len 4)
  TK_FROM        FROM          (code 143, len 4)
  TK_ID          users         (code 60,  len 5)
  TK_WHERE       WHERE         (code 150, len 5)
  TK_ID          age           (code 60,  len 3)
  TK_GT          >             (code 55,  len 1)
  TK_INTEGER     40            (code 156, len 2)
  TK_ORDER       ORDER         (code 146, len 5)
  TK_BY          BY            (code 34,  len 2)
  TK_ID          name          (code 60,  len 4)
```

Note: token *codes* come from `parse.h`, which Lemon generates — they change between
versions. Never hard-code them.

**Exercises:**
1. Tokenize `SELECT "name", 'name', [name], \`name\` FROM t;` — four different quoting
   styles. Which become `TK_ID` and which `TK_STRING`?
2. Tokenize `SELECT 1--comment\n+2;` and explain the result.
3. Tokenize `SELECT * FROM t WHERE x = ?1 AND y = :name AND z = $v;` — what token type do
   parameters get?
4. Find a byte sequence that yields `TK_ILLEGAL`.
5. Read `keywordCode()` and `keywordhash.h` in the amalgamation (`grep -n "keywordCode"`).
   How many keywords are there, and what is the hash function?

---

## Lab 4.2 — ⭐ Watch name resolution happen

`tokdump` sets `sqlite3TreeTrace = 0xffffffff` before `sqlite3_prepare_v2()`, which makes
`treeview.c` dump the `Select` tree at every transformation stage.

### Verified output (trimmed)

```
1/0/ACF40B950: begin processing:
'-- SELECT (1/ACF40B950) selFlags=0x0 nSelectRow=0
    |-- result-set
    |   '-- SPAN("name")
    |       '-- ID "name"                      ← just a name; nothing resolved yet
    |-- FROM
    |   '-- {-1:*} users                       ← cursor not assigned yet (-1)
    |-- WHERE
    |   '-- GT
    |       |-- ID "age"
    |       '-- 40
    '-- ORDERBY
        '-- ID "name"

1/0/ACF40B950: After result-set wildcard expansion:
    |-- FROM
    |   '-- {0:*} users tab='users' nCol=3 ptr=100981B50 used=0   ← cursor 0 assigned

1/0/ACF40B950: after name resolution:
    |-- result-set
    |   '-- {0:1} pTab=... fg.af=800000.n      ← TK_COLUMN: cursor 0, column 1 (name)
    |-- WHERE
    |   '-- GT
    |       |-- {0:2} pTab=...                 ← cursor 0, column 2 (age)
    |       '-- 40
    '-- ORDERBY
        '-- iOrderByCol=1                      ← ORDER BY now refers to result column 1
            '-- {0:1} pTab=...

1/0/ACF40B950: Constant propagation not helpful
1/0/ACF40B950: After all FROM-clause analysis: ...
```

**Read the notation:** `{cursor:column}` — `{0:1}` is "cursor 0, column 1". `used=6` is the
`colUsed` bitmask (columns 1 and 2 ⇒ bits 1|2 = 6), which the planner uses to decide
whether an index is *covering*.

**Exercises — run each and describe what changed at which stage:**
```sh
./tokdump "SELECT * FROM users"                                   # wildcard expansion
./tokdump "SELECT u.name, o.total FROM users u JOIN orders o ON o.uid=u.id"
./tokdump "SELECT name FROM users WHERE id IN (SELECT uid FROM orders)"
./tokdump "SELECT name FROM (SELECT * FROM users WHERE age>40)"   # subquery flattening!
./tokdump "SELECT count(*), age FROM users GROUP BY age HAVING count(*)>1"
./tokdump "WITH t(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM t WHERE x<5) SELECT * FROM t"
./tokdump "SELECT name FROM users WHERE age>40 AND age>40"        # duplicate terms
./tokdump "SELECT a.name FROM users a, users b WHERE a.id=b.id"   # self join, 2 cursors
```

Deliverable `labs/phase04/treetrace-notes.md`: for the subquery-flattening example, show
the tree before and after and name the function that did it (`flattenSubquery` in
`select.c`).

---

## Lab 4.3 — Read the grammar

```sh
cd ~/Desktop/sqllite/sqlite-src
wc -l src/parse.y
grep -n "^cmd ::=" src/parse.y | head -40          # every statement type
grep -n "%fallback" src/parse.y
grep -n "%left\|%right\|%nonassoc" src/parse.y     # operator precedence, in order
sed -n '/^oneselect(A) ::= SELECT/,/^}/p' src/parse.y
```

Answer in `labs/phase04/grammar-notes.md`:
1. List SQLite's operator precedence from lowest to highest, straight from `parse.y`.
2. What does `%fallback ID ABORT ACTION ...` accomplish? Prove it:
   `CREATE TABLE t(key, action, rollback);` — does it work? Which keywords are *not* in the
   fallback list, and why not?
3. Find the rule for `INSERT ... ON CONFLICT` (upsert). How many grammar rules does it take?
4. Find `%syntax_error` and describe SQLite's error-message strategy.
5. How is `LIKE` handled — a keyword or a function? (Hint: `TK_LIKE_KW`, `likeFunc`.)

Then regenerate the parser and see how big it is:
```sh
cd ~/Desktop/sqllite/sqlite-src/build
make parse.c && wc -l parse.c && grep -c "" parse.h
grep -n "YYNSTATE\|YYNRULE\|yy_action\[\]" parse.c | head
```

---

## Lab 4.4 — Break the parser on purpose

```sh
./tokdump "SELECT FROM"                                    # syntax error path
./tokdump "SELECT * FROM t WHERE ((((((((((1))))))))))"    # nesting
python3 -c "print('SELECT ' + '('*2000 + '1' + ')'*2000)" > deep.sql
sqlite3 :memory: < deep.sql                                # expect: parser stack overflow
python3 -c "print('SELECT 1' + ' + 1'*100000 + ';')" > long.sql
sqlite3 :memory: < long.sql
```

Investigate the limits:
```sh
sqlite3 :memory: "SELECT * FROM pragma_compile_options WHERE compile_options LIKE 'MAX%';"
grep -n "SQLITE_MAX_EXPR_DEPTH\|SQLITE_LIMIT_SQL_LENGTH\|YYSTACKDEPTH" \
  ~/Desktop/sqllite/sqlite-amalgamation-3500400/sqlite3.c | head
```

Write `labs/phase04/limits.md`: which limit fires first for deep nesting vs. long
expressions vs. many columns vs. many tables in a join (the last is
`SQLITE_MAX_COMPOUND_SELECT`/64-table bitmask limit — find it).

---

## Lab 4.5 — Affinity, empirically

```sh
sqlite3 :memory: <<'SQL'
CREATE TABLE t(
  a INT, b INTEGER, c TINYINT, d VARCHAR(10), e CHARACTER(5), f TEXT,
  g BLOB, h "", i REAL, j DOUBLE, k FLOAT, l NUMERIC, m DECIMAL(10,5),
  n BOOLEAN, o DATE, p DATETIME, q STRING, r "FLOATING POINT"
);
SELECT name, type FROM pragma_table_info('t');
SQL
```

For each column, predict the affinity using the 5 rules, then verify:

```sh
sqlite3 :memory: <<'SQL'
CREATE TABLE t(a INT, d VARCHAR(10), g BLOB, i REAL, l NUMERIC, r "FLOATING POINT");
INSERT INTO t VALUES('123','123','123','123','123','123');
SELECT typeof(a), typeof(d), typeof(g), typeof(i), typeof(l), typeof(r) FROM t;
SQL
```

Expected: `integer|text|text|real|integer|integer` — note `r` ("FLOATING POINT" contains
"INT") and `g` (BLOB affinity keeps the string as text).

Then the comparison consequences:
```sh
sqlite3 :memory: <<'SQL'
CREATE TABLE t(x INTEGER, y TEXT);
INSERT INTO t VALUES(1,'1');
SELECT x='1', y=1, x=y FROM t;        -- 1|1|1 ... explain each!
SELECT '1'=1;                          -- 0  (two literals: no affinity applied)
SQL
```

Deliverable: `labs/phase04/affinity.md` — the 18-column prediction table with your
reasoning, plus an explanation of all four comparison results.

---

## Lab 4.6 — The schema is text: prove it

```sh
cd ~/Desktop/sqllite/labs/phase04
sqlite3 sch.db 'CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT); CREATE INDEX i ON t(b);'
sqlite3 sch.db 'SELECT type,name,tbl_name,rootpage,sql FROM sqlite_schema;'

# now corrupt the SQL text (not the data) and watch the database become unopenable
sqlite3 sch.db 'PRAGMA writable_schema=ON;
                UPDATE sqlite_schema SET sql="CREATE TABLE t(a INTEGER PRIMARY KEY, b TEX"
                WHERE name="t";'
sqlite3 sch.db 'SELECT * FROM t;'          # expect: malformed database schema
sqlite3 sch.db 'PRAGMA writable_schema=ON; PRAGMA integrity_check;'
```

Repair it the same way, then answer in `labs/phase04/schema-notes.md`:
1. Why does SQLite store SQL text rather than a binary schema?
2. What is `sqlite3InitCallback()` doing with `db->init.busy` and `db->init.newTnum`?
   (find them in the amalgamation)
3. What does `PRAGMA schema_version` do and who bumps it?
4. Reproduce `SQLITE_SCHEMA`: prepare a statement on connection A, run DDL on connection B,
   then step A's statement. What happens with `prepare()` vs `prepare_v2()`?

---

## Lab 4.7 — Add a keyword to SQL (real front-end surgery)

Goal: make `EXPLAIN SHAPE <select>` a valid statement that prints "hello" — a deliberately
trivial feature whose point is touching all the generated-code machinery.

```sh
cd ~/Desktop/sqllite/sqlite-src
# 1. add the keyword
grep -n "\"SELECT\"" tool/mkkeywordhash.c
#    add:  { "SHAPE",  "TK_SHAPE",  ALWAYS, 1 },   (follow the existing format)
# 2. add a grammar rule in src/parse.y, e.g.
#    cmd ::= SHAPE select(X). { ...call a function you write... }
# 3. rebuild
cd build && make sqlite3 2>&1 | tail -20
./sqlite3 :memory: "SHAPE SELECT 1;"
```

Deliverable: a patch file `labs/phase04/add-keyword.diff` and notes on which generated
files changed (`keywordhash.h`, `parse.c`, `parse.h`) and how you found where to edit.

This is the single best preparation for real front-end work: you will never again be
confused about what is generated and what is hand-written.

---

## Lab 4.8 — Tree printing from the debugger

```sh
lldb ~/Desktop/sqllite/sqlite-src/build/sqlite3
(lldb) b sqlite3Select
(lldb) run :memory:
sqlite> CREATE TABLE t(a,b); SELECT a FROM t WHERE b>1 ORDER BY a;
(lldb) call (void)sqlite3TreeViewSelect(0, p, 0)
(lldb) call (void)sqlite3TreeViewExpr(0, p->pWhere, 0)
(lldb) call (void)sqlite3TreeViewSrcList(0, p->pSrc)
(lldb) p p->selFlags
(lldb) p p->pSrc->a[0].iCursor
```

Deliverable: `labs/phase04/treeview-session.md` with your session transcript and an
explanation of every `selFlags` bit that was set (`grep -n "define SF_" sqlite3.c`).
