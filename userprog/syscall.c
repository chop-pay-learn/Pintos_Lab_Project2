/* 用户态系统调用处理（userprog/syscall.c）
 *
 * 该文件负责：
 * - 在 0x30 号中断上接收用户进程发起的系统调用请求（参见 intr_register_int）。
 * - 从用户栈（intr_frame->esp）解析系统调用号与参数，并进行严格的指针/缓冲区校验。
 * - 调用具体的系统调用实现（文件操作、进程控制、标准 IO），并将结果写回 eax。
 *
 * 设计要点：
 * - 所有从用户态传入的指针必须检查：落在用户地址空间且有有效页映射；缓冲区按“长度”而非“以 NUL 结尾”校验。
 * - 对标准输入/输出（fd==0/1）做特殊处理；文件系统操作统一使用 filesys_lock 防止并发冲突。
 * - 出错时统一通过 error_exit() 结束进程并返回退出码 -1，保证内核健壮性。
 */
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "devices/input.h"
#include "devices/timer.h"

static void syscall_handler(struct intr_frame *);

/* 统一失败退出
 * 功能：当检测到非法指针、越界参数或不允许的操作时，以退出码 -1 终止当前线程。
 * 并发：仅写当前线程状态，无需加锁。
 * 副作用：设置 exit_code 后调用 thread_exit()，不返回。
 */
void error_exit()
{
  thread_current()->exit_code = -1;
  thread_exit();
}

/* 用户指针校验
 * 功能：校验 [ptr, ptr+byte) 范围内的每个字节是否映射到用户地址空间的有效页。
 * 参数：
 * - ptr：用户传入的起始指针，允许为任意对齐
 * - byte：需要检查的长度（字节数），可跨页
 * 返回：若合法，返回原始 ptr；若非法，直接 error_exit()。
 * 并发：只读页表，不加锁。
 * 失败处理：任一字节未映射或落入内核地址空间则终止进程。
 */
void *check_ptr(void *ptr, int byte)
{
  if (ptr == NULL || !is_user_vaddr(ptr))
    error_exit();

  uint32_t *pd = thread_current()->pagedir;
  const uint8_t *p = (const uint8_t *)ptr;
  for (int i = 0; i < byte; i++)
    if (pagedir_get_page(pd, p + i) == NULL)
      error_exit();
  return ptr;
}
/* C 字符串校验
 * 功能：从起始地址开始逐字节检查，直到遇到 '\\0'；确保整条字符串都落在用户空间且已映射。
 * 适用：仅用于以 NUL 结尾的参数（如文件名、命令行字符串）；不适用于原始缓冲区。
 */
void check_str(char *ptr)
{
  // 不能直接使用 strlen
  char *tmp = ptr;
  while (true)
  {
    tmp = check_ptr(tmp, 1);
    if ((*tmp) == '\0')
      break;
    tmp++;
  }
}

/* 在线程的打开文件列表中查找 FD
 * 功能：返回与 fd 对应的 file_shadow，便于获取底层 struct file*。
 * 返回：存在返回指针，不存在返回 NULL。
 * 并发：遍历当前线程的私有列表，无需加锁。
 */
struct file_shadow *foreach_file(int fd)
{
  struct thread *cur = thread_current();
  struct list *l = &cur->file_list;
  struct list_elem *e;
  for (e = list_begin(l); e != list_end(l); e = list_next(e))
  {
    struct file_shadow *tmp = list_entry(e, struct file_shadow, elem);
    if (tmp->fd == fd)
      return tmp;
  }
  return NULL;
}

/* 关机（测试用） */
void syscall_halt(struct intr_frame *f)
{
  shutdown_power_off();
}
/* 退出当前进程
 * 参数：用户栈上的第一个参数为退出码（int）
 * 语义：记录退出码并终止线程
 */
void syscall_exit(struct intr_frame *f)
{
  int exit_code = *(int *)check_ptr(f->esp + 4, 4);
  thread_current()->exit_code = exit_code;
  thread_exit();
}
/* 创建并执行新进程
 * 参数：命令行字符串（C 字符串）
 * 语义：校验字符串后调用 process_execute，返回新进程 tid 或 -1
 */
void syscall_exec(struct intr_frame *f)
{
  char *cmd = *(char **)check_ptr(f->esp + 4, 4);
  check_str(cmd);
  f->eax = process_execute(cmd);
}
/* 等待子进程退出
 * 参数：子进程 tid
 * 语义：一次性等待，返回子进程退出码；非法 tid/重复等待返回 -1
 */
void syscall_wait(struct intr_frame *f)
{
  int pid = *(int *)check_ptr(f->esp + 4, 4);
  f->eax = process_wait(pid);
}

// syscall.c
/* 创建文件
 * 参数：文件名（C 字符串）、初始大小（字节）
 * 并发：持 filesys_lock 保护底层文件系统
 */
void syscall_create(struct intr_frame *f)
{
  char *file_name = *(char **)check_ptr(f->esp + 4, 4);
  check_str(file_name);
  int file_size = *(int *)check_ptr(f->esp + 8, 4);

  lock_acquire(&filesys_lock);
  f->eax = filesys_create(file_name, file_size);
  lock_release(&filesys_lock);
}

/* 删除文件 */
void syscall_remove(struct intr_frame *f)
{
  char *file_name = *(char **)check_ptr(f->esp + 4, 4);
  check_str(file_name);

  lock_acquire(&filesys_lock);
  f->eax = filesys_remove(file_name);
  lock_release(&filesys_lock);
}

