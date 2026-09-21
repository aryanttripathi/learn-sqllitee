# Phase 4 — Code Walkthrough: Tokenizer, Parser, Schema

Source:
[`src/tokenize.c`](https://github.com/sqlite/sqlite/blob/master/src/tokenize.c) ·
[`src/parse.y`](https://github.com/sqlite/sqlite/blob/master/src/parse.y) ·
[`src/resolve.c`](https://github.com/sqlite/sqlite/blob/master/src/resolve.c) ·
[`src/build.c`](https://github.com/sqlite/sqlite/blob/master/src/build.c) ·
[`src/prepare.c`](https://github.com/sqlite/sqlite/blob/master/src/prepare.c)

---

## Mental model for this phase

> **Read `parse.y`, never `parse.c`.** The grammar file is hand-written and is half the
> front end; `parse.c` is a generated table-driven machine with no human-meaningful
> structure. Anything you want to know about SQL syntax is a grammar rule plus a C action.

And the second rule: **the front end's output is `iCursor` and `iColumn` numbers.** Parsing
produces names; resolution replaces names with integers; everything downstream only ever
sees integers. When you get lost in `resolve.c`, ask "which integer is this trying to fill
in?"

---

## 1. `sqlite3RunParser()` — the whole front-end driver

Real code, `src/tokenize.c`, trimmed to the load-bearing lines:

```c
SQLITE_PRIVATE int sqlite3RunParser(Parse *pParse, const char *zSql){
  int nErr = 0;
  void *pEngine;                  /* The LEMON-generated LALR(1) parser */
  int n = 0;                      /* Length of the next token token */
  int tokenType;                  /* type of the next token */
  int lastTokenParsed = -1;       /* type of the previous token */
  sqlite3 *db = pParse->db;
  int mxSqlLen;
#ifdef sqlite3Parser_ENGINEALWAYSONSTACK
  yyParser sEngine;    /* Space to hold the Lemon-generated Parser object */
#endif

  mxSqlLen = db->aLimit[SQLITE_LIMIT_SQL_LENGTH];
  if( db->nVdbeActive==0 ){
    AtomicStore(&db->u1.isInterrupted, 0);
  }
  pParse->rc = SQLITE_OK;
  pParse->zTail = zSql;
#ifdef sqlite3Parser_ENGINEALWAYSONSTACK
  pEngine = &sEngine;
  sqlite3ParserInit(pEngine, pParse);
#else
  pEngine = sqlite3ParserAlloc(sqlite3Malloc, pParse);
  if( pEngine==0 ){ sqlite3OomFault(db); return SQLITE_NOMEM_BKPT; }
#endif
  pParentParse = db->pParse;
  db->pParse = pParse;
  while( 1 ){
    n = sqlite3GetToken((u8*)zSql, &tokenType);
    mxSqlLen -= n;
    if( mxSqlLen<0 ){
      pParse->rc = SQLITE_TOOBIG;
      pParse->nErr++;
      break;
    }
    if( tokenType>=TK_WINDOW ){            /* TK_SPACE, TK_COMMENT, TK_ILLEGAL, ... */
      if( AtomicLoad(&db->u1.isInterrupted) ){
        pParse->rc = SQLITE_INTERRUPT;
        pParse->nErr++;
        break;
      }
      if( tokenType==TK_SPACE ){
        zSql += n;
        continue;                          /* whitespace never reaches the parser */
      }
      ...
    }
    ...
    sqlite3Parser(pEngine, tokenType, pParse->sLastToken);   /* ← the LALR machine */
    ...
  }
  ...
}
```

| Line | Reading |
|---|---|
| `yyParser sEngine;` under `ENGINEALWAYSONSTACK` | the parser object lives on the **C stack** in the common build — no malloc per statement. `sqlite3ParserAlloc` is the fallback. Parsing happens on every `prepare()`, so allocation cost matters. |
| `mxSqlLen -= n; if( mxSqlLen<0 )` | `SQLITE_LIMIT_SQL_LENGTH` is enforced *during* tokenizing, not up front, so a hostile 1 GB statement is rejected early instead of being scanned twice. |
| `if( db->nVdbeActive==0 ) AtomicStore(&db->u1.isInterrupted, 0);` | clear a stale `sqlite3_interrupt()` only if nothing is running — otherwise you would cancel someone else's interrupt. |
| `if( tokenType>=TK_WINDOW )` | **the token-code ordering is load-bearing.** Lemon assigns codes so that all "non-grammar" tokens (SPACE, COMMENT, ILLEGAL, and the context-sensitive WINDOW/OVER/FILTER) sit above a threshold. One comparison filters them all. |
| `AtomicLoad(&db->u1.isInterrupted)` inside the loop | this is where `sqlite3_interrupt()` cancels a *long parse* (think a 100k-row `VALUES` list). |
| `if( tokenType==TK_SPACE ){ zSql += n; continue; }` | comments and whitespace are dropped here. The grammar never sees them — which is why SQLite cannot round-trip comments in `ALTER TABLE`. |
| `sqlite3Parser(pEngine, tokenType, pParse->sLastToken)` | **one token pushed at a time.** There is no token array; the tokenizer and parser are co-routines in the loop. |
| `db->pParse = pParse;` with `pParentParse` saved | parses nest (`sqlite3NestedParse` for DDL). The connection tracks the innermost one. |

**Take-away:** the entire front-end driver is a token loop plus a nesting stack. All the
intelligence lives in the grammar actions.

---

## 2. `sqlite3GetToken()` — a lexer as a jump table

[Source](https://github.com/sqlite/sqlite/blob/master/src/tokenize.c) — search for
`SQLITE_PRIVATE int sqlite3GetToken(`. Shape:

```c
int sqlite3GetToken(const unsigned char *z, int *tokenType){
  int i, c;
  switch( aiClass[*z] ){        /* 256-entry character class table */
    case CC_SPACE: { for(i=1; sqlite3Isspace(z[i]); i++){}
                     *tokenType = TK_SPACE; return i; }
    case CC_MINUS: {
      if( z[1]=='-' ){          /* -- comment */
        for(i=2; (c=z[i])!=0 && c!='\n'; i++){}
        *tokenType = TK_SPACE; return i;
      }
      *tokenType = TK_MINUS; return 1;
    }
    case CC_ID: {
      for(i=1; sqlite3IsIdChar(z[i]); i++){}
      *tokenType = keywordCode((char*)z, i, tokenType);   /* keyword or identifier? */
      return i;
    }
    ...
  }
}
```

Two design points worth your attention:

- **`aiClass[]`** turns the first byte into a case label with no comparison chain. Building a
  256-entry table to avoid `if(c==' '||c=='\t'||...)` is the same instinct as `decodeFlags`
  installing function pointers in Phase 1: decide once, then dispatch.
- **`keywordCode()`** is a *generated perfect hash* (`keywordhash.h`, produced by
  `tool/mkkeywordhash.c`). Every identifier costs one hash + one `memcmp`, never a keyword
  list scan. Look at the generated file once:
  ```sh
  A=~/Desktop/sqllite/sqlite-amalgamation-3500400
  grep -n "static int keywordCode" $A/sqlite3.c
  sed -n '/^static int keywordCode/,/^}/p' $A/sqlite3.c | head -40
  ```
  The `aKWHash[]`, `aKWNext[]`, `aKWLen[]`, `aKWOffset[]`, `zKWText[]` arrays are all
  emitted by the build; `zKWText` is one long string with all keywords packed together and
  overlapping where possible.

---

## 3. `parse.y` — read grammar rules, not generated tables

The rule that builds a `SELECT`:

```
oneselect(A) ::= SELECT distinct(D) selcollist(W) from(X) where_opt(Y)
                 groupby_opt(P) having_opt(Q) orderby_opt(Z) limit_opt(L). {
  A = sqlite3SelectNew(pParse,W,X,Y,P,Q,Z,D,L);
}
```
Read it as: *when the parser reduces these nine symbols, call `sqlite3SelectNew()` and the
result becomes the value of `oneselect`.* The `(A)`, `(W)`, `(X)` names are the value
bindings. **This one line is the entire connection between SQL syntax and the `Select`
struct.**

Expression rules are even more direct:
```
expr(A) ::= expr(X) PLUS expr(Y).   { A = sqlite3PExpr(pParse, TK_PLUS, X, Y); }
expr(A) ::= expr(X) EQ   expr(Y).   { A = sqlite3PExpr(pParse, TK_EQ,   X, Y); }
```
Precedence comes from the declaration block, and reading it top to bottom gives you
SQLite's operator precedence table for free:
```
%left OR.
%left AND.
%right NOT.
%left IS MATCH LIKE_KW BETWEEN IN ISNULL NOTNULL NE EQ.
%left GT LE LT GE.
%right ESCAPE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH REM.
%left CONCAT.
%left COLLATE.
%right BITNOT.
```

Three Lemon-specific directives worth knowing:

| Directive | Effect |
|---|---|
| `%destructor` | frees a partially built subtree when a syntax error aborts a reduction. This is why a malformed statement leaks nothing, even mid-tree. |
| `%fallback ID ABORT ACTION AFTER ...` | lets those keywords be used as identifiers where unambiguous — the reason `CREATE TABLE t(key, action)` works. |
| `%syntax_error` / `%stack_overflow` | the error-message and depth-limit hooks. |

```sh
grep -n "%fallback" ~/Desktop/sqllite/sqlite-src/src/parse.y
grep -n "^%left\|^%right\|^%nonassoc" ~/Desktop/sqllite/sqlite-src/src/parse.y
```

**Skip `parse.c` entirely** except once, to see `yy_action[]`/`yy_reduce()` and confirm
there is nothing human-readable there.

---

## 4. `sqlite3AffinityType()` — the five rules, as a rolling hash

Real code, `src/build.c`:

```c
SQLITE_PRIVATE char sqlite3AffinityType(const char *zIn, Column *pCol){
  u32 h = 0;
  char aff = SQLITE_AFF_NUMERIC;
  const char *zChar = 0;

  while( zIn[0] ){
    u8 x = *(u8*)zIn;
    h = (h<<8) + sqlite3UpperToLower[x];
    zIn++;
    if( h==(('c'<<24)+('h'<<16)+('a'<<8)+'r') ){             /* CHAR */
      aff = SQLITE_AFF_TEXT;
      zChar = zIn;
    }else if( h==(('c'<<24)+('l'<<16)+('o'<<8)+'b') ){       /* CLOB */
      aff = SQLITE_AFF_TEXT;
    }else if( h==(('t'<<24)+('e'<<16)+('x'<<8)+'t') ){       /* TEXT */
      aff = SQLITE_AFF_TEXT;
    }else if( h==(('b'<<24)+('l'<<16)+('o'<<8)+'b')          /* BLOB */
        && (aff==SQLITE_AFF_NUMERIC || aff==SQLITE_AFF_REAL) ){
      aff = SQLITE_AFF_BLOB;
      if( zIn[0]=='(' ) zChar = zIn;
    }else if( h==(('r'<<24)+('e'<<16)+('a'<<8)+'l')          /* REAL */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
    }else if( h==(('f'<<24)+('l'<<16)+('o'<<8)+'a')          /* FLOA */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
    }else if( h==(('d'<<24)+('o'<<16)+('u'<<8)+'b')          /* DOUB */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
    }else if( (h&0x00FFFFFF)==(('i'<<16)+('n'<<8)+'t') ){    /* INT */
      aff = SQLITE_AFF_INTEGER;
      break;
    }
  }
  ...
}
```

This is the best short function in the front end. Line by line:

- `h = (h<<8) + sqlite3UpperToLower[x];` — a **4-byte sliding window** over the declared
  type, case-folded as it goes. `h` always holds the last four characters.
- Each `else if` compares that window to a 4-character literal built at compile time from
  character constants. No `strstr`, no allocation, one pass.
- `(h&0x00FFFFFF)==...'int'` — INT is only three characters, so the top byte is masked off.
- **`break;` only on INT, and the INT branch has no `aff==` guard.** Every other match keeps
  scanning and can be overridden; INT wins unconditionally and stops the loop. Trace
  `"FLOATING POINT"`: the window reaches `floa` → `aff = REAL`; scanning continues through
  `loat`, `oati`, ... `poin`, `oint`; the masked low three bytes of `oint` are `int` →
  `aff = INTEGER`, `break`. The famously surprising result is this one unguarded branch.
  Verified: `CREATE TABLE t(a "FLOATING POINT"); INSERT INTO t VALUES('123');
  SELECT typeof(a) FROM t;` → `integer`.
- The guards `&& aff==SQLITE_AFF_NUMERIC` implement rule precedence: REAL/FLOA/DOUB only
  apply if nothing stronger has matched yet, and BLOB only downgrades from NUMERIC/REAL.
- `zChar` remembers where `CHAR(` / `BLOB(` started so the tail of the function can parse
  `VARCHAR(255)` into a **size estimate** (`Column.szEst`), used later by the query planner
  for row-size costing.

Try to break it, then check against the code:
```sh
sqlite3 :memory: 'CREATE TABLE t(a "FLOATING POINT", b "MY TEXT FIELD", c "BIGBLOB");
                  SELECT name,type FROM pragma_table_info("t");'
```

---

## 5. `lookupName()` — mechanism only (~400 lines)

[Source](https://github.com/sqlite/sqlite/blob/master/src/resolve.c) — search for
`static int lookupName(`. Do not read it linearly; it is one long cascade with many special
cases. The mechanism:

```
Inputs : zDb, zTab, zCol (any may be NULL) + a NameContext chain
Output : pExpr->op = TK_COLUMN, pExpr->iTable = cursor, pExpr->iColumn = column index

for( pNC = current context; pNC; pNC = pNC->pNext ){     /* ← outer queries */
   for each SrcItem in pNC->pSrcList:
       if zDb given and doesn't match the item's schema        → skip
       if zTab given and doesn't match name or alias           → skip
       for each column of the table:
            if name matches (case-insensitive)                 → candidate
       also check "rowid"/"oid"/"_rowid_"                      → iColumn = -1
   if a candidate was found in this context:
       pNC->nRef++          ← THE correlation counter
       break
   /* not found here: also try result-set aliases (ORDER BY / GROUP BY / HAVING) */
}
if( cnt==0 ) → "no such column"   ;  if( cnt>1 ) → "ambiguous column name"
```

The two lines that matter most downstream:

```c
pExpr->iTable  = pMatch->iCursor;    /* becomes OP_Column's P1 in the bytecode */
pExpr->iColumn = (i16)iCol;          /* becomes OP_Column's P2 */
pNC->nRef++;                         /* if pNC is an OUTER context ⇒ correlated subquery */
```

`nRef` on an outer `NameContext` is the *entire* mechanism by which SQLite knows a subquery
is correlated — which decides whether it can be cached, flattened, or must be re-run per
row. One counter.

Special names handled here (worth knowing they exist so you can find them):
`rowid`/`oid`/`_rowid_`, `NEW.`/`OLD.` inside triggers, `excluded.` inside upserts, and
generated-column references.

You saw the *effect* of this function in Phase 4 Lab 4.2, where the treetrace output changed
from `ID "name"` to `{0:1}`. That is `lookupName` writing those two integers.

---

## 6. Schema bootstrap: `sqlite3InitCallback()` (mechanism)

[Source](https://github.com/sqlite/sqlite/blob/master/src/prepare.c). `sqlite3InitOne()`
runs `SELECT name, rootpage, sql FROM sqlite_schema` and calls this per row:

```
if( argv[3] /* the CREATE statement text */ ){
    db->init.iDb    = iDb;
    db->init.newTnum = rootpage;      /* use THIS page, don't allocate one */
    db->init.orphanTrigger = 0;
    db->init.busy = 1;                /* ← "we are loading, not executing DDL" */
    sqlite3_prepare(db, argv[3], ..., &pStmt, 0);    /* RE-PARSE the CREATE stmt */
    db->init.busy = 0;
}else{
    /* no SQL text: an internal index (sqlite_autoindex_*) — build it directly */
}
```

The whole trick is `db->init.busy`. With it set, `sqlite3StartTable()` / `sqlite3CreateIndex()`
take a different path: install the object in the schema hash and use `db->init.newTnum` as
the root page, instead of allocating a page and emitting bytecode. **The same code that
executes `CREATE TABLE` also loads the schema** — one code path, two modes.

Consequences you can now explain precisely:
- a corrupt `sql` column makes the database unopenable (the re-parse fails),
- `ALTER TABLE RENAME` must rewrite stored SQL text of dependent objects,
- `PRAGMA writable_schema=ON` exists to repair exactly this,
- `SQLITE_SCHEMA` is thrown when the cookie changes, because every cached plan was built
  from a re-parse of that text.

```sh
grep -n "db->init.busy" ~/Desktop/sqllite/sqlite-amalgamation-3500400/sqlite3.c | head -20
```

---

## 7. Exercises against real source

Answer in `labs/phase04/source-questions.md` with `src/file.c` + function citations:

1. In `sqlite3RunParser`, why is the interrupt check inside the `tokenType>=TK_WINDOW`
   branch rather than at the top of the loop? What would the cost be if it ran per token?
2. Find `aiClass[]` and `keywordCode()`. How many keywords are there in your version, and
   what does `zKWText[]` look like?
3. In `sqlite3AffinityType`, trace `"FLOATING POINT"` character by character and show the
   value of `h` when the INT branch fires.
4. Find where `Column.szEst` is set from `VARCHAR(255)` and find one place the planner uses
   it.
5. In `lookupName`, find the exact line that increments `nRef` on an outer context, then
   find who reads it (`sqlite3SelectNew`? `flattenSubquery`? `sqlite3ExprIsConstant`?).
6. Find three places in `build.c` that test `db->init.busy` and explain how each behaves
   differently during schema load versus real DDL.
7. Find `sqlite3NestedParse()`. How does it implement the `#N` (VDBE register) syntax
   extension that is unavailable to user SQL?

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^static const unsigned char aiClass\[\]/,/};/p' $A/sqlite3.c | head -20
grep -n "szEst" $A/sqlite3.c | head -20
sed -n '/^SQLITE_PRIVATE void sqlite3NestedParse(/,/^}/p' $A/sqlite3.c
```
