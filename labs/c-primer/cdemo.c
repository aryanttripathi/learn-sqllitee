/* cdemo.c — every C technique SQLite relies on, demonstrated and printed.
 * Build: cc -Wall -O0 -g -o cdemo cdemo.c && ./cdemo
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ---------- 1. sized integer types ---------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int64_t  i64;
typedef uint64_t u64;

/* ---------- 2. struct layout & padding ---------- */
struct BadLayout  { u8 a; u32 b; u8 c; u32 d; };   /* 16 bytes: padding waste */
struct GoodLayout { u32 b; u32 d; u8 a; u8 c; };   /* 12 bytes: same fields */

/* ---------- 3. bit flags ---------- */
#define MEM_Null  0x0001
#define MEM_Str   0x0002
#define MEM_Int   0x0004
#define MEM_Real  0x0008
#define MEM_Blob  0x0010
#define MEM_Dyn   0x1000

/* ---------- 4. union: one slot, many types (this is Mem.u) ---------- */
typedef struct Mem Mem;
struct Mem {
  union MemValue { double r; i64 i; int nZero; } u;
  u16 flags;
  int n;
  char *z;
};

/* ---------- 5. "subclassing" by first member (this is sqlite3_file) ---------- */
typedef struct File File;
typedef struct FileMethods FileMethods;
struct FileMethods {                      /* the vtable */
  int (*xRead)(File*, void*, int, i64);
  int (*xWrite)(File*, const void*, int, i64);
  int (*xClose)(File*);
};
struct File {                             /* the "base class" */
  const FileMethods *pMethods;
};
typedef struct RealFile RealFile;
struct RealFile {
  File base;            /* MUST be first: we get cast to File* */
  FILE *fp;
  char zName[64];
  long nRead, nWritten;
};

static int realRead(File *pF, void *buf, int n, i64 off){
  RealFile *p = (RealFile*)pF;            /* the downcast that first-member makes legal */
  p->nRead += n;
  fseek(p->fp, (long)off, SEEK_SET);
  return (int)fread(buf, 1, (size_t)n, p->fp);
}
static int realWrite(File *pF, const void *buf, int n, i64 off){
  RealFile *p = (RealFile*)pF;
  p->nWritten += n;
  fseek(p->fp, (long)off, SEEK_SET);
  return (int)fwrite(buf, 1, (size_t)n, p->fp);
}
static int realClose(File *pF){
  RealFile *p = (RealFile*)pF;
  fclose(p->fp);
  return 0;
}
static const FileMethods realMethods = { realRead, realWrite, realClose };

/* ---------- 6. flexible array member (the a[1] trick) ---------- */
typedef struct ExprList ExprList;
struct ExprList {
  int nExpr;
  struct ExprItem { int id; char name[8]; } a[1];   /* grown by realloc */
};
static ExprList *exprListAppend(ExprList *p, int id, const char *zName){
  int n = p ? p->nExpr : 0;
  size_t sz = sizeof(ExprList) + (size_t)n * sizeof(struct ExprItem);
  ExprList *pNew = realloc(p, sz);
  if( !pNew ) return p;
  pNew->nExpr = n + 1;
  pNew->a[n].id = id;
  snprintf(pNew->a[n].name, sizeof(pNew->a[n].name), "%s", zName);
  return pNew;
}

/* ---------- 7. big-endian encode/decode by hand ---------- */
static void put4be(u8 *p, u32 v){
  p[0] = (u8)(v>>24); p[1] = (u8)(v>>16); p[2] = (u8)(v>>8); p[3] = (u8)v;
}
static u32 get4be(const u8 *p){
  return ((u32)p[0]<<24) | ((u32)p[1]<<16) | ((u32)p[2]<<8) | p[3];
}

/* ---------- 8. sign extension on a hand-rolled 3-byte integer ---------- */
static i64 get3be_signed(const u8 *p){
  i64 v = (p[0] & 0x80) ? -1 : 0;        /* seed with all-ones if negative */
  v = (v<<8) | p[0];
  v = (v<<8) | p[1];
  v = (v<<8) | p[2];
  return v;
}

