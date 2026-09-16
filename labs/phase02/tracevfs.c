/* tracevfs.c — a VFS shim that logs every OS call SQLite makes.
 * Build (macOS, against system sqlite):
 *     cc -Wall -o tracevfs tracevfs.c -lsqlite3
 * Build (against your own amalgamation):
 *     cc -Wall -I<amalg dir> -o tracevfs tracevfs.c <amalg>/sqlite3.c -lpthread -ldl -lm
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "sqlite3.h"

typedef struct TraceFile TraceFile;
struct TraceFile {
  sqlite3_file base;        /* MUST be first: we are a subclass */
  sqlite3_file *pReal;      /* the wrapped file */
  char zName[64];
};

static sqlite3_vfs *g_pRoot;          /* the VFS we delegate to */
static FILE *g_log;

static const char *lockName(int e){
  switch(e){
    case SQLITE_LOCK_NONE:      return "NONE";
    case SQLITE_LOCK_SHARED:    return "SHARED";
    case SQLITE_LOCK_RESERVED:  return "RESERVED";
    case SQLITE_LOCK_PENDING:   return "PENDING";
    case SQLITE_LOCK_EXCLUSIVE: return "EXCLUSIVE";
  }
  return "?";
}

static int traceClose(sqlite3_file *pFile){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "CLOSE   %s\n", p->zName);
  return p->pReal->pMethods->xClose(p->pReal);
}
static int traceRead(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "READ    %-12s off=%-9lld amt=%d\n", p->zName, (long long)iOfst, iAmt);
  return p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);
}
static int traceWrite(sqlite3_file *pFile, const void *z, int iAmt, sqlite3_int64 iOfst){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "WRITE   %-12s off=%-9lld amt=%d\n", p->zName, (long long)iOfst, iAmt);
  return p->pReal->pMethods->xWrite(p->pReal, z, iAmt, iOfst);
}
static int traceTruncate(sqlite3_file *pFile, sqlite3_int64 size){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "TRUNC   %-12s size=%lld\n", p->zName, (long long)size);
  return p->pReal->pMethods->xTruncate(p->pReal, size);
}
static int traceSync(sqlite3_file *pFile, int flags){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "SYNC    %-12s flags=0x%x  <<<<<< durability barrier\n", p->zName, flags);
  return p->pReal->pMethods->xSync(p->pReal, flags);
}
static int traceFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize){
  TraceFile *p = (TraceFile*)pFile;
  return p->pReal->pMethods->xFileSize(p->pReal, pSize);
}
static int traceLock(sqlite3_file *pFile, int eLock){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "LOCK    %-12s -> %s\n", p->zName, lockName(eLock));
  return p->pReal->pMethods->xLock(p->pReal, eLock);
}
static int traceUnlock(sqlite3_file *pFile, int eLock){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "UNLOCK  %-12s -> %s\n", p->zName, lockName(eLock));
  return p->pReal->pMethods->xUnlock(p->pReal, eLock);
}
static int traceCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  TraceFile *p = (TraceFile*)pFile;
  int rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  fprintf(g_log, "CHKRES  %-12s -> %d\n", p->zName, *pResOut);
  return rc;
}
static int traceFileControl(sqlite3_file *pFile, int op, void *pArg){
  TraceFile *p = (TraceFile*)pFile;
  return p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
}
static int traceSectorSize(sqlite3_file *pFile){
  TraceFile *p = (TraceFile*)pFile;
  return p->pReal->pMethods->xSectorSize(p->pReal);
}
static int traceDeviceCharacteristics(sqlite3_file *pFile){
  TraceFile *p = (TraceFile*)pFile;
  return p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
}
static int traceShmMap(sqlite3_file *pFile, int iPg, int pgsz, int b, void volatile **pp){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "SHMMAP  %-12s pg=%d sz=%d extend=%d\n", p->zName, iPg, pgsz, b);
  return p->pReal->pMethods->xShmMap(p->pReal, iPg, pgsz, b, pp);
}
static int traceShmLock(sqlite3_file *pFile, int ofst, int n, int flags){
  TraceFile *p = (TraceFile*)pFile;
  fprintf(g_log, "SHMLOCK %-12s ofst=%d n=%d flags=0x%x\n", p->zName, ofst, n, flags);
  return p->pReal->pMethods->xShmLock(p->pReal, ofst, n, flags);
}
static void traceShmBarrier(sqlite3_file *pFile){
  TraceFile *p = (TraceFile*)pFile;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}
static int traceShmUnmap(sqlite3_file *pFile, int del){
  TraceFile *p = (TraceFile*)pFile;
  return p->pReal->pMethods->xShmUnmap(p->pReal, del);
}

static sqlite3_io_methods trace_io_methods = {
  2,                          /* iVersion */
  traceClose, traceRead, traceWrite, traceTruncate, traceSync, traceFileSize,
  traceLock, traceUnlock, traceCheckReservedLock, traceFileControl,
  traceSectorSize, traceDeviceCharacteristics,
  traceShmMap, traceShmLock, traceShmBarrier, traceShmUnmap,
  0, 0                        /* xFetch, xUnfetch (iVersion 3) */
};

