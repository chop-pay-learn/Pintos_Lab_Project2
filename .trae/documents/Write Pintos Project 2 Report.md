I have analyzed your source code (specifically `threads/thread.h`, `userprog/process.c`, and `userprog/syscall.c`) and understand your implementation logic for Argument Passing, System Calls, and Process Synchronization.

I will generate a detailed Markdown report in `Project2_report.md` with the following structure:

1.  **实验目的 (Experimental Purpose)**
    *   Understanding user program execution in Pintos.
    *   Implementing argument passing to the stack.
    *   Implementing system calls for process and file management.
2.  **实验内容 (Experimental Content)**
    *   Argument Passing (参数传递)
    *   System Calls (系统调用)
    *   Process Termination Messages (进程终止信息)
    *   Deny Writes to Executables (禁止写入正在运行的可执行文件)
3.  **具体实现过程 (Specific Implementation Details)**
    *   **数据结构修改 (Data Structure Changes)**: Analysis of changes in `struct thread` (parent/child pointers, semaphores, file descriptors).
    *   **参数传递 (Argument Passing)**: How `push_argument` in `process.c` parses the command line and sets up the stack.
    *   **系统调用处理 (System Call Handling)**:
        *   The `syscall_handler` and memory safety checks (`check_ptr`).
        *   Implementation of File System calls (using `filesys_lock` and `file_shadow`).
    *   **进程控制 (Process Control)**:
        *   `process_execute` and `sema_create` for load synchronization.
        *   `process_wait` and `sema_wait` for exit synchronization.
        *   Handling zombie processes using `thread_shadow`.
4.  **实验总结 (Summary)**

I will create this file without modifying any of your source code.