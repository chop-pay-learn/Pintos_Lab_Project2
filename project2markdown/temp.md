## 2. 核心数据结构设计 (Data Structures)

为了支持多进程环境下的同步与资源管理，必须对 `struct thread` 进行深度扩展。此外，为了解决父子进程生命周期不一致导致的“僵尸进程”或“悬空指针”问题，我们引入了辅助结构体。

### 2.1 扩展 `struct thread` (`threads/thread.h`)

```c
struct thread {
    /* ... 原有字段 (tid, status, stack, priority 等) ... */

    /* Project 2 新增字段 */

    /* 1. 进程谱系管理 */
    struct thread *parent;           // 指向父进程的指针
    struct list child_list;          // 存放子进程信息的列表 (存储 struct thread_shadow)

    /* 2. 自身状态与同步 */
    struct thread_shadow *to;        // 指向描述自身状态的影子结构体
    struct semaphore sema_create;    // 用于 exec：父进程在此等待子进程加载完毕
    struct semaphore sema_wait;      // 用于 wait：父进程在此等待子进程退出
    bool create_success;             // 记录子进程加载 ELF 是否成功
    int exit_code;                   // 进程退出码

    /* 3. 文件系统资源 */
    struct file *exec_file;          // 当前正在运行的可执行文件 (用于 deny_write)
    struct list file_list;           // 打开文件描述符表 (存储 struct file_shadow)
    int next_fd;                     // 下一个可用的文件描述符 ID (从 2 开始)
};
```

**设计原理分析：**
*   **`sema_create` vs `sema_wait`**：我们需要两个不同的信号量。`sema_create` 用于 `process_execute`，父进程必须阻塞直到子进程**加载完成**（即知道是成功还是失败）才能返回 tid；而 `sema_wait` 用于 `process_wait`，父进程阻塞直到子进程**彻底运行结束**。
*   **`file_list` 与 `next_fd`**：Pintos 不像 Linux 那样有系统级的打开文件表，我们简化设计，在每个线程内部维护一个私有的 FD 映射表。

### 2.2 影子结构体 `struct thread_shadow`

这是一个极其关键的设计。在 Pintos 中，当线程调用 `thread_exit` 时，其 `struct thread` 会被销毁并释放内存。如果父进程在子进程退出后才调用 `wait`，它将无法访问子进程的 `struct thread` 来获取退出码。

为此，我们设计了独立于 `struct thread` 存在的 `struct thread_shadow`：

```c
struct thread_shadow {
    tid_t tid;                       // 关联的线程 ID
    struct thread *from;             // 指向实际线程 (仅在 is_alive=true 时有效)
    int exit_code;                   // 退出码
    bool is_alive;                   // 线程是否存活
    bool is_being_waited;            // 防止对同一个进程重复 wait
    struct list_elem child_elem;     // 挂载在父进程 child_list 上的节点
};
```
*   **生命周期**：在 `thread_create` 时分配，挂载在父进程的 `child_list` 上。仅当父进程退出（且不再需要子进程信息）或父进程成功 `wait` 子进程后，该结构体才会被释放。

### 2.3 文件描述符结构 `struct file_shadow`

```c
struct file_shadow {
    int fd;                 // 用户可见的整数描述符
    struct file *f;         // 内核态的文件对象指针
    struct list_elem elem;  // 挂载在 thread->file_list 上
};
```

---

## 3. 详细实现过程 (Implementation Details)

### 3.1 参数传递 (Argument Passing)

**目标**：将类似 `args-none 1 2 "hello world"` 的命令行分解，并按 80x86 调用约定压入用户栈。

**实现逻辑 (`userprog/process.c` -> `push_argument`)**：

1.  **字符串切分**：
    使用 `strtok_r` 将命令行字符串切分为单词。例如命令 `grep foo bar` 切分为 `grep`, `foo`, `bar`。
2.  **物理入栈**：
    将字符串内容直接拷贝到栈顶（`PHYS_BASE` 下方）。注意栈是向下增长的。
3.  **对齐处理**：
    将 `esp` 向下移动，直到 `esp % 4 == 0`。这是为了提升内存访问性能。
4.  **构建指针数组 (argv)**：
    *   压入 `argv[argc]` (即 NULL)。
    *   从后往前压入 `argv[i]` 的地址（指向步骤2中压入的字符串首地址）。
5.  **构建元数据**：
    *   压入 `argv` 数组的首地址 (`char **`)。
    *   压入 `argc` (`int`)。
    *   压入伪造的返回地址 (`void *`)，通常为 0。

**堆栈布局图示：**

```text
地址        内容                 含义
0xC0000000  [ ... ]             (PHYS_BASE)
            [ "bar\0" ]         参数字符串
            [ "foo\0" ]
            [ "grep\0" ]
            [ padding ]         栈对齐 (0-3 字节)
            [ NULL ]            argv[3]
            [ ptr_to_bar ]      argv[2]
            [ ptr_to_foo ]      argv[1]
            [ ptr_to_grep ]     argv[0]
            [ ptr_to_argv_0 ]   argv (char **) 指向 argv[0] 的地址
            [ 3 ]               argc (int)
ESP ->      [ 0 ]               Return Address (伪造)
```

### 3.2 系统调用基础设施 (System Call Infrastructure)

**中断处理 (`userprog/syscall.c`)**：

当用户程序执行 `int 0x30` 时，CPU 捕获中断并调用 `syscall_handler`。此时，用户栈指针 `esp` 可以在中断帧 `struct intr_frame *f` 中找到。

