# Lab 5 Copy-on-Write Fork for xv6

在传统的 `xv6` 操作系统中，`fork()` 系统调用会将父进程的所有用户空间内存复制到子进程中。这种内存复制的方式在父进程占用较大内存时，可能会导致显著的性能开销，尤其是当子进程紧接着执行 `exec()` 系统调用时，这些复制的内存通常会被丢弃，从而造成资源浪费。本实验的目的是通过实现写时复制（Copy-On-Write, COW）技术，优化 `fork()` 的性能，使得物理内存的分配和复制仅在实际需要时才进行，从而大大减少不必要的内存开销。


## 5.1 Implement copy-on-write fork

### 实验目的
本实验的目标是通过实现写时复制（Copy-On-Write, COW）的 fork() 方法，优化 xv6 中的内存管理。具体而言，COW fork() 机制通过延迟分配和复制物理内存页，减少不必要的内存复制操作。在 COW fork() 中，子进程只创建页表，其用户内存的页表项指向父进程的物理页，并且将这些页表项标记为只读。当父进程或子进程尝试写入这些共享页面时，CPU 会触发页错误。内核页错误处理程序随后为发生错误的进程分配新的物理页，并将原始页面复制到新页中，最后更新页表项以指向新页，并允许写操作。通过这种机制，可以有效减少内存浪费，并在需要时动态分配内存资源。

### 实现步骤

#### 1. 定义`ref_count[]` 

`ref_count[]`数组用于跟踪物理内存页面的被多少个进程使用；并且确保在多个进程共享同一物理页面时，只有在所有进程都释放了该页面后，物理页面才会真正被释放。私以为，整个COW功能的实现都是围绕`ref_count[]`这个引用计数机制来展开的。

定义一个`int`型的数组`ref_count`，它的元素个数与`xv6`的页面数相同，每个元素对应一个物理页面，会记录该页面被多少个进程引用。

后来在测试的时候发现，多个进程可能会同时访问或修改同一物理页面的引用计数，导致`ref_count[]`中的值出现错误，所以增加一个自旋锁保护它的读写。

```c
uint32 ref_count[PHYSTOP / PGSIZE] = {0};   // 页表引用次数，用于实现COW
struct spinlock ref_count_lock;             // 保护 ref_count 的自旋锁
```

#### 2. 在 `riscv.h`添加宏

`PTE_RSW` 是页表项中“为软件保留”的标志位，本实验中用它来表示一个页面是不是还未复制的COW页面。

`PA2INDEX`宏用于把物理地址转换为页编号。知道物理地址的页编号，就能够去`ref_count[index]`里面知道引用计次了。

```c
#define PTE_RSW (1L << 8) // reserved for software, 标记COW映射
#define PA2INDEX(pa) (((uint64)(pa)) / PGSIZE) // 从物理地址获取页面编号
```

#### 3. 拷贝页面时，先不复制

在 `vm.c` 中，修改了 `uvmcopy` 函数，使其支持写时复制。在拷贝页表时，我们将页面标记为只读，并设置 COW 标记，同时增加引用计数。

```c
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");

    // 若父进程内存页设置了 PTE_W ，则父子进程都清除 PTE_W 并设置 COW 位
    if (*pte & PTE_W) 
    {
      *pte |= PTE_RSW;
      *pte -= PTE_W;
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);

    // 引用计数++
    acquire(&ref_count_lock);
    ++ref_count[PA2INDEX(pa)];
    release(&ref_count_lock);

    // uvmcopy 中不再需要 kalloc
    // if ((mem = kalloc()) == 0) goto err;
    // memmove(mem, (char*)pa, PGSIZE);

    if(mappages(new, i, PGSIZE, (uint64)pa, flags) != 0){
      // kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
```

#### 4. 分配新页的情形：用户态写入

`fork()`后再写内存，会试图写入没有`PTE_W`的页，发生页面错误。在 `usertrap` 函数中，我们通过`r_scause()`的值来判断页面错误导致的陷入，然后使用 `cowhandler` 来处理写时复制逻辑。

`scause`寄存器记录中断或异常的原因，值为15 表示 Store/AMO page fault：

![](img/05/scause-values.png)

`stval`寄存器在发生异常时存储与异常相关的地址或其他信息。对于页面异常，`stval` 中保存的值就是引发缺页异常的虚拟地址：

![](img/05/stval.png)

`scause` 存储异常的原因，而 `stval` 提供了与该异常相关的额外上下文信息。所以通过`r_scause()`和`r_stval()`的组合使用，我们能够在`usertrap`中处理COW模式下的页面错误。

```c
  // 当COW页面发生写操作时，产生页面错误
  else if (r_scause() == 15) // scause 寄存器的值为15 表示 Store/AMO page fault
  {
    uint64 va = r_stval();
    if (va >= p->sz)
      setkilled(p);
    int ret = cowhandler(p->pagetable, va);
    if (ret != 0)
      setkilled(p);
  }
```

增加 `cowhandler` 函数，用于处理当进程试图写入只读页面时触发的页面错误。此函数会分配新的物理内存并将数据从旧页面复制到新页面，并且将页表项更新为可写。