/* 打开文件
 * 返回：文件描述符（fd），失败返回 -1
 * 说明：为当前线程分配递增的 fd，并记录到 file_list
 */
void syscall_open(struct intr_frame *f)
{
  char *file_name = *(char **)check_ptr(f->esp + 4, 4);
  check_str(file_name);

  lock_acquire(&filesys_lock);
  struct file *open_file = filesys_open(file_name);
  lock_release(&filesys_lock);

  if (open_file == NULL)
  {
    f->eax = -1;
    return;
  }

  struct thread *cur = thread_current();
  struct file_shadow *tmp = malloc(sizeof(struct file_shadow));
  // 打开文件, 记录文件编号, 插入打开文件的队列
  tmp->f = open_file, tmp->fd = cur->next_fd++;
  list_push_back(&cur->file_list, &tmp->elem);
  f->eax = tmp->fd;
}

/* 获取文件大小（字节数） */
void syscall_filesize(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);

  struct file_shadow *tmp = foreach_file(fd);
  if (tmp == NULL || tmp->f == NULL)
    f->eax = -1;
  else
  {
    lock_acquire(&filesys_lock);
    f->eax = file_length(tmp->f);
    lock_release(&filesys_lock);
  }
}

/* 读取文件/标准输入
 * 参数：fd、缓冲区指针、大小
 * 语义：fd==0 从键盘读取；fd==1 非法；其他为文件读取
 * 校验：缓冲区按长度检查，避免对非 NUL 缓冲区用字符串校验
 */
void syscall_read(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);
  char *buf = *(char **)check_ptr(f->esp + 8, 4);
  int size = *(int *)check_ptr(f->esp + 12, 4);
  check_ptr(buf, size);

  if (fd == 1)
    error_exit();

  // 文档中有写 input_getc 处理标准输入
  if (fd == 0)
  {
    for (int i = 0; i < size; i++)
      *buf = input_getc(), buf++;
    f->eax = size;
  }
  else
  {
    struct file_shadow *tmp = foreach_file(fd);
    if (tmp == NULL)
      f->eax = -1;
    else
    {
      lock_acquire(&filesys_lock);
      f->eax = file_read(tmp->f, buf, size);
      lock_release(&filesys_lock);
    }
  }
}
/* 写入文件/标准输出
 * 参数：fd、缓冲区指针、大小
 * 语义：fd==1 使用 putbuf 输出到控制台；fd==0 非法；其他为文件写入
 * 校验：缓冲区按长度检查
 */
void syscall_write(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);
  char *buf = *(char **)check_ptr(f->esp + 8, 4);
  int size = *(int *)check_ptr(f->esp + 12, 4);
  check_ptr(buf, size);

  if (fd == 0)
    error_exit();
  // 文档中写 putbuf 处理输出
  if (fd == 1)
  {
    putbuf(buf, size);
    f->eax = size;
  }
  else
  {
    struct file_shadow *tmp = foreach_file(fd);
    if (tmp == NULL)
      f->eax = -1;
    else
    {
      lock_acquire(&filesys_lock);
      f->eax = file_write(tmp->f, buf, size);
      lock_release(&filesys_lock);
    }
  }
}

/* 设置文件读写位置 */
void syscall_seek(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);
  int pos = *(int *)check_ptr(f->esp + 8, 4);

  struct file_shadow *tmp = foreach_file(fd);
  if (tmp != NULL)
  {
    lock_acquire(&filesys_lock);
    file_seek(tmp->f, pos);
    lock_release(&filesys_lock);
  }
}

/* 获取文件当前位置 */
void syscall_tell(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);

  struct file_shadow *tmp = foreach_file(fd);
  if (tmp != NULL)
  {
    lock_acquire(&filesys_lock);
    f->eax = file_tell(tmp->f);
    lock_release(&filesys_lock);
  }
  else
    f->eax = -1;
}

/* 关闭文件并回收 FD */
void syscall_close(struct intr_frame *f)
{
  int fd = *(int *)check_ptr(f->esp + 4, 4);

  struct file_shadow *tmp = foreach_file(fd);
  if (tmp != NULL)
  {
    lock_acquire(&filesys_lock);
    file_close(tmp->f);
    // 从队列中移除, 释放 file_shadow
    list_remove(&tmp->elem);
    free(tmp);
    lock_release(&filesys_lock);
  }
}

int (*func[20])(struct intr_frame *);

/* 初始化系统调用分派表并注册中断入口 */
void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");

  func[SYS_HALT] = syscall_halt;
  func[SYS_EXIT] = syscall_exit;
  func[SYS_EXEC] = syscall_exec;
  func[SYS_WAIT] = syscall_wait;
  func[SYS_CREATE] = syscall_create;
  func[SYS_REMOVE] = syscall_remove;
  func[SYS_OPEN] = syscall_open;
  func[SYS_FILESIZE] = syscall_filesize;
  func[SYS_READ] = syscall_read;
  func[SYS_WRITE] = syscall_write;
  func[SYS_SEEK] = syscall_seek;
  func[SYS_TELL] = syscall_tell;
  func[SYS_CLOSE] = syscall_close;
}

static void
/* 系统调用总入口
 * 步骤：先校验用户栈指针，再读出系统调用号并检查范围，最后分派到具体实现。
 * 异常：非法指针/编号将导致进程以 -1 退出。
 */
syscall_handler(struct intr_frame *f UNUSED)
{
  check_ptr(f->esp, 4);
  int number = *(int *)(f->esp);
  if (number < 0 || number >= 20 || func[number] == NULL)
    error_exit();
  (func[number])(f);
}