/* ---------- 9. varint (SQLite's actual encoding) ---------- */
static int putVarint(u8 *p, u64 v){
  int i, j, n = 0;
  u8 buf[10];
  if( v & (((u64)0xff000000)<<32) ){      /* 9-byte form */
    p[8] = (u8)v; v >>= 8;
    for(i=7; i>=0; i--){ p[i] = (u8)((v & 0x7f) | 0x80); v >>= 7; }
    return 9;
  }
  do { buf[n++] = (u8)((v & 0x7f) | 0x80); v >>= 7; } while( v!=0 );
  buf[0] &= 0x7f;                          /* clear the continuation bit on the LAST byte */
  for(i=0, j=n-1; j>=0; j--, i++) p[i] = buf[j];
  return n;
}
static int getVarint(const u8 *p, u64 *v){
  u64 x = 0; int i;
  for(i=0; i<8; i++){
    x = (x<<7) | (p[i] & 0x7f);
    if( (p[i] & 0x80)==0 ){ *v = x; return i+1; }
  }
  x = (x<<8) | p[8];
  *v = x;
  return 9;
}

/* ---------- 10. goto-cleanup error handling ---------- */
static int doWork(int failAt){
  char *a = 0, *b = 0;
  int rc = 0;
  a = malloc(16); if( !a ){ rc = 7; goto done; }
  if( failAt==1 ){ rc = 1; goto done; }
  b = malloc(16); if( !b ){ rc = 7; goto done; }
  if( failAt==2 ){ rc = 2; goto done; }
done:
  free(b);                 /* free(NULL) is legal and does nothing */
  free(a);
  return rc;
}

/* ---------- 11. memmove vs memcpy ---------- */
static void shiftDemo(void){
  u8 buf[10] = {0,1,2,3,4,5,6,7,8,9};
  memmove(buf+2, buf, 6);   /* overlapping: only memmove is defined here */
  printf("  after memmove(buf+2,buf,6): ");
  for(int i=0;i<10;i++) printf("%d ", buf[i]);
  printf("\n");
}

int main(void){
  printf("=== 1. type sizes on this machine ===\n");
  printf("  u8=%zu u16=%zu u32=%zu i64=%zu  void*=%zu  double=%zu\n",
         sizeof(u8), sizeof(u16), sizeof(u32), sizeof(i64), sizeof(void*), sizeof(double));

  printf("\n=== 2. struct padding ===\n");
  printf("  BadLayout  {u8,u32,u8,u32} = %zu bytes\n", sizeof(struct BadLayout));
  printf("  GoodLayout {u32,u32,u8,u8} = %zu bytes  <- same data, fewer bytes\n",
         sizeof(struct GoodLayout));
  printf("  offsetof(BadLayout,b)=%zu  offsetof(GoodLayout,a)=%zu\n",
         offsetof(struct BadLayout,b), offsetof(struct GoodLayout,a));

  printf("\n=== 3. bit flags ===\n");
  {
    u16 flags = MEM_Int;
    printf("  flags=0x%04x  isInt=%d isStr=%d\n", flags, !!(flags&MEM_Int), !!(flags&MEM_Str));
    flags |= MEM_Str;                       /* value now cached as BOTH int and text */
    printf("  after |= MEM_Str: 0x%04x  isInt=%d isStr=%d\n",
           flags, !!(flags&MEM_Int), !!(flags&MEM_Str));
    flags &= ~MEM_Int;                      /* clear one bit */
    printf("  after &= ~MEM_Int: 0x%04x  isInt=%d isStr=%d\n",
           flags, !!(flags&MEM_Int), !!(flags&MEM_Str));
  }

  printf("\n=== 4. union ===\n");
  {
    Mem m;
    memset(&m, 0, sizeof(m));
    m.u.i = 42; m.flags = MEM_Int;
    printf("  as int : %lld  (sizeof union = %zu, sizeof Mem = %zu)\n",
           (long long)m.u.i, sizeof(m.u), sizeof(Mem));
    m.u.r = 3.5; m.flags = MEM_Real;
    printf("  as real: %g   <- same 8 bytes, flags say how to read them\n", m.u.r);
  }

  printf("\n=== 5. first-member subclassing (the VFS pattern) ===\n");
  {
    RealFile rf;
    File *pGeneric;
    char tmp[32] = "hello sqlite";
    char back[32] = {0};
    memset(&rf, 0, sizeof(rf));
    snprintf(rf.zName, sizeof(rf.zName), "demo.bin");
    rf.fp = fopen("demo.bin", "w+b");
    rf.base.pMethods = &realMethods;
    pGeneric = (File*)&rf;                  /* upcast: legal, same address */
    printf("  &rf = %p   (File*)&rf = %p   <- IDENTICAL addresses\n",
           (void*)&rf, (void*)pGeneric);
    pGeneric->pMethods->xWrite(pGeneric, tmp, 12, 0);
    pGeneric->pMethods->xRead(pGeneric, back, 12, 0);
    printf("  wrote/read through the generic pointer: \"%s\"\n", back);
    printf("  private state still reachable after downcast: name=%s nRead=%ld nWritten=%ld\n",
           rf.zName, rf.nRead, rf.nWritten);
    pGeneric->pMethods->xClose(pGeneric);
    remove("demo.bin");
  }

  printf("\n=== 6. flexible array member ===\n");
  {
    ExprList *p = malloc(sizeof(ExprList));
    p->nExpr = 0;
    p = exprListAppend(p, 10, "age");
    p = exprListAppend(p, 11, "name");
    p = exprListAppend(p, 12, "city");
    printf("  nExpr=%d  alloc=%zu bytes  items:", p->nExpr,
           sizeof(ExprList) + (size_t)(p->nExpr-1)*sizeof(struct ExprItem));
    for(int i=0;i<p->nExpr;i++) printf(" (%d,%s)", p->a[i].id, p->a[i].name);
    printf("\n  header and items are ONE allocation, contiguous in memory\n");
    free(p);
  }

  printf("\n=== 7. big-endian on disk ===\n");
  {
    u8 page[8];
    u32 v = 0x12345678;
    put4be(page, v);
    printf("  0x%08x stored as bytes: %02x %02x %02x %02x  (same on every CPU)\n",
           v, page[0], page[1], page[2], page[3]);
    printf("  read back: 0x%08x\n", get4be(page));
    {
      u32 native = v;
      u8 *q = (u8*)&native;
      printf("  the SAME value in NATIVE memory order here: %02x %02x %02x %02x %s\n",
             q[0], q[1], q[2], q[3], q[0]==0x12 ? "(big-endian CPU)" : "(little-endian CPU)");
    }
  }

  printf("\n=== 8. sign extension (serial type 3 = 3-byte int) ===\n");
  {
    u8 pos[3] = {0x00, 0x00, 0x7b};   /*  123 */
    u8 neg[3] = {0xff, 0xff, 0x85};   /* -123 */
    printf("  00 00 7b -> %lld\n", (long long)get3be_signed(pos));
    printf("  ff ff 85 -> %lld   <- seeded v with -1 so the top bits are ones\n",
           (long long)get3be_signed(neg));
  }

  printf("\n=== 9. varint round-trip ===\n");
  {
    u64 vals[] = {0, 1, 127, 128, 200, 16383, 16384, 1000000, (u64)-1};
    for(unsigned k=0;k<sizeof(vals)/sizeof(vals[0]);k++){
      u8 buf[9]; u64 out = 0;
      int n = putVarint(buf, vals[k]);
      int m = getVarint(buf, &out);
      printf("  %-20llu -> %d bytes:", (unsigned long long)vals[k], n);
      for(int i=0;i<n;i++) printf(" %02x", buf[i]);
      printf("   back=%llu %s\n", (unsigned long long)out,
             (out==vals[k] && n==m) ? "OK" : "MISMATCH");
    }
  }

  printf("\n=== 10. goto-cleanup ===\n");
  printf("  doWork(0)=%d doWork(1)=%d doWork(2)=%d  (no leaks in any path)\n",
         doWork(0), doWork(1), doWork(2));

  printf("\n=== 11. memmove vs memcpy ===\n");
  shiftDemo();

  printf("\n=== 12. pointer arithmetic on a page buffer ===\n");
  {
    u8 page[64];
    u8 *hdr = page;            /* the page header */
    u8 *cellIdx = page + 8;    /* the cell pointer array starts after the 8-byte header */
    memset(page, 0, sizeof(page));
    page[0] = 0x0d;                       /* leaf table page */
    put4be(page+3, 0);                    /* (bytes 3..4 would be nCell in the real format) */
    printf("  page=%p  hdr=%p  cellIdx=%p  cellIdx-page=%td\n",
           (void*)page, (void*)hdr, (void*)cellIdx, cellIdx - page);
    printf("  p[i] means *(p+i); adding to a u8* advances by EXACTLY i bytes\n");
  }
  return 0;
}
