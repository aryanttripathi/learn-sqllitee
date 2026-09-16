/* tokdump.c — dump the token stream and the parse tree of any SQL statement.
 *
 * Trick: we #include the AMALGAMATION itself, so every SQLITE_PRIVATE (static)
 * internal function is visible to us. This is the cheapest way to call SQLite
 * internals without patching the build.
 *
 * Build (takes ~20s; sqlite3.c is 260k lines):
 *   cc -O0 -g -DSQLITE_DEBUG=1 -DSQLITE_ENABLE_TREETRACE -DSQLITE_ENABLE_WHERETRACE \
 *      -I../../sqlite-amalgamation-3500400 -o tokdump tokdump.c -lpthread -ldl -lm
 * Run:
 *   ./tokdump "SELECT name FROM users WHERE age > 40 ORDER BY name"
 */
#include "sqlite3.c"          /* yes, the .c file */
#include <stdio.h>

static const char *tokenName(int t){
  switch( t ){
#define N(x) case x: return #x;
    N(TK_SEMI) N(TK_EXPLAIN) N(TK_QUERY) N(TK_PLAN) N(TK_BEGIN) N(TK_TRANSACTION)
    N(TK_COMMIT) N(TK_END) N(TK_ROLLBACK) N(TK_CREATE) N(TK_TABLE) N(TK_DROP)
    N(TK_SELECT) N(TK_FROM) N(TK_WHERE) N(TK_GROUP) N(TK_HAVING) N(TK_ORDER)
    N(TK_BY) N(TK_LIMIT) N(TK_DISTINCT) N(TK_JOIN) N(TK_ON) N(TK_AS) N(TK_UNION)
    N(TK_INSERT) N(TK_INTO) N(TK_VALUES) N(TK_UPDATE) N(TK_SET) N(TK_DELETE)
    N(TK_ID) N(TK_STRING) N(TK_INTEGER) N(TK_FLOAT) N(TK_BLOB) N(TK_VARIABLE)
    N(TK_DOT) N(TK_COMMA) N(TK_LP) N(TK_RP) N(TK_STAR) N(TK_PLUS) N(TK_MINUS)
    N(TK_SLASH) N(TK_REM) N(TK_EQ) N(TK_NE) N(TK_LT) N(TK_LE) N(TK_GT) N(TK_GE)
    N(TK_AND) N(TK_OR) N(TK_NOT) N(TK_IS) N(TK_NULL) N(TK_LIKE_KW) N(TK_BETWEEN)
    N(TK_IN) N(TK_CASE) N(TK_WHEN) N(TK_THEN) N(TK_ELSE) N(TK_CAST) N(TK_COLLATE)
    N(TK_CONCAT) N(TK_INDEX) N(TK_PRIMARY) N(TK_KEY) N(TK_UNIQUE) N(TK_SPACE)
    N(TK_ILLEGAL) N(TK_ASC) N(TK_DESC) N(TK_INDEXED) N(TK_WITH)
#undef N
  }
  return "TK_(other)";
}

int main(int argc, char **argv){
  const char *zSql = argc>1 ? argv[1]
                            : "SELECT name FROM users WHERE age > 40 ORDER BY name";
  sqlite3 *db = 0;
  Parse sParse;
  char *zErr = 0;
  int i = 0, n, tokType;

  /* ---------- part 1: the raw token stream ---------- */
  printf("=== TOKENS for: %s\n", zSql);
  while( zSql[i] ){
    n = sqlite3GetToken((const unsigned char*)&zSql[i], &tokType);
    if( tokType!=TK_SPACE ){
      printf("  %-14s %-12.*s  (code %d, len %d)\n",
             tokenName(tokType), n, &zSql[i], tokType, n);
    }
    i += n;
  }

  /* ---------- part 2: let SQLite parse it and print its own trees ---------- */
  if( sqlite3_open(":memory:", &db) ) return 1;
  sqlite3_exec(db,
      "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);"
      "CREATE TABLE orders(id INTEGER PRIMARY KEY, uid INT, total REAL);"
      "CREATE INDEX i_age ON users(age);", 0, 0, &zErr);
  if( zErr ){ printf("schema error: %s\n", zErr); sqlite3_free(zErr); }

#ifdef SQLITE_DEBUG
  printf("\n=== TREETRACE: Select-tree transformations + generated plan\n");
  sqlite3TreeTrace = 0xffffffff;      /* every tree-dump point */
  sqlite3WhereTrace = 0;              /* set to 0xfff for planner internals (Phase 6) */
#endif
  {
    sqlite3_stmt *pStmt = 0;
    if( sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0)!=SQLITE_OK ){
      printf("  prepare error: %s\n", sqlite3_errmsg(db));
    }else{
      printf("\n=== BYTECODE\n%s\n", sqlite3_expanded_sql(pStmt) ? "" : "");
#ifdef SQLITE_DEBUG
      sqlite3TreeTrace = 0;
#endif
      sqlite3_finalize(pStmt);
    }
  }
  sqlite3_close(db);
  return 0;
}
