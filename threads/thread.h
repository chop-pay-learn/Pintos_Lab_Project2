#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>

/* 线程生命周期状态（调度用） */
enum thread_status {
  THREAD_RUNNING, /* Running thread. */
  THREAD_READY,   /* Not running but ready to run. */
  THREAD_BLOCKED, /* Waiting for an event to trigger. */
  THREAD_DYING    /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* 线程/用户进程描述结构（每个线程占一页，剩余作为内核栈）
 *
   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                 /* Thread identifier. */
  enum thread_status status; /* Thread state. */
  char name[16];             /* Name (for debugging purposes). */
  uint8_t *stack;            /* Saved stack pointer. */
  int priority;              /* Priority. */
  struct list_elem allelem;  /* List element for all threads list. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /* List element. */

#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir; /* Page directory. */
#endif
  /* Project 2 新增：父子关系与同步、文件/FD 管理 */
  struct thread *parent;  // 父进程指针：用于创建/等待阶段的同步
  struct list child_list; // 子进程的 shadow 列表，父进程用来跟踪子状态

  struct thread_shadow *to;     // 指向自己的 shadow，用于向父进程报告退出码等
  struct semaphore sema_create; // 创建阶段同步：子完成加载后唤醒父
  struct semaphore sema_wait;   // 等待阶段同步：父在此等待子退出
  bool create_success;          // 子进程是否创建/加载成功
  int exit_code;                // 子进程的退出码（由系统调用/异常设置）

  struct file *exec_file;       // 当前正在执行的文件（运行期间 deny_write）
  struct list file_list;        // 打开文件的 FD 映射列表（file_shadow）
  int next_fd;                  // 文件描述符分配起点（0/1 为 stdin/stdout，故从 2 开始）

  /* Owned by thread.c. */
  unsigned magic; /* Detects stack overflow. */
};

struct lock filesys_lock;
/* 子进程影子（父进程持有）
 * 记录 tid、退出码与存活状态，配合 child_list 管理；当父进程等待时由子进程唤醒。 */
struct thread_shadow {
  tid_t tid;
  struct thread *from;            // 指向子进程本体（存活时有效）
  int exit_code;                  // 子进程结束时的退出码
  bool is_alive, is_being_waited; // 是否存活；是否已被父进程等待（一次性）

  struct list_elem child_elem; // 配合 child_list
};

/* 文件影子（线程持有）
 * 将递增的 FD 映射到底层 struct file*，便于在系统调用中检索。 */
struct file_shadow {
  int fd;
  struct file *f;
  struct list_elem elem;
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;
extern bool thread_report_latency;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

#endif /* threads/thread.h */
