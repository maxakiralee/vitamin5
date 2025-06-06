#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"

// static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);
static bool setup_arguments(const char *cmd_line, void **esp);

struct pargs {
    char *fn_copy;
    struct child_status *child;
    struct semaphore load_sema;
    bool load_success;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute(const char *file_name) {
    char *fn_copy;
    tid_t tid;

    // sema_init(&temporary, 0);
    /* Make a copy of FILE_NAME.
       Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);

    /* Extract program name for thread name */
    char *prog_name_copy = palloc_get_page(0);
    if (prog_name_copy == NULL) {
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }
    strlcpy(prog_name_copy, file_name, PGSIZE);
    
    char *save_ptr;
    char *prog_name = strtok_r(prog_name_copy, " ", &save_ptr);

    struct child_status *child = palloc_get_page(0);
    if (child == NULL) {
        palloc_free_page(fn_copy);
        palloc_free_page(prog_name_copy);
        return TID_ERROR;
    }

    struct pargs *args = palloc_get_page(0);
    if (args == NULL) {
        palloc_free_page(fn_copy);
        palloc_free_page(prog_name_copy);
        palloc_free_page(child);
        return TID_ERROR;
    }

    child->tid = TID_ERROR; // Will be updated if thread_create succeeds
    child->exit_code = -1;
    child->waited = false;
    sema_init(&child->exit_sema, 0);
    list_push_back(&thread_current()->children, &child->elem); // Add to parent's list

    args->fn_copy = fn_copy;
    args->child = child;
    sema_init(&args->load_sema, 0);
    args->load_success = false;

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(prog_name, PRI_DEFAULT, start_process, args);

    palloc_free_page(prog_name_copy); // prog_name_copy is no longer needed
    
    if (tid == TID_ERROR) {
        palloc_free_page(fn_copy); // Child won't free it
        list_remove(&child->elem); // Remove from parent's list
        palloc_free_page(child);   // Free child_status struct
        palloc_free_page(args);    // Free pargs struct
        return TID_ERROR;
    }

    // Thread creation was successful
    child->tid = tid; // Update TID in child_status

    sema_down(&args->load_sema); // Wait for child process to finish loading

    bool load_success = args->load_success;
    palloc_free_page(args); // Free pargs struct, it's no longer needed by parent

    if (!load_success) {
        // Child failed to load. process_wait will handle cleanup of child_status.
        return TID_ERROR;
    }

    return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void *args) {
    struct pargs *pargs = args;
    char *file_name = pargs->fn_copy;
    thread_current()->status_of_child = pargs->child;
    struct intr_frame if_;
    bool success;

    /* Initialize interrupt frame and load executable. */
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);
    pargs->load_success = success;
    sema_up(&pargs->load_sema); // signal completion of loading

    /* If load failed, quit. */
    palloc_free_page(file_name);
    if (!success)
        thread_exit();

    /* Start the user process by simulating a return from an
       interrupt, implemented by intr_exit (in
       threads/intr-stubs.S).  Because intr_exit takes all of its
       arguments on the stack in the form of a `struct intr_frame',
       we just point the stack pointer (%esp) to our stack frame
       and jump to it. */
    asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid) {
    struct thread *cur = thread_current();
    struct list_elem *e;
    struct child_status *child_to_wait_on = NULL;

    // Find the child in the current thread's children list
    for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e)) {
        struct child_status *cs = list_entry(e, struct child_status, elem);
        if (cs->tid == child_tid) {
            child_to_wait_on = cs;
            break;
        }
    }

    // If child not found, or already waited on (which implies it would have been removed), return -1.
    // The original check `child->waited` handles if wait is called multiple times on a found child before it's removed.
    if (child_to_wait_on == NULL || child_to_wait_on->waited) {
        return -1;
    }

    child_to_wait_on->waited = true; // Mark as being waited on (or that waiting has started)
    sema_down(&child_to_wait_on->exit_sema); // Wait for the child to exit

    int exit_code = child_to_wait_on->exit_code;

    // Child has exited, remove its status structure from parent's list and free it.
    list_remove(&child_to_wait_on->elem);
    palloc_free_page(child_to_wait_on);

    return exit_code;
}

