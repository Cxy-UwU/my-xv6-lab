// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13
#define hash(num) num % NBUCKETS
struct
{
  struct spinlock bucket_lock[NBUCKETS];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf bucket_head[NBUCKETS];
} bcache;

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i < NBUCKETS; i++){
    initlock(&bcache.bucket_lock[i], "bcache"); 
  }

  // Create linked list of buffers
  for (int i = 0; i < NBUCKETS; i++){
    bcache.bucket_head[i].prev = &bcache.bucket_head[i]; // 初始化空循环链表
    bcache.bucket_head[i].next = &bcache.bucket_head[i];
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.bucket_head[hash(b->blockno)].next; // 根据blockno，把buffer放入不同的桶中
    b->prev = &bcache.bucket_head[hash(b->blockno)];
    initsleeplock(&b->lock, "buffer");
    bcache.bucket_head[hash(b->blockno)].next->prev = b;
    bcache.bucket_head[hash(b->blockno)].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int id = hash(blockno);

  acquire(&bcache.bucket_lock[id]);

  // Is the block already cached?
  for (b = bcache.bucket_head[id].next; b != &bcache.bucket_head[id]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.bucket_lock[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for (int i = (id + 1) % NBUCKETS; i != id; i = (i + 1) % NBUCKETS)
  {
    acquire(&bcache.bucket_lock[i]);
    struct buf *b;
    for (b = bcache.bucket_head[i].prev; b != &bcache.bucket_head[i]; b = b->prev)
    {
      if (b->refcnt == 0)
      {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // Disconnect this buffer from its current position
        b->prev->next = b->next;
        b->next->prev = b->prev;

        release(&bcache.bucket_lock[i]);

        // Insert this buffer into the new bucket's chain
        b->prev = &bcache.bucket_head[id];
        b->next = bcache.bucket_head[id].next;
        b->next->prev = b;
        b->prev->next = b;
        release(&bcache.bucket_lock[id]);

        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.bucket_lock[i]);
  }

  panic("bget: no buffers"); // If all buffers are in use, panic
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  int id = hash(b->blockno);

  releasesleep(&b->lock);

  acquire(&bcache.bucket_lock[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.bucket_head[id].next;
    b->prev = &bcache.bucket_head[id];
    bcache.bucket_head[id].next->prev = b;
    bcache.bucket_head[id].next = b;
  }

  release(&bcache.bucket_lock[id]);
}

void
bpin(struct buf *b) {
  int id = hash(b->blockno);
  acquire(&bcache.bucket_lock[id]);
  b->refcnt++;
  release(&bcache.bucket_lock[id]);
}

void
bunpin(struct buf *b) {
  int id = hash(b->blockno);
  acquire(&bcache.bucket_lock[id]);
  b->refcnt--;
  release(&bcache.bucket_lock[id]);
}


