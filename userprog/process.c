/* 用户进程创建与加载（userprog/process.c）
 *
 * 职责：
 * - 解析命令行、创建子线程、加载 ELF 可执行文件至用户地址空间。
 * - 搭建用户栈（参数入栈），设置初始 eip/esp，并通过 intr_exit 切入用户态。
 * - 父子进程同步：创建阶段用 sema_create，等待阶段用 sema_wait；退出码通过 shadow 传递。
 * - 执行文件写保护：运行期间 deny write，退出时 allow write 并关闭。
 *
 * 设计约束：
 * - load() 仅调用一次，避免页表/文件状态被破坏。
 * - 参数入栈需 4 字节对齐，栈内容包括字符串、argv 数组、argv 指针与 argc、返回地址。
 * - 资源释放顺序：先通知父/子、再销毁页表、最后回收打开文件与 exec_file。
 */
#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);
void push_argument(void **esp, char *cmd);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute(const char *file_name)
{
  /* 复制命令串两份：一份交给加载线程使用，另一份用于解析进程名。
   * 这样可以避免原始指针越界与竞态读取。 */
  char *fn_copy, *get_name;
  tid_t tid;

  // 把文件拷贝两次, 这样就不会访问到 原来 file_name 的内容从而越界
  fn_copy = palloc_get_page(0);
  get_name = palloc_get_page(0);
  if (fn_copy == NULL || get_name == NULL)
    return TID_ERROR;

  strlcpy(fn_copy, file_name, PGSIZE);
  strlcpy(get_name, file_name, PGSIZE);

  // 获取进程名字
  char *save_ptr;
  get_name = strtok_r(get_name, " ", &save_ptr);

  /* Create a new thread to executeute FILE_NAME. */
  tid = thread_create(get_name, PRI_DEFAULT, start_process, fn_copy);
  // 释放没用的 get_name 内存
  palloc_free_page(get_name);

  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);

  struct thread *cur = thread_current();
  sema_down(&cur->sema_create);
  if (!cur->create_success)
    return -1;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process(void *file_name_)
{
  /* 子线程入口：
   * - 初始化中断帧，设置用户段寄存器与标志位
   * - 解析命令行参数，持 filesys_lock 加载 ELF
   * - 将参数压入用户栈，设置执行文件写保护
   * - 通过 intr_exit 进入用户态 */
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  struct thread *cur = thread_current(); // new

  // new
  // 创建备份页面：用于参数入栈的原始命令串
  char *cmd = palloc_get_page(0);
  strlcpy(cmd, file_name_, PGSIZE);
  // 提取文件名
  char *save_ptr, *token;
  file_name = strtok_r(file_name, " ", &save_ptr);

  lock_acquire(&filesys_lock);
  success = load(file_name, &if_.eip, &if_.esp);
  lock_release(&filesys_lock);

  if (!success)
  {
    palloc_free_page(cmd);
    /*new begin*/
    /* 加载失败：向父进程报告创建失败并退出 */
    cur->exit_code = -1;
    cur->parent->create_success = false;
    sema_up(&cur->parent->sema_create);
    thread_exit();
    /*new end*/
  }

  // 把参数信息放到栈中
  push_argument(&if_.esp, cmd);
  // hex_dump((uintptr_t)if_.esp, if_.esp, (PHYS_BASE) - if_.esp, true);

  lock_acquire(&filesys_lock);
  struct file *f = filesys_open(file_name);
  file_deny_write(f);
  lock_release(&filesys_lock);
  thread_current()->exec_file = f;

  // 此时 cmd 已经解析完了, 之后不会再用到, 所以释放
  palloc_free_page(file_name);
  palloc_free_page(cmd);

  cur->parent->create_success = true;
  sema_up(&cur->parent->sema_create);

  // end new

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

void push_argument(void **esp, char *cmd)
{
  /* 将命令行参数压入用户栈，布局如下（自高地址向低地址）：
   *   [字符串块 ... 按 token 逆序压入并包含结尾 NUL]
   *   [对齐到 4 字节]
   *   [argv[argc] = 0]
   *   [argv[argc-1] ... argv[0] 指针]
   *   [argv（指向 argv[0] 的指针）]
   *   [argc]
   *   [return address = 0]
   */
  // (*esp) 等价于 if_.esp
  // (*(int *)(*esp)) 表示栈中存的真实值

  // 参数数量和参数列表地址
  int argc = 0, argv[64];
  char *token, *save_ptr;

  (*esp) = PHYS_BASE;

  for (token = strtok_r(cmd, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr))
  {
    size_t len = strlen(token);
    (*esp) -= (len + 1);
    memcpy((*esp), token, len + 1);
    argv[argc++] = (*esp);
  }

  (*esp) = (int)(*esp) & 0xfffffffc; // word_align
  (*esp) -= 4, (*(int *)(*esp)) = 0; // argv[argc]

  for (int i = argc - 1; i >= 0; i--) // argv[i];
    (*esp) -= 4, (*(int *)(*esp)) = argv[i];

  (*esp) -= 4, (*(int *)(*esp)) = (*esp) + 4; // argv
  (*esp) -= 4, (*(int *)(*esp)) = argc;       // argc
  (*esp) -= 4, (*(int *)(*esp)) = 0;          // return address
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
  /* 等待指定子进程结束（一次性等待）：
   * - 在父进程的 child_list 中查找 tid 对应的 shadow
   * - 若不存在或已被等待，返回 -1
   * - 若子进程仍存活，则阻塞在其 sema_wait；随后取出退出码、移除并释放 shadow */
  struct thread *cur = thread_current();
  struct list *l = &cur->child_list;
  struct list_elem *e;
  struct thread_shadow *target = NULL;

  for (e = list_begin(l); e != list_end(l); e = list_next(e))
  {
    struct thread_shadow *tmp = list_entry(e, struct thread_shadow, child_elem);
    if (tmp->tid == child_tid)
    {
      target = tmp;
      break;
    }
  }

  if (target == NULL || target->is_being_waited)
    return -1;

  target->is_being_waited = true;
  if (target->is_alive && target->from != NULL)
    sema_down(&target->from->sema_wait);

  int code = target->exit_code;
  list_remove(&target->child_elem);
  free(target);
  return code;
}

/* Free the current process's resources. */
void process_exit(void)
{
  /* 回收当前进程资源：
   * - 更新子影子状态：退出码与 is_alive 标记；若父已等待则唤醒
   * - 销毁页表并取消激活
   * - 关闭 exec_file（允许写回）与所有普通打开文件
   * - 打印退出信息 */
  struct thread *cur = thread_current();
  uint32_t *pd;

  // 更新子节点信息
  struct list *l = &cur->child_list;
  struct list_elem *e;
  for (e = list_begin(l); e != list_end(l); e = list_next(e))
  {
    struct thread_shadow *tmp = list_entry(e, struct thread_shadow, child_elem);
    // 加此判断, tmp->from 不为空
    if (tmp->is_alive)
      tmp->from->parent = NULL;
  }

  if (cur->parent == NULL)
    free(cur->to);
  else
  {
    // 把信息下放到影子
    cur->to->exit_code = cur->exit_code;
    if (cur->to->is_being_waited)
      sema_up(&cur->sema_wait);

    cur->to->is_alive = false;
    cur->to->from = NULL;
  }

  pd = cur->pagedir;
  if (pd != NULL)
  {
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  if (cur->exec_file != NULL)
  {
    lock_acquire(&filesys_lock);
    file_allow_write(cur->exec_file);
    file_close(cur->exec_file);
    lock_release(&filesys_lock);
    cur->exec_file = NULL;
  }

  while (!list_empty(&cur->file_list))
  {
    e = list_pop_front(&cur->file_list);
    struct file_shadow *tmp = list_entry(e, struct file_shadow, elem);
    lock_acquire(&filesys_lock);
    file_close(tmp->f);
    lock_release(&filesys_lock);
    free(tmp);
  }
  // 记得在尾部加上 退出信息
  printf("%s: exit(%d)\n", cur->name, cur->to->exit_code);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate(void)
{
  struct thread *t = thread_current();

  /* Activate thread's page tables. */
  pagedir_activate(t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void **esp);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char *file_name, void (**eip)(void), void **esp)
{
  struct thread *t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL)
  {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024)
  {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
  {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type)
    {
    case PT_NULL:
    case PT_NOTE:
    case PT_PHDR:
    case PT_STACK:
    default:
      /* Ignore this segment. */
      break;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
      goto done;
    case PT_LOAD:
      if (validate_segment(&phdr, file))
      {
        bool writable = (phdr.p_flags & PF_W) != 0;
        uint32_t file_page = phdr.p_offset & ~PGMASK;
        uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
        uint32_t page_offset = phdr.p_vaddr & PGMASK;
        uint32_t read_bytes, zero_bytes;
        if (phdr.p_filesz > 0)
        {
          /* Normal segment.
             Read initial part from disk and zero the rest. */
          read_bytes = page_offset + phdr.p_filesz;
          zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
        }
        else
        {
          /* Entirely zero.
             Don't read anything from disk. */
          read_bytes = 0;
          zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
        }
        if (!load_segment(file, file_page, (void *)mem_page,
                          read_bytes, zero_bytes, writable))
          goto done;
      }
      else
        goto done;
      break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  file_close(file);
  return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
  /* 校验待加载段：
   * - 文件偏移与虚拟地址页内偏移一致
   * - 文件范围合法，memsz >= filesz，且非空
   * - 虚拟地址范围在用户空间且不环绕，且不映射到页 0
   */
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void *)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  /* 将文件段加载到用户页：
   * - 计算每页读入与清零字节数
   * - 从用户池分配页，读取文件内容并补零
   * - 调用 install_page 建立映射
   */
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
       We will read PAGE_READ_BYTES bytes from FILE
       and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
    {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable))
    {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp)
{
  /* 在用户虚拟地址空间的最高页映射一个零页作为初始栈：
   * - 可写
   * - 如果映射失败，返回 false */
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL)
  {
    success = install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
  /* 建立用户页到内核页的映射：
   * - 保证 upage 尚未映射
   * - 设置可写标志位
   * - 返回是否成功 */
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pagedir, upage) == NULL && pagedir_set_page(t->pagedir, upage, kpage, writable));
}