```c
int cowhandler(pagetable_t pagetable, uint64 va)
{
  char *mem;
  // 如果虚拟地址va超过最大虚拟地址MAXVA，返回错误
  if (va >= MAXVA)
    return -1;
  // 获取虚拟地址va对应的页表项指针PTE
  pte_t *pte = walk(pagetable, va, 0);
  // 如果没有找到对应的页表项，返回错误
  if (pte == 0)
    return -1;
  // 检查PTE是否有效，即检查PTE是否具有COW标志位、用户态访问权限和有效位
  if ((*pte & PTE_RSW) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_V) == 0)
    return -1;
  // 为新页面分配物理内存，如果分配失败则返回错误
  if ((mem = kalloc()) == 0)
    return -1;
  // 获取旧的物理地址
  uint64 pa = PTE2PA(*pte);
  // 将旧页面的数据复制到新分配的页面中
  memmove((char *)mem, (char *)pa, PGSIZE);
  // 注意：
  // 因为分配了新的页面，所以要减少旧页面的引用计数
  kfree((void *)pa);
  // 获取旧的页表项标志
  uint flags = PTE_FLAGS(*pte);
  // 将PTE_W标志位设置为1，并将页表项指向新分配的物理页面(mem)
  *pte = (PA2PTE(mem) | flags | PTE_W);
  // 将COW标志位PTE_RSW清除
  *pte &= ~PTE_RSW;
  // 返回0表示成功
  return 0;
}
```

#### 5. 分配新页的情形：内核态写回

`copyout`负责把数据正确地拷贝回用户态，而此时要写的页可能没有`PTE_W`，所以需要修改`copyout`，在内核态给进程页表写数据的时候，进行新页的分配和数据的复制、更改。

```c
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) // 在此处不检查 PTE_W。因为写时复制的页本来就设置的是不可写的。
      return -1;
    pa0 = PTE2PA(*pte);

    struct proc *p = myproc();
    if (*pte == 0) // 如果PTE无效（为0），则标记进程为killed并返回错误
    {
      setkilled(p);
      return -1;
    }

    // 检查是否是COW页面
    // 如果页表项中PTE_W标志位没有设置（即页面不可写），但页面有COW标记（通过PTE_RSW标志位检测），需要触发写时复制。
    if ((*pte & PTE_W) == 0)
    {
      if (*pte & PTE_RSW)
      {
        // 执行写时复制
        char *mem = kalloc();
        if (mem == 0) // 如果分配新的物理页失败，标记进程为killed并返回错误
        {
          setkilled(p);
          return -1;
        }
        memmove(mem, (char *)pa0, PGSIZE);      // 将旧页面的数据复制到新分配的页面中
        uint flags = PTE_FLAGS(*pte);           // 保存旧的页表项标志
        uvmunmap(pagetable, va0, 1, 1);         // 取消映射旧的页表项，同时释放旧的物理页面
        *pte = (PA2PTE(mem) | flags | PTE_W);   // 更新页表项，使其指向新分配的页面，并设置可写标志
        *pte &= ~PTE_RSW;                       // 清除COW标志位
        pa0 = (uint64)mem;                      // 更新 pa0 到新的物理地址
      }
      else
      {
        return -1; // 如果不是COW页面但仍不可写，返回错误
      }
    }


    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
```

#### 6. 在`kalloc`中初始化`ref_count`

一个页面被`kalloc`初次分配的时候，引用计数为1。

```c
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
    uint32 index = PA2INDEX(r);
    acquire(&ref_count_lock);
    ref_count[index] = 1; // 新分配的页面初始引用计数为 1
    release(&ref_count_lock);
  }

  return (void*)r;
}
```

#### 7. 修改`kfree`

当`ref_count`大于0的时候，释放页面意味着减少该页面的`ref_count`；当减少`ref_count`后，该页面没有被引用（`ref_count`等于0），那么才真正地释放内存。

```c
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 获取页面索引
  uint32 index = PA2INDEX(pa);

  // 减少引用计数
  acquire(&ref_count_lock);
  if (ref_count[index] > 0)
    ref_count[index]--;
  else 
    ref_count[index] = 0;
  uint32 this_count = ref_count[index];
  release(&ref_count_lock);

  // 仅当引用计数为零时释放页面
  if (this_count == 0)
  {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
}
```



### 实验中遇到的问题和解决方法

**并发访问导致`ref_count[]`出错**

测试的时候，发现测试用例中有多个进程同时访问或修改同一物理页面的引用计数的情形，导致`ref_count[]`中的值出现错误。

举例来说，一个父进程的多个子进程在短时间内创建孙子进程，会存在两个进程同时读取同一个 `ref_count` 值并进行操作，然后写回修改后的值，最终的 `ref_count` 可能只反映了一次更新，而丢失了其他更新，导致`ref_count[index]`的值偏小。之后这些进程依次销毁，偏小的`ref_count[index]`会提前减小到0，导致`kfree`提前调用，过早释放了别的进程仍然要使用的内存，引发kernel panic。

解决办法就是给`ref_count[]`加一个自旋锁来保护它的读写。

### 实验心得

Copy-on-write机制非常实用，在创建子进程时进行了更高效的内存管理。测试程序中，一个进程占用了超过一半的物理内存，又通过fork()创建子进程，原本这会因为内存不足而直接导致`fork()`失败，而实现`COW`机制后就能够应对这样的情形，这是操作系统内存虚拟化带来的实打实的益处。

本实验还让我了解到了`riscv`下`scause`寄存器和`stval`寄存器的作用。`scause` 存储异常的原因，而 `stval` 提供了与该异常相关的额外上下文信息。所以通过`r_scause()`和`r_stval()`的组合使用，我们能够在`usertrap`中处理COW模式下的页面错误。
