/* minibtree.c — a miniature SQLite-shaped B+tree you can hold in your head.
 *
 * Mirrors SQLite's real layout so the concepts transfer:
 *   - slotted pages: header, 2-byte cell-pointer array growing DOWN from the
 *     header, cell bodies growing UP from the end of the page
 *   - big-endian on-page integers
 *   - table b-tree shape: interior cells are (child, key) with NO payload,
 *     leaves hold (key, payload); interior key K means "child holds keys <= K"
 *   - page type bytes reuse SQLite's: 0x05 interior table, 0x0d leaf table
 *   - a root split creates a new level (SQLite: balance_deeper)
 *
 * Simplifications vs. real SQLite: fixed small pages, in-memory "file", no
 * overflow pages, no delete/freeblocks, split at the midpoint instead of
 * balance_nonroot's 3-sibling redistribution.
 *
 * Build: cc -Wall -O0 -g -o minibtree minibtree.c
 * Run:   ./minibtree seq 2000 ; ./minibtree rand 2000 ; ./minibtree seq 20
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define PAGE_SIZE   256
#define MAX_PAGES   65536

static unsigned char g_pages[MAX_PAGES][PAGE_SIZE];
static int  g_nPage = 0;              /* pages allocated; page numbers are 1-based */
static int  g_root  = 0;
static long g_splits = 0;