**内存安全检查 (`check_ptr`)**：
这是内核稳定性的基石。用户程序可能会传入 NULL 指针、内核地址空间的指针（> `PHYS_BASE`）或未映射的虚拟地址。

```c
void *check_ptr(void *ptr, int byte) {
    // 1. 基本范围检查
    if (ptr == NULL || !is_user_vaddr(ptr))
        error_exit();

    // 2. 页表映射检查
    // 必须检查指针范围覆盖的所有页，防止跨页越界
    uint32_t *pd = thread_current()->pagedir;
    for (int i = 0; i < byte; i++) {
        if (pagedir_get_page(pd, (uint8_t *)ptr + i) == NULL)
            error_exit();
    }
    return ptr;
}
```
*   **原理**：利用 CPU 的 MMU 机制。如果 `pagedir_get_page` 返回 NULL，说明该虚拟地址没有对应的物理页，访问将导致 Page Fault。我们在系统调用层提前拦截，主动终止进程 (`exit(-1)`) 而不是让内核 Panic。

### 3.3 进程控制系统调用 (Process Control)

这是本次实验最复杂的部分，涉及三个核心函数：

#### A. Execute (创建与执行)
**函数**：`process_execute(char *file_name)` / `start_process`
1.  **解析**：主线程解析出文件名（第一个 token）。
2.  **创建**：调用 `thread_create` 创建子线程。
3.  **同步等待**：父进程立即调用 `sema_down(&cur->sema_create)` 进入阻塞状态。
4.  **子进程加载**：
    *   子线程在 `start_process` 中调用 `load()`。
    *   若加载成功：设置 `parent->create_success = true`。
    *   若加载失败：设置 `parent->create_success = false`，并退出。
    *   **唤醒**：无论成功失败，子线程最后调用 `sema_up(&parent->sema_create)`。
5.  **父进程返回**：父进程被唤醒，检查 `create_success`。成功返回 tid，失败返回 -1。

#### B. Wait (等待退出)
**函数**：`process_wait(tid_t child_tid)`
1.  **查找影子**：遍历 `child_list` 寻找对应 `tid` 的 `thread_shadow`。若找不到（非子进程）或 `is_being_waited` 为真（已等待过），直接返回 -1。
2.  **设置标志**：标记 `is_being_waited = true`。
3.  **阻塞等待**：如果 `shadow->is_alive` 为真，说明子进程还在运行。父进程调用 `sema_down(&shadow->from->sema_wait)` 阻塞自己。
4.  **获取结果**：当父进程被唤醒（由子进程退出时触发），或者如果子进程早已退出，直接从 `shadow->exit_code` 读取返回值。
5.  **资源清理**：将 `shadow` 从链表移除并 `free`。

#### C. Exit (退出)
**函数**：`process_exit`
1.  **资源释放**：关闭所有打开文件，释放页目录。
2.  **文件写保护释放**：`file_close(exec_file)` 会自动重新允许写入。
3.  **通知父进程**：
    *   设置自身的 `thread_shadow` 状态：`is_alive = false`，`exit_code = code`。
    *   如果父进程正在 `sema_wait` 上等待，调用 `sema_up` 唤醒它。
4.  **通知子进程**：遍历自己的 `child_list`，将所有子进程的 `parent` 指针置为 NULL（成为孤儿进程）。

### 3.4 文件系统调用 (File System Calls)

**并发控制**：
Pintos 的文件系统实现（`filesys/` 目录下）并非线程安全的。为了简化实现，我们在所有文件操作（`filesys_*`）外层包裹了一把全局大锁 `filesys_lock`。
*   `lock_acquire(&filesys_lock)`
*   `filesys_operation(...)`
*   `lock_release(&filesys_lock)`

**文件描述符 (FD) 实现**：
1.  **Open**：
    *   调用 `filesys_open` 得到 `struct file *f`。
    *   分配 `struct file_shadow`，设置 `fd = thread->next_fd++`。
    *   将 shadow 加入 `thread->file_list`。
    *   返回 fd。
2.  **Read/Write**：
    *   **FD 0/1**：特殊处理。Read fd=0 调用 `input_getc()`；Write fd=1 调用 `putbuf()`。
    *   **其他 FD**：遍历 `file_list` 找到对应的 `struct file *`，加锁调用 `file_read` / `file_write`。
3.  **Close**：
    *   查找并移除节点，关闭底层文件，释放内存。

**可执行文件保护 (Deny Write)**：
在 `load()` 成功后，立即调用 `file_deny_write(exec_file)`。这会在底层 inode 上设置标志，阻止任何写操作。该文件对象一直保留在 `thread` 结构中，直到 `process_exit` 时才关闭，从而释放写保护。

---

## 4. 实验总结 (Conclusion)

通过本次实验，我们深入理解了操作系统如何为用户程序提供服务。核心收获包括：
1.  **栈帧构建**：深刻理解了函数调用约定和栈内存布局。
2.  **保护模式**：学会了如何在特权级之间安全地传递数据，防止用户程序破坏内核。
3.  **并发同步**：掌握了使用信号量解决复杂的父子进程同步问题（特别是加载同步和退出同步的区别）。
4.  **资源管理**：通过“影子结构”优雅地解决了进程生命周期管理中的资源释放难题。

最终实现的系统能够正确运行参数传递测试、多进程递归执行测试以及各种文件操作测试，证明了设计的健壮性。
