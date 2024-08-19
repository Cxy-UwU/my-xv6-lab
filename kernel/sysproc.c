#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  uint64 base;            // 要检查的起始地址
  int len;                // 要检查的长度
  uint64 mask;            // 结果地址
  argaddr(0, &base);
  argint(1, &len);
  argaddr(2, &mask);

  uint64 result = 0;
  struct proc *proc = myproc(); // 获取当前进程
  // 遍历指定的长度
  for (int i = 0; i < len; i++)
  {
    pte_t *pte = walk(proc->pagetable, base + i * PGSIZE, 0);
    if (*pte & PTE_A) // 如果PTE_A位为1，则将其置为0，并在result中记录
    {
      *pte -= PTE_A;
      result |= (1L << i);
    }
  }
  // 将结果复制到用户空间
  if (copyout(proc->pagetable, mask, (char *)&result, sizeof(result)) < 0)
    panic("sys_pgacess copyout error");

  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