/* ---------- big-endian helpers ---------- */
static void put16(unsigned char *p, unsigned v){ p[0]=(unsigned char)(v>>8); p[1]=v&0xff; }
static unsigned get16(const unsigned char *p){ return (p[0]<<8)|p[1]; }
static void put32(unsigned char *p, uint32_t v){
  p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16);
  p[2]=(unsigned char)(v>>8);  p[3]=(unsigned char)v;
}
static uint32_t get32(const unsigned char *p){
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* ---------- page header (mirrors SQLite) ----------
 *  0      type: 0x05 interior, 0x0d leaf
 *  3..4   nCell
 *  5..6   cell content area start
 *  8..11  right-most child (interior only)
 */
static unsigned char *pg(int n){ return g_pages[n-1]; }
static int  pType(int n){ return pg(n)[0]; }
static int  isLeaf(int n){ return pType(n)==0x0d; }
static int  nCell(int n){ return (int)get16(pg(n)+3); }
static void setNCell(int n, int v){ put16(pg(n)+3, (unsigned)v); }
static int  cellTop(int n){ return (int)get16(pg(n)+5); }
static void setCellTop(int n, int v){ put16(pg(n)+5, (unsigned)v); }
static int  hdrSize(int n){ return isLeaf(n) ? 8 : 12; }
static unsigned char *cellPtrArray(int n){ return pg(n)+hdrSize(n); }
static int  cellOff(int n, int i){ return (int)get16(cellPtrArray(n)+i*2); }
static unsigned char *cell(int n, int i){ return pg(n)+cellOff(n,i); }
static int  rightChild(int n){ return (int)get32(pg(n)+8); }
static void setRightChild(int n, int c){ put32(pg(n)+8, (uint32_t)c); }
static int  freeSpace(int n){ return cellTop(n) - (hdrSize(n) + nCell(n)*2); }

static int newPage(int type){
  int n = ++g_nPage;
  unsigned char *p;
  assert( n < MAX_PAGES );
  p = pg(n);
  memset(p, 0, PAGE_SIZE);
  p[0] = (unsigned char)type;
  setNCell(n, 0);
  setCellTop(n, PAGE_SIZE);
  return n;
}

/* ---------- cells ----------
 * leaf cell:     4-byte key | 2-byte payload len | payload
 * interior cell: 4-byte child | 4-byte key
 */
static int cellSize(int n, int i){
  return isLeaf(n) ? 4 + 2 + (int)get16(cell(n,i)+4) : 8;
}
static uint32_t cellKey(int n, int i){
  unsigned char *c = cell(n,i);
  return isLeaf(n) ? get32(c) : get32(c+4);
}
static int cellChild(int n, int i){ return (int)get32(cell(n,i)); }

/* Insert a raw cell body at slot i. Caller guarantees room. */
static void insertCellAt(int n, int i, const unsigned char *body, int sz){
  int top = cellTop(n) - sz;
  unsigned char *a = cellPtrArray(n);
  int nc = nCell(n);
  assert( top >= hdrSize(n) + (nc+1)*2 );
  memcpy(pg(n)+top, body, (size_t)sz);
  memmove(a + (i+1)*2, a + i*2, (size_t)(nc-i)*2);   /* shift pointer array right */
  put16(a + i*2, (unsigned)top);
  setCellTop(n, top);
  setNCell(n, nc+1);
}

/* First cell index whose key >= k (so: the child that owns k). */
static int pageSearch(int n, uint32_t k, int *exact){
  int lwr = 0, upr = nCell(n)-1;
  *exact = 0;
  while( lwr <= upr ){
    int mid = (lwr+upr)/2;
    uint32_t mk = cellKey(n, mid);
    if( mk < k )      lwr = mid+1;
    else if( mk > k ) upr = mid-1;
    else { *exact = 1; return mid; }
  }
  return lwr;
}

/* ---------- split ----------
 * Splits page n in half. Returns the new right page; *pDiv receives the
 * separator key: every key in the left page is <= *pDiv.
 * Leaf split copies the separator (B+tree); interior split PROMOTES it
 * (the promoted cell's child becomes the left page's right-most child).
 */
static int splitPage(int n, uint32_t *pDiv){
  int right, nc = nCell(n), half = nc/2, i, oldRight = 0, leftLast = 0;
  int type = pType(n);

  g_splits++;
  if( type==0x05 ) oldRight = rightChild(n);

  right = newPage(type);

  /* Build the two halves in scratch pages, then copy back. This is a crude
   * stand-in for defragmentPage() + balance_nonroot()'s cell redistribution. */
  {
    int scratchL = newPage(type), scratchR = newPage(type);
    if( type==0x0d ){
      for(i=0; i<half; i++)  insertCellAt(scratchL, nCell(scratchL), cell(n,i), cellSize(n,i));
      for(i=half; i<nc; i++) insertCellAt(scratchR, nCell(scratchR), cell(n,i), cellSize(n,i));
      *pDiv = cellKey(n, half-1);
    }else{
      for(i=0; i<half-1; i++) insertCellAt(scratchL, nCell(scratchL), cell(n,i), 8);
      *pDiv    = cellKey(n, half-1);
      leftLast = cellChild(n, half-1);            /* promoted cell's child */
      for(i=half; i<nc; i++)  insertCellAt(scratchR, nCell(scratchR), cell(n,i), 8);
      setRightChild(scratchL, leftLast);
      setRightChild(scratchR, oldRight);
    }
    memcpy(pg(n),     pg(scratchL), PAGE_SIZE);
    memcpy(pg(right), pg(scratchR), PAGE_SIZE);
    g_nPage -= 2;                                  /* release the scratch pages */
  }
  return right;
}

/* After a child of page p split, point the old entry at the NEW right page and
 * insert a fresh entry (child, childDiv) in front of it. */
static void fixupChild(int p, int child, uint32_t childDiv, int newRight){
  unsigned char body[8];
  int i, nc = nCell(p);
  put32(body,   (uint32_t)child);
  put32(body+4, childDiv);
  for(i=0; i<nc; i++){
    if( cellChild(p,i)==child ){
      put32(cell(p,i), (uint32_t)newRight);        /* old entry now covers the right half */
      insertCellAt(p, i, body, 8);
      return;
    }
  }
  assert( rightChild(p)==child );
  insertCellAt(p, nc, body, 8);
  setRightChild(p, newRight);
}

static int pageHasChild(int p, int child){
  int i;
  if( rightChild(p)==child ) return 1;
  for(i=0; i<nCell(p); i++) if( cellChild(p,i)==child ) return 1;
  return 0;
}

/* Returns 0, or the new right page if page n split (then *pDiv is set). */
static int insertInto(int n, uint32_t key, const char *payload, int nPayload,
                      uint32_t *pDiv){
  int exact, i;
  if( isLeaf(n) ){
    unsigned char body[PAGE_SIZE];
    int sz = 4 + 2 + nPayload;
    i = pageSearch(n, key, &exact);
    if( exact ) return 0;                          /* duplicate key: ignore */
    put32(body, key);
    put16(body+4, (unsigned)nPayload);
    memcpy(body+6, payload, (size_t)nPayload);
    if( freeSpace(n) < sz + 2 ){
      uint32_t div;
      int right = splitPage(n, &div);
      int target = (key > div) ? right : n;
      i = pageSearch(target, key, &exact);
      insertCellAt(target, i, body, sz);
      *pDiv = div;
      return right;
    }
    insertCellAt(n, i, body, sz);
    return 0;
  }else{
    int child, newRight;
    uint32_t childDiv = 0;
    i = pageSearch(n, key, &exact);                /* key <= cellKey(i) => go left */
    child = (i >= nCell(n)) ? rightChild(n) : cellChild(n, i);
    newRight = insertInto(child, key, payload, nPayload, &childDiv);
    if( newRight==0 ) return 0;

    if( freeSpace(n) < 8 + 2 ){
      uint32_t div;
      int right = splitPage(n, &div);
      int target = pageHasChild(n, child) ? n : right;
      fixupChild(target, child, childDiv, newRight);
      *pDiv = div;
      return right;
    }
    fixupChild(n, child, childDiv, newRight);
    return 0;
  }
}

static void btInsert(uint32_t key, const char *payload){
  uint32_t div = 0;
  int right;
  if( g_root==0 ) g_root = newPage(0x0d);
  right = insertInto(g_root, key, payload, (int)strlen(payload), &div);
  if( right ){
    /* SQLite's balance_deeper: the tree grows one level; in real SQLite the
     * ROOT PAGE NUMBER must not change, so it copies the root down instead. */
    int oldRoot = g_root;
    int newRoot = newPage(0x05);
    unsigned char body[8];
    put32(body,   (uint32_t)oldRoot);
    put32(body+4, div);
    insertCellAt(newRoot, 0, body, 8);
    setRightChild(newRoot, right);
    g_root = newRoot;
  }
}

static const unsigned char *btSearch(uint32_t key, int *pLen){
  int n = g_root, exact, i;
  while( n && !isLeaf(n) ){
    i = pageSearch(n, key, &exact);
    n = (i >= nCell(n)) ? rightChild(n) : cellChild(n, i);
  }
  if( !n ) return 0;
  i = pageSearch(n, key, &exact);
  if( !exact ) return 0;
  *pLen = (int)get16(cell(n,i)+4);
  return cell(n,i)+6;
}

/* ---------- reporting ---------- */
static int gDepth, gLeaves, gInner, gCells, gUsed;
static uint32_t gPrevKey; static int gOrderOk;
static void walk(int n, int depth){
  int i;
  if( depth > gDepth ) gDepth = depth;
  if( isLeaf(n) ){
    gLeaves++;
    gCells += nCell(n);
    gUsed  += PAGE_SIZE - freeSpace(n) - hdrSize(n);
    for(i=0; i<nCell(n); i++){
      uint32_t k = cellKey(n,i);
      if( k <= gPrevKey && !(gPrevKey==0 && k==0) ) gOrderOk = 0;
      gPrevKey = k;
    }
    return;
  }
  gInner++;
  for(i=0; i<nCell(n); i++) walk(cellChild(n,i), depth+1);
  walk(rightChild(n), depth+1);
}
static void dump(int n, int depth){
  int i;
  printf("%*spage %d %-8s nCell=%2d free=%3d", depth*2, "", n,
         isLeaf(n)?"LEAF":"INTERIOR", nCell(n), freeSpace(n));
  if( !isLeaf(n) ) printf(" right=%d", rightChild(n));
  printf("  keys:");
  for(i=0; i<nCell(n) && i<10; i++) printf(" %u", cellKey(n,i));
  if( nCell(n)>10 ) printf(" ...");
  printf("\n");
  if( !isLeaf(n) ){
    for(i=0; i<nCell(n); i++) dump(cellChild(n,i), depth+1);
    dump(rightChild(n), depth+1);
  }
}

int main(int argc, char **argv){
  int mode = (argc>1 && strcmp(argv[1],"rand")==0);
  int N = argc>2 ? atoi(argv[2]) : 200;
  int i, missing = 0, nDistinct = 0;
  char buf[24];
  uint32_t *keys = malloc(sizeof(uint32_t)*(size_t)N);

  srand(12345);
  for(i=0; i<N; i++) keys[i] = mode ? (uint32_t)(rand()%(N*4)+1) : (uint32_t)(i+1);
  for(i=0; i<N; i++){
    snprintf(buf, sizeof(buf), "v%u", keys[i]);
    btInsert(keys[i], buf);
  }
  if( N <= 40 ) dump(g_root, 0);

  gDepth=gLeaves=gInner=gCells=gUsed=0; gPrevKey=0; gOrderOk=1;
  walk(g_root, 1);
  for(i=0; i<N; i++){
    int len; const unsigned char *p = btSearch(keys[i], &len);
    if( !p ) missing++;
  }
  /* count distinct keys inserted */
  {
    uint32_t *s = malloc(sizeof(uint32_t)*(size_t)N);
    int j;
    memcpy(s, keys, sizeof(uint32_t)*(size_t)N);
    for(i=0;i<N;i++) for(j=i+1;j<N;j++) if(s[j]<s[i]){uint32_t t=s[i];s[i]=s[j];s[j]=t;}
    for(i=0;i<N;i++) if(i==0||s[i]!=s[i-1]) nDistinct++;
    free(s);
  }

  printf("mode=%-10s inserted=%d distinct=%d | pages=%d depth=%d inner=%d leaves=%d "
         "cells=%d | avg leaf fill=%.1f%% splits=%ld\n",
         mode?"random":"sequential", N, nDistinct, g_nPage, gDepth, gInner, gLeaves,
         gCells, gLeaves ? 100.0*gUsed/(gLeaves*(PAGE_SIZE-8)) : 0.0, g_splits);
  printf("checks: lookups missing=%d  cells==distinct? %s  keys in order? %s\n",
         missing, gCells==nDistinct ? "YES" : "NO", gOrderOk ? "YES" : "NO");
  free(keys);
  return (missing || gCells!=nDistinct || !gOrderOk) ? 1 : 0;
}
