# Lab 10 mmap

### 实验目的

本实验的目的是在xv6操作系统中实现`mmap`和`munmap`系统调用，以支持将文件映射到进程的地址空间。通过实现这些系统调用，我们可以使进程能够对内存映射的文件区域进行读写操作，并在进程退出或调用`munmap`时正确地处理这些映射的区域。

### 实验步骤

#### 1. 初始化`mmap`和`munmap`系统调用

首先，我们在xv6中定义`mmap`和`munmap`系统调用的框架。

```c
[user.h]
char *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);

[usys.pl]
entry("mmap");
entry("munmap");

[syscall.h]
#define SYS_mmap 22
#define SYS_munmap 23

[syscall.c]
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
...
[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
};
```

#### 2. VMA数据结构的设计

在每个进程的结构体中添加一个VMA（Virtual Memory Area）数组，用于记录内存映射区域的信息。该结构体包括映射的起始地址、长度、权限、文件描述符等。

```C
// Virtual memory mapping
struct vma
{
  int valid;      // 有效位，当值为 0 时表示无效，即为 empty element
  uint64 addr;    // 记录起始地址
  int len;        // 长度
  int prot;       // 权限（read/write）
  int flags;      // 区域类型（shared/private）
  int off;        // 偏移量
  struct file *f; // 映射的文件
  uint64 refcnt;  // （延迟申请）已经映射的页数量
};
#define VMA_MAX 16

struct proc {
  ...
  struct vma vma_array[VMA_MAX];
  uint64 vma_top_addr;
};    
```

在allocproc中，初始化进程的VMA结构体。

```C
static struct proc*
allocproc(void)
{
	...
	// Initialize the vma array and the currently available VMA top address. 
    for(int i = 0;i<VMA_MAX;i++)
    {
       p->vma_array[i].valid = 0;
       p->vma_array[i].refcnt = 0;
    }
    p->vma_top_addr = MAXVA-2*PGSIZE;
```



#### 3. 实现`mmap`

`mmap`的实现主要分为以下步骤：

- 寻找进程地址空间中未使用的区域来进行内存映射。
- 创建对应的VMA条目，增加文件的引用计数，以防止文件被提前关闭。

```C
uint64 sys_mmap(void)
{
  uint64 addr;
  int len, prot, flags, fd, off;
  argaddr(0, &addr);
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &off);

  struct proc* p = myproc();
  struct file* f = p->ofile[fd];
  
  // Check whether this operation is legal
  if((flags==MAP_SHARED && f->writable==0 && (prot&PROT_WRITE))) return -1;

  // Find an empty VMA struct. 
  int idx = 0;
  for(;idx<VMA_MAX;idx++)
    if(p->vma_array[idx].valid==0)
      break;
  if(idx==VMA_MAX)
    panic("All VMA struct is full!");
  // printf("sys_mmap: Find available VMA struct, idx = %d\n", idx);
  
  // Fill this VMA struct.
  struct vma* vp = &p->vma_array[idx];
  vp->valid = 1;
  vp->len = len;
  vp->flags = flags;
  vp->off = off;
  vp->prot = prot;
  vp->f = f;
  filedup(f); // This file's refcnt += 1. 
  p->vma_top_addr-=len;
  vp->addr = p->vma_top_addr; // The usable user virtual address. 
  // printf("sys_mmap: Successfully mapped a file, with addr=%p, len=%x\n", vp->addr, vp->len);
  return vp->addr;
}
```



#### 4. 实现页面错误处理

当访问映射区域时，如果相应的物理页面尚未分配，会触发页面错误。我们需要在页面错误处理程序中：

- 分配物理页面并将文件内容读入其中。
- 将页面映射到进程的地址空间中，并设置正确的权限。

```C
else if(r_scause()==13){ //usertrap, page fault
    // Handle load page fault (when a file is mapped in the vitrual address space, but the physical page is not loaded). 
    uint64 target_va = r_stval();
    struct vma* vp = 0;
    // printf("usertrap: Trying to visit va %p\n", target_va);

    // Find the VMA struct that this file belongs to. 
    for(struct vma *now = p->vma_array;now<p->vma_array+VMA_MAX;now++)
    {
      // printf("usertrap: VMA, addr=%p, len=%x, valid=%d\n", now->addr, now->len, now->valid);
      if(now->addr<=target_va && target_va<now->addr+now->len 
         && now->valid)
      {
        vp = now;
        break;
      }
    }

    if(vp)
    {
      // Allocate a page into physical memory, and map it to the virtual memory. 
      uint64 mem = (uint64)kalloc();
      memset((void *)mem, 0, PGSIZE); // Set the page to all zero. 
      if(mappages(p->pagetable, target_va, PGSIZE, mem, PTE_U|PTE_V|(vp->prot<<1))<0)
        panic("Cannot map a virtual page for the file!");
      
      // Load the content of the page. 
      vp->refcnt += 1;
      ilock(vp->f->ip);
      readi(vp->f->ip, 0, mem, target_va-vp->addr, PGSIZE); // Load a file page from the disk
      iunlock(vp->f->ip);
    }
    else
    {
      printf("Unable to find the VMA struct that the file belongs to!\n");
      goto unexpected_scause;
    }
  }
    else if((which_dev = devintr()) != 0){
```



