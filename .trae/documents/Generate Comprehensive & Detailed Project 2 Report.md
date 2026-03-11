I will completely rewrite `Project2_report.md` to be extremely detailed, focusing on implementation principles, deep code analysis, and architectural decisions.

The new report will include:

1.  **Architecture & Principles**:
    *   **User/Kernel Mode Switching**: Detailed explanation of `int 0x30`, interrupt frames, and how `esp` transitions between rings.
    *   **Virtual Memory Safety**: In-depth explanation of why and how we validate user pointers (`check_ptr`, `pagedir_get_page`), ensuring kernel stability.
    *   **Process Lifecycle Management**: The theoretical basis for "Shadow Threads" (handling zombie processes) and Semaphore synchronization.

2.  **Detailed Implementation Walkthrough (with Code Analysis)**:
    *   **Data Structures (`thread.h`)**: Line-by-line justification for every new field (`parent`, `child_list`, `sema_create`, `sema_wait`, `file_list`).
    *   **Argument Passing**:
        *   **Stack Diagram**: A visual representation of the user stack after pushing arguments.
        *   **Code Logic**: Detailed analysis of `push_argument` and `strtok_r` usage.
    *   **System Call Infrastructure**:
        *   Analysis of `syscall_handler` and the dispatch table.
        *   The logic behind `check_ptr` and `check_str`.
    *   **Process Synchronization (Exec/Wait/Exit)**:
        *   **Exec**: How `sema_create` ensures the parent waits for the child's *load* (not exit).
        *   **Wait**: How `thread_shadow` allows the parent to retrieve the exit code even after the child's `struct thread` is destroyed.
        *   **Exit**: The cleanup sequence and resource deallocation.
    *   **File System**: The File Descriptor (FD) table implementation and Global Lock (`filesys_lock`) strategy.

3.  **Experimental Results**: Summary of outcomes.

I will generate this comprehensive report directly in `Project2_report.md`.