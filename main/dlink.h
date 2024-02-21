#ifndef __DLINK_H__
#define __DLINK_H__ 1

// Doubly linked lists
typedef struct sDLEntry {
  struct sDLEntry *prevP;
  struct sDLEntry *nextP;
} tDLEntry;


// Insert `newP` node into a DL immediately before node `p`.
static inline void DLInsertBefore(tDLEntry *p, tDLEntry *newP) {
  newP->nextP = p;
  newP->prevP = p->prevP;
  p->prevP->nextP = newP;
  p->prevP = newP;
}


// Insert `newP` node into a DL immediately after node `p`.
static inline void DLInsertAfter(tDLEntry *p, tDLEntry *newP) {
  newP->prevP = p;
  newP->nextP = p->nextP;
  p->nextP->prevP = newP;
  p->nextP = newP;
}

#endif