static int traceOpen(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile,
                     int flags, int *pOutFlags){
  TraceFile *p = (TraceFile*)pFile;
  int rc;
  const char *zTail;
  p->pReal = (sqlite3_file*)&p[1];
  zTail = zName ? strrchr(zName, '/') : 0;
  snprintf(p->zName, sizeof(p->zName), "%s", zTail ? zTail+1 : (zName ? zName : "<temp>"));
  fprintf(g_log, "OPEN    %-12s flags=0x%x\n", p->zName, flags);
  rc = g_pRoot->xOpen(g_pRoot, zName, p->pReal, flags, pOutFlags);
  if( rc==SQLITE_OK ){
    /* match the wrapped file's iVersion so we never call a NULL method */
    trace_io_methods.iVersion = p->pReal->pMethods->iVersion < 2 ? 1 : 2;
    p->base.pMethods = &trace_io_methods;
  }
  return rc;
}
static int traceDelete(sqlite3_vfs *pVfs, const char *zName, int dirSync){
  const char *zTail = strrchr(zName, '/');
  fprintf(g_log, "DELETE  %-12s dirSync=%d  <<<<<< possible COMMIT POINT\n",
          zTail ? zTail+1 : zName, dirSync);
  return g_pRoot->xDelete(g_pRoot, zName, dirSync);
}
static int traceAccess(sqlite3_vfs *pVfs, const char *zName, int flags, int *pRes){
  int rc = g_pRoot->xAccess(g_pRoot, zName, flags, pRes);
  if( strstr(zName, "journal") || strstr(zName, "-wal") ){
    const char *zTail = strrchr(zName, '/');
    fprintf(g_log, "ACCESS  %-12s flags=%d exists=%d\n",
            zTail ? zTail+1 : zName, flags, *pRes);
  }
  return rc;
}
static int traceFullPathname(sqlite3_vfs *pVfs, const char *zName, int nOut, char *zOut){
  return g_pRoot->xFullPathname(g_pRoot, zName, nOut, zOut);
}
static void *traceDlOpen(sqlite3_vfs *p, const char *z){ return g_pRoot->xDlOpen(g_pRoot,z); }
static void traceDlError(sqlite3_vfs *p, int n, char *z){ g_pRoot->xDlError(g_pRoot,n,z); }
static void (*traceDlSym(sqlite3_vfs *p, void *pH, const char *z))(void){
  return g_pRoot->xDlSym(g_pRoot,pH,z);
}
static void traceDlClose(sqlite3_vfs *p, void *pH){ g_pRoot->xDlClose(g_pRoot,pH); }
static int traceRandomness(sqlite3_vfs *p, int n, char *z){ return g_pRoot->xRandomness(g_pRoot,n,z); }
static int traceSleep(sqlite3_vfs *p, int n){ return g_pRoot->xSleep(g_pRoot,n); }
static int traceCurrentTime(sqlite3_vfs *p, double *d){ return g_pRoot->xCurrentTime(g_pRoot,d); }
static int traceGetLastError(sqlite3_vfs *p, int n, char *z){ return g_pRoot->xGetLastError(g_pRoot,n,z); }
static int traceCurrentTimeInt64(sqlite3_vfs *p, sqlite3_int64 *pT){
  return g_pRoot->xCurrentTimeInt64(g_pRoot,pT);
}

static sqlite3_vfs trace_vfs = {
  2, 0, 1024, 0, "trace", 0,
  traceOpen, traceDelete, traceAccess, traceFullPathname,
  traceDlOpen, traceDlError, traceDlSym, traceDlClose,
  traceRandomness, traceSleep, traceCurrentTime, traceGetLastError,
  traceCurrentTimeInt64
};

int traceVfsRegister(FILE *log){
  g_log = log;
  g_pRoot = sqlite3_vfs_find(0);
  if( !g_pRoot ) return SQLITE_ERROR;
  trace_vfs.szOsFile = sizeof(TraceFile) + g_pRoot->szOsFile;
  trace_vfs.mxPathname = g_pRoot->mxPathname;
  return sqlite3_vfs_register(&trace_vfs, 0);   /* 0 = do not make default */
}

int main(int argc, char **argv){
  sqlite3 *db;
  char *zErr = 0;
  const char *zJournal = argc>1 ? argv[1] : "delete";
  char zSql[256];

  traceVfsRegister(stdout);
  remove("trace.db"); remove("trace.db-journal"); remove("trace.db-wal");

  if( sqlite3_open_v2("trace.db", &db,
        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, "trace") ){
    fprintf(stderr, "open failed: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  snprintf(zSql, sizeof(zSql), "PRAGMA journal_mode=%s;", zJournal);
  printf("\n===== %s =====\n", zSql);
  sqlite3_exec(db, zSql, 0, 0, &zErr);

  printf("\n===== CREATE TABLE =====\n");
  sqlite3_exec(db, "CREATE TABLE t(a,b);", 0, 0, &zErr);

  printf("\n===== INSERT (implicit transaction) =====\n");
  sqlite3_exec(db, "INSERT INTO t VALUES(1,'one');", 0, 0, &zErr);

  printf("\n===== EXPLICIT TRANSACTION, 2 inserts =====\n");
  sqlite3_exec(db, "BEGIN; INSERT INTO t VALUES(2,'two'); "
                   "INSERT INTO t VALUES(3,'three'); COMMIT;", 0, 0, &zErr);

  printf("\n===== SELECT =====\n");
  sqlite3_exec(db, "SELECT * FROM t;", 0, 0, &zErr);

  printf("\n===== CLOSE =====\n");
  sqlite3_close(db);
  if( zErr ) fprintf(stderr, "error: %s\n", zErr);
  return 0;
}
