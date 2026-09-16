/* seriesvtab.c — a complete, minimal eponymous virtual table.
 *
 *   SELECT value FROM myseries(1, 10, 2);
 *   SELECT value FROM myseries WHERE start=1 AND stop=10 AND step=2;
 *
 * Demonstrates the whole vtab protocol: xCreate/xConnect, xBestIndex, xOpen,
 * xFilter, xNext, xEof, xColumn, xRowid, xDisconnect.
 *
 * Build as a loadable extension:
 *   cc -fPIC -shared -I. -o myseries.dylib seriesvtab.c      (macOS)
 *   cc -fPIC -shared -I. -o myseries.so    seriesvtab.c      (Linux)
 * Load:  sqlite3 :memory: ".load ./myseries" "SELECT * FROM myseries(1,5,1);"
 */
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* Column numbering must match the CREATE TABLE passed to declare_vtab */
#define SERIES_COL_VALUE 0
#define SERIES_COL_START 1
#define SERIES_COL_STOP  2
#define SERIES_COL_STEP  3

typedef struct SeriesCursor SeriesCursor;
struct SeriesCursor {
  sqlite3_vtab_cursor base;      /* MUST be first */
  sqlite3_int64 iRowid;
  sqlite3_int64 value, start, stop, step;
};

static int seriesConnect(sqlite3 *db, void *pAux, int argc,
                         const char *const*argv, sqlite3_vtab **ppVtab,
                         char **pzErr){
  sqlite3_vtab *pNew;
  int rc;
  (void)pAux; (void)argc; (void)argv; (void)pzErr;
  /* HIDDEN columns are usable as arguments/constraints but not returned by * */
  rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(value INTEGER, start HIDDEN, stop HIDDEN, step HIDDEN)");
  if( rc!=SQLITE_OK ) return rc;
  pNew = *ppVtab = sqlite3_malloc(sizeof(*pNew));
  if( pNew==0 ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS);
  return SQLITE_OK;
}

static int seriesDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int seriesOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor){
  SeriesCursor *pCur = sqlite3_malloc(sizeof(*pCur));
  (void)p;
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

static int seriesClose(sqlite3_vtab_cursor *cur){
  sqlite3_free(cur);
  return SQLITE_OK;
}

static int seriesNext(sqlite3_vtab_cursor *cur){
  SeriesCursor *p = (SeriesCursor*)cur;
  p->value += p->step;
  p->iRowid++;
  return SQLITE_OK;
}

static int seriesEof(sqlite3_vtab_cursor *cur){
  SeriesCursor *p = (SeriesCursor*)cur;
  return p->step>0 ? p->value > p->stop : p->value < p->stop;
}

static int seriesColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i){
  SeriesCursor *p = (SeriesCursor*)cur;
  sqlite3_int64 x;
  switch( i ){
    case SERIES_COL_START: x = p->start; break;
    case SERIES_COL_STOP:  x = p->stop;  break;
    case SERIES_COL_STEP:  x = p->step;  break;
    default:               x = p->value; break;
  }
  sqlite3_result_int64(ctx, x);
  return SQLITE_OK;
}

static int seriesRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  *pRowid = ((SeriesCursor*)cur)->iRowid;
  return SQLITE_OK;
}

/* xFilter receives the constraints xBestIndex asked for, in argv[] order.
 * idxNum is a bitmask: 1=start given, 2=stop given, 4=step given. */
static int seriesFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr,
                        int argc, sqlite3_value **argv){
  SeriesCursor *p = (SeriesCursor*)cur;
  int i = 0;
  (void)idxStr; (void)argc;
  p->start = (idxNum & 1) ? sqlite3_value_int64(argv[i++]) : 0;
  p->stop  = (idxNum & 2) ? sqlite3_value_int64(argv[i++]) : 0xffffffff;
  p->step  = (idxNum & 4) ? sqlite3_value_int64(argv[i++]) : 1;
  if( p->step==0 ) p->step = 1;
  p->value = p->start;
  p->iRowid = 1;
  return SQLITE_OK;
}

/* xBestIndex: tell SQLite which constraints we can consume and what it costs. */
static int seriesBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  int i, idxNum = 0;
  int aIdx[3] = {-1,-1,-1};      /* argv position for start, stop, step */
  (void)tab;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    const struct sqlite3_index_constraint *pC = &pIdxInfo->aConstraint[i];
    int col;
    if( !pC->usable ) continue;
    if( pC->op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    switch( pC->iColumn ){
      case SERIES_COL_START: col = 0; idxNum |= 1; break;
      case SERIES_COL_STOP:  col = 1; idxNum |= 2; break;
      case SERIES_COL_STEP:  col = 2; idxNum |= 4; break;
      default: continue;
    }
    aIdx[col] = i;
  }
  {
    int nArg = 0;
    for(i=0; i<3; i++){
      if( aIdx[i]>=0 ){
        pIdxInfo->aConstraintUsage[aIdx[i]].argvIndex = ++nArg;
        pIdxInfo->aConstraintUsage[aIdx[i]].omit = 1;   /* we handle it fully */
      }
    }
  }
  pIdxInfo->idxNum = idxNum;
  /* cost model: cheap if bounded, astronomically expensive if unbounded */
  if( (idxNum & 3)==3 ){
    pIdxInfo->estimatedCost = 2.0;
    pIdxInfo->estimatedRows = 1000;
    pIdxInfo->orderByConsumed = 1;      /* we always produce ascending values */
  }else{
    pIdxInfo->estimatedCost = 2147483647.0;
    pIdxInfo->estimatedRows = 2147483647;
  }
  return SQLITE_OK;
}

static sqlite3_module seriesModule = {
  0,                  /* iVersion */
  0,                  /* xCreate — 0 makes this an EPONYMOUS-ONLY virtual table */
  seriesConnect,
  seriesBestIndex,
  seriesDisconnect,
  0,                  /* xDestroy */
  seriesOpen,
  seriesClose,
  seriesFilter,
  seriesNext,
  seriesEof,
  seriesColumn,
  seriesRowid,
  0,0,0,0,0,0,0,0,0,0,0,0
};

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_myseries_init(sqlite3 *db, char **pzErrMsg,
                          const sqlite3_api_routines *pApi){
  int rc;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  rc = sqlite3_create_module(db, "myseries", &seriesModule, 0);
  return rc;
}