#### 5. 实现`munmap`

`munmap`的实现包括以下步骤：

- 在VMA数组中找到对应的映射区域。

- 解除页面的映射，并在必要时将数据写回文件。

- 更新文件引用计数。

  ```C
  uint64 sys_munmap(void)
  {
    uint64 addr;
    int len;
    argaddr(0, &addr);
    argint(1, &len);
    struct proc* p = myproc();
  
    struct vma* vp = 0;
    // Find the VMA struct that this file belongs to. 
    for(struct vma *now = p->vma_array;now<p->vma_array+VMA_MAX;now++)
    {
      // printf("usertrap: VMA, addr=%p, len=%x, valid=%d\n", now->addr, now->len, now->valid);
      if(now->addr<=addr && addr<now->addr+now->len 
          && now->valid)
      {
        vp = now;
        break;
      }
    }
  
    if(vp)
    {
      if( walkaddr( p->pagetable , addr ) != 0)
      {
        // Write back and unmap. 
        if(vp->flags==MAP_SHARED) filewrite(vp->f, addr, len);
        uvmunmap(p->pagetable, addr, len/PGSIZE, 1);
        return 0;
      }
      // Update the file's refcnt. 
      vp->refcnt -= 1;
      if(vp->refcnt) // set the vma struct to invalid. 
      {
        fileclose(vp->f);
        vp->valid = 0;
      }
      return 0;
    }
    else
    {
      panic("Cannot find a vma struct representing this file!");
    }
  }
  ```

  

#### 6. 扩展`fork`和`exit`系统调用

- 在`fork`中，子进程需要继承父进程的映射区域，并增加文件的引用计数。

  ```C
    // Copy the struct vma array. 
    np->vma_top_addr = p->vma_top_addr;
    for(int i = 0;i<VMA_MAX;i++)
    {
      if(p->vma_array[i].valid)
      {
        filedup(p->vma_array[i].f);
        memmove(&np->vma_array[i], &p->vma_array[i], sizeof(struct vma));
      }
    }
  ```

- 在`exit`中，进程退出时需自动解除所有映射区域，并根据情况更新文件状态。

  ```C
   // Release the mapped files in the virtual memory. 
    for(int i = 0;i<VMA_MAX;i++)
    {
      if(p->vma_array[i].valid)
      {
        struct vma* vp = &p->vma_array[i];
        for(uint64 addr = vp->addr;addr<vp->addr+vp->len;addr+=PGSIZE)
        {
          if(walkaddr(p->pagetable, addr) != 0)
          {
            if(vp->flags==MAP_SHARED) filewrite(vp->f, addr, PGSIZE);
            uvmunmap(p->pagetable, addr, 1, 1);
          }
        }
        fileclose(p->vma_array[i].f);
        p->vma_array[i].valid = 0;
      }
    }
  ```

  

### 实验中遇到的问题和解决方法

* **什么时候把文件加载到内存？**

  这个过程有点像Copy on write，当进程访问一个映射的虚拟地址时，如果对应的物理页面还没有加载，会触发一个页面错误（Page Fault），操作系统的页面错误处理程序将执行以下操作：
  
  - **定位对应的 VMA**：根据触发页面错误的虚拟地址，操作系统会查找进程的 `VMA` 列表，确定该地址是否属于某个映射区域。
  - **分配物理内存**：如果确实属于某个映射区域，操作系统将分配一个物理页面来存放文件内容。
  - **加载文件内容**：操作系统会从文件中读取相应的数据块（ 4KB 的页面大小）到刚刚分配的物理内存。
  - **更新页表**：将这个物理页面与虚拟地址绑定，并设置正确的访问权限（例如，只读或可写）

### 实验心得

`mmap`是一个综合性很强的实验。涉及了系统调用的编写，涉及了内存管理，也涉及了用户态陷入时的错误处理，有一些COW的思想体现在里面，当然也有文件系统的知识。通过本次实验，在xv6中实现了`mmap`和`munmap`系统调用，能够文件映射到进程的虚拟内存中，并通过内存操作来读取文件的内容。这让我对操作系统有了更深入的理解。



make grade：

![](img/10/grade.png)