/* Free the current process's resources. */
void process_exit(void) {
    struct thread *cur = thread_current();
    uint32_t *pd;

    // If this thread was created by a parent process (i.e., it's a child),
    // signal its parent that it's exiting.
    // cur->status_of_child is set in start_process.
    if (cur->status_of_child != NULL) {
        // The exit code should have been set by SYS_EXIT or remains -1 for abnormal termination.
        sema_up(&cur->status_of_child->exit_sema);
    }

    /* Close all open files */
    int i;
    for (i = 2; i < MAX_FILES; i++) {
        if (cur->files[i] != NULL) {
            file_close(cur->files[i]);
            cur->files[i] = NULL;
        }
    }

    /* Re-allow write access to the executable and close it */
    if (cur->executable != NULL) {
        file_allow_write(cur->executable);
        file_close(cur->executable);
        cur->executable = NULL;
    }

    /* Free child_status structures for children that were not waited for. */
    while (!list_empty(&cur->children)) {
        struct list_elem *e = list_pop_front(&cur->children);
        struct child_status *cs = list_entry(e, struct child_status, elem);
        palloc_free_page(cs);
    }

    /* Destroy the current process's page directory and switch back
       to the kernel-only page directory. */
    pd = cur->pagedir;
    if (pd != NULL) {
        /* Correct ordering here is crucial.  We must set
           cur->pagedir to NULL before switching page directories,
           so that a timer interrupt can't switch back to the
           process page directory.  We must activate the base page
           directory before destroying the process's page
           directory, or our active page directory will be one
           that's been freed (and cleared). */
        cur->pagedir = NULL;
        pagedir_activate(NULL);
        pagedir_destroy(pd);
    }
    // sema_up(&temporary);
    sema_up(&cur->status_of_child->exit_sema); // signal child has exited
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate(void) {
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
struct Elf32_Ehdr {
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
struct Elf32_Phdr {
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
#define PT_NULL 0 /* Ignore. */
#define PT_LOAD 1 /* Loadable segment. */
#define PT_DYNAMIC 2 /* Dynamic linking info. */
#define PT_INTERP 3 /* Name of dynamic loader. */
#define PT_NOTE 4 /* Auxiliary info. */
#define PT_SHLIB 5 /* Reserved. */
#define PT_PHDR 6 /* Program header table. */
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
bool load(const char *file_name, void (**eip)(void), void **esp) {
    struct thread *t = thread_current();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Make a copy of file_name to parse the program name.
       file_name (the original) contains the full command line. */
    char *file_name_for_parsing = palloc_get_page(0);
    if (file_name_for_parsing == NULL) {
        printf("load: palloc_get_page failed for file_name_for_parsing\n");
        return false; 
    }
    strlcpy(file_name_for_parsing, file_name, PGSIZE);
    
    char *save_ptr_load; /* Renamed to avoid conflict if this is pasted inside another function scope */
    char *actual_program_name = strtok_r(file_name_for_parsing, " ", &save_ptr_load);

    /* Allocate and activate page directory. */
    t->pagedir = pagedir_create();
    if (t->pagedir == NULL) {
        palloc_free_page(file_name_for_parsing);
        goto done;
    }
    process_activate();

    /* Open executable file. */
    file = filesys_open(actual_program_name);
    if (file == NULL) {
        printf("load: %s: open failed\n", actual_program_name);
        palloc_free_page(file_name_for_parsing);
        goto done;
    }

    /* Deny write access to the executable file */
    file_deny_write(file);
    
    /* Store the executable file in the thread structure */
    t->executable = file;
    
    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 3 || ehdr.e_version != 1 ||
        ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        palloc_free_page(file_name_for_parsing);
        goto done;
    }

    palloc_free_page(file_name_for_parsing); /* Successfully parsed, opened. Free the copy. */
    file_name_for_parsing = NULL; /* Avoid potential double free in later error paths via done label */

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
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
            if (validate_segment(&phdr, file)) {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0) {
                    /* Normal segment.
                       Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) -
                                  read_bytes);
                } else {
                    /* Entirely zero.
                       Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                }
                if (!load_segment(file, file_page, (void *) mem_page,
                                  read_bytes, zero_bytes, writable))
                    goto done;
            } else
                goto done;
            break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(esp))
        goto done;

    /* Set up arguments on the stack */
    if (!setup_arguments(file_name, esp))
        goto done;

    /* Start address. */
    *eip = (void (*)(void)) ehdr.e_entry;

    success = true;

done:
    /* We arrive here whether the load is successful or not. */
    /* Don't close the file here anymore - we need to keep it open
       to maintain write protection */
    if (!success && file != NULL) {
        file_close(file);
        t->executable = NULL;
    }
    return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr *phdr, struct file *file) {
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (Elf32_Off) file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *) phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *) (phdr->p_vaddr + phdr->p_memsz)))
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
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
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
        if (file_read(file, kpage, page_read_bytes) != (int) page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
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
static bool setup_stack(void **esp) {
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
        if (success)
            *esp = PHYS_BASE;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Set up the stack with command line arguments according to x86 calling convention */
static bool setup_arguments(const char *cmd_line, void **esp) {
    /* Make a copy of the command line to avoid modifying the original */
    char *cmd_copy = palloc_get_page(0);
    if (cmd_copy == NULL)
        return false;
    
    strlcpy(cmd_copy, cmd_line, PGSIZE);
    
    /* Parse the command line and count arguments */
    int argc = 0;
    char *token, *save_ptr;
    char *argv[128]; /* Limit number of arguments */
    
    /* Parse first token (program name) */
    token = strtok_r(cmd_copy, " ", &save_ptr);
    if (token == NULL) {
        palloc_free_page(cmd_copy);
        return false;
    }
    
    /* Store all arguments */
    while (token != NULL) {
        argv[argc++] = token;
        token = strtok_r(NULL, " ", &save_ptr);
    }
    
    /* Calculate total size needed and ensure it fits */
    size_t total_size = 0;
    for (int i = 0; i < argc; i++) {
        total_size += strlen(argv[i]) + 1; /* string + null terminator */
    }
    total_size += sizeof(char *) * (argc + 1); /* pointers + NULL sentinel */
    total_size += sizeof(char **); /* argv */
    total_size += sizeof(int);    /* argc */
    total_size += sizeof(void *); /* return address */
    
    if (total_size > PGSIZE) {
        palloc_free_page(cmd_copy);
        return false;
    }
    
    /* Set the initial stack pointer to the top of user virtual memory */
    /* Note: setup_stack already initializes *esp to PHYS_BASE.
       This line is kept for consistency with the provided "working" example. */
    *esp = PHYS_BASE; 
    
    /* Store addresses of strings on the stack */
    char *arg_addresses[argc];
    
    /* 1. Push strings onto the stack in reverse order */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1; /* Include null terminator */
        *esp -= len;
        memcpy(*esp, argv[i], len);
        arg_addresses[i] = *esp;
    }
    
    /* 2. Word-align the stack pointer to a multiple of 4 */
    *esp = (void *)((uintptr_t)(*esp) & ~3u);
    
    /* 3. Insert 16-byte alignment padding.
       The goal is that after argc_int is pushed, its address is 16-byte aligned. */
    uintptr_t esp_after_word_align = (uintptr_t)*esp;
    /* Size of the block: NULL, argv_ptr[0]..[argc-1], argv_char**, argc_int */
    int size_of_main_arg_block = (1 + argc + 1 + 1) * 4; 
    
    uintptr_t esp_at_argc_if_no_16b_padding = esp_after_word_align - size_of_main_arg_block;
    int padding_needed = esp_at_argc_if_no_16b_padding & 0xF; /* Bytes to subtract to align down */
                                                             /* (esp_at_argc_if_no_16b_padding % 16) */
    
    *esp -= padding_needed; // Decrement stack pointer for padding
    memset(*esp, 0, padding_needed); // Zero out the padding area
    
    /* 4. Push argv[argc] = NULL sentinel */
    *esp -= 4;
    *(uint32_t *)*esp = 0;
    
    /* 5. Push pointers to each string (argv[argc-1] to argv[0]) */
    for (int i = argc - 1; i >= 0; i--) {
        *esp -= 4;
        *(uint32_t *)*esp = (uint32_t)arg_addresses[i];
    }
    
    /* 6. Push pointer to argv (address of argv[0]'s slot on stack) */
    uint32_t argv_on_stack_ptr_val = (uint32_t)*esp; 
    *esp -= 4;
    *(uint32_t *)*esp = argv_on_stack_ptr_val;
    
    /* 7. Push argc */
    *esp -= 4;
    *(int *)*esp = argc;
    
    /* 8. Push fake return address */
    *esp -= 4;
    *(uint32_t *)*esp = 0;
    
    palloc_free_page(cmd_copy);
    return true;
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
static bool install_page(void *upage, void *kpage, bool writable) {
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
       address, then map our page there. */
    return (pagedir_get_page(t->pagedir, upage) == NULL &&
            pagedir_set_page(t->pagedir, upage, kpage, writable));
}
