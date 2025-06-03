#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

#include "lib/kernel/stdio.h"
#include "devices/input.h"

static void syscall_handler(struct intr_frame *);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static void check_valid_ptr(const void *ptr); 
static void check_valid_string(const char *str);
static void check_valid_buffer(const void *buffer, unsigned size);

static struct lock filesys_lock;

/* Reads a byte at user virtual address UADDR.
UADDR must be below PHYS_BASE.
Returns the byte value if successful, -1 if a segfault
occurred. */
static int get_user (const uint8_t *uaddr){
    int result;
    asm ("movl $1f, %0; movzbl %1, %0; 1:"
        : "=&a" (result) : "m" (*uaddr));
    return result;
}
/* Writes BYTE to user address UDST.
UDST must be below PHYS_BASE.
Returns true if successful, false if a segfault occurred. */
static bool put_user (uint8_t *udst, uint8_t byte){
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:" 
        : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}

static void check_valid_ptr(const void *ptr) {
    if (ptr == NULL || !is_user_vaddr(ptr) || ptr < (void *)0x08048000) {
        printf("%s: exit(-1)\n", thread_current()->name);
        thread_exit();
    }
}

static void check_valid_string(const char *str) {
    if (str == NULL) {
        printf("%s: exit(-1)\n", thread_current()->name);
        thread_exit();
    }
    while (true) {
        check_valid_ptr(str);
        if (*str == '\0')
            break;
        str++;
    }
}

static void check_valid_buffer(const void *buffer, unsigned size) {
    const uint8_t *buf = buffer;
    for (unsigned i = 0; i < size; i++) {
        check_valid_ptr(buf + i);
    }
}
void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
}

static void syscall_handler(struct intr_frame *f UNUSED) {
    uint32_t *args = ((uint32_t *) f->esp);
    check_valid_ptr(args);


    /*
     * The following print statement, if uncommented, will print out the syscall
     * number whenever a process enters a system call. You might find it useful
     * when debugging. It will cause tests to fail, however, so you should not
     * include it in your final submission.
     */

    /* printf("System call number: %d\n", args[0]); */

    switch (args[0]) {
        case SYS_EXIT:
            f->eax = args[1];
            printf("%s: exit(%d)\n", thread_current()->name, args[1]);
            thread_exit();
            break;

        case SYS_INCREMENT:
            f->eax = ++args[1];
            printf("%s: exit(%d)\n", thread_current()->name, args[1]);
            thread_exit();
            break;

        case SYS_WRITE:
            {
                // check_valid_ptr(args + 1);
                // check_valid_ptr(args + 2);
                // check_valid_ptr(args + 3);

                int fd = args[1];
                const void *buffer = (const void *)args[2];
                unsigned size = args[3];
                struct thread *cur = thread_current();

                check_valid_ptr(buffer);
                check_valid_buffer(buffer, size);
                
                if (fd == 1) { 
                    // Write to stdout
                    putbuf((const char *)buffer, size); 
                    f->eax = size; // return number of bytes written
                } else if (fd < 2 || fd >= MAX_FILES || cur->files[fd] == NULL) {
                    f->eax = -1; // Invalid file descriptor or file not open
                } else {
                    // Write to file
                    lock_acquire(&filesys_lock);
                    f->eax = file_write(cur->files[fd], buffer, size);
                    lock_release(&filesys_lock);
                }
            }
            break;

        case SYS_READ:
            {
                check_valid_ptr((void *)args[1]);
                check_valid_ptr((void *)args[2]);
                check_valid_ptr((void *)args[3]);

                int fd = args[1];
                void *buffer = (void *)args[2];
                unsigned size = args[3];
                struct thread *cur = thread_current();

                check_valid_ptr(buffer);
                check_valid_buffer(buffer, size);
                
                if (fd < 0 || fd >= MAX_FILES) {
                    f->eax = -1; // Invalid file descriptor
                } else if (fd == 0) {
                    // Reading from stdin
                    unsigned i;
                    uint8_t *buf = (uint8_t *)buffer;
                    for (i = 0; i < size; i++) {
                        char c = input_getc();
                        if (c == '\r') c = '\n';  // Convert carriage return to newline
                        buf[i] = c;
                        if (c == '\n') {
                            i++;
                            break;
                        }
                    }
                    f->eax = i;
                } else if (fd == 1) {
                    // Can't read from stdout
                    f->eax = -1;
                } else if (cur->files[fd] == NULL) {
                    f->eax = -1; // File not open
                } else {
                    lock_acquire(&filesys_lock);
                    f->eax = file_read(cur->files[fd], buffer, size);
                    lock_release(&filesys_lock);
                }
            }
            break;

        case SYS_CREATE:
            {
                // check_valid_ptr(args + 1);
                // check_valid_ptr(args + 2);

                const char *file = (const char *)args[1];
                off_t initial_size = (off_t)args[2];
                
                check_valid_string(file);

                lock_acquire(&filesys_lock);
                bool success = filesys_create(file, initial_size);
                lock_release(&filesys_lock);
                
                f->eax = success;
            } 
            break;

        case SYS_REMOVE:
            {
                // check_valid_ptr(args + 1);

                const char *file = (const char *)args[1];

                check_valid_string(file);

                lock_acquire(&filesys_lock);
                bool success = filesys_remove(file);
                lock_release(&filesys_lock);          
                f->eax = success;
            }
            break;
    

        case SYS_OPEN:
            {           
                // check_valid_ptr(args + 1);

                const char *file = (const char *)args[1];

                check_valid_string(file);

                lock_acquire(&filesys_lock);
                struct file *opened_file = filesys_open(file);
                lock_release(&filesys_lock);       
                
                if (opened_file == NULL) {
                    f->eax = -1;
                } else {
                    struct thread* current_thread = thread_current();
                    int fd = current_thread->next_fd++;
                    if (fd >= MAX_FILES) {
                        file_close(opened_file); // Close the file if fd limit is reached
                        f->eax = -1; // Return error
                    } else {
                        // Store ptr to the opened file in the thread's file descriptor table
                        current_thread->files[fd] = opened_file; 
                        f->eax = fd; // Return the file descriptor
                    }
                } 
            }
            break;

        case SYS_FILESIZE:
            {
                int fd = args[1];
                struct thread *cur = thread_current();
                
                if (fd < 2 || fd >= MAX_FILES || cur->files[fd] == NULL) {
                    f->eax = -1; // Invalid file descriptor
                } else {
                    lock_acquire(&filesys_lock);
                    f->eax = file_length(cur->files[fd]);
                    lock_release(&filesys_lock);
                }
            }
            break;

        case SYS_SEEK:
            lock_acquire(&filesys_lock);
            file_seek(thread_current()->files[args[1]], args[2]);
            lock_release(&filesys_lock);
            break;

        case SYS_TELL:
            lock_acquire(&filesys_lock);
            off_t pos = file_tell(thread_current()->files[args[1]]);
            lock_release(&filesys_lock);
            f->eax = pos;
            break;

        case SYS_CLOSE:
            {
                struct thread *cur = thread_current();
                lock_acquire(&filesys_lock);
                file_close(cur->files[args[1]]);
                cur->files[args[1]] = NULL;
                lock_release(&filesys_lock);
            }
            break;
            
        case SYS_HALT:
            shutdown_power_off();
            break;

        default:
            // Handle unknown system calls
            break;
    }

}


    // if (args[0] == SYS_EXIT) {
    //     f->eax = args[1];
    //     printf("%s: exit(%d)\n", thread_current()->name, args[1]);
    //     thread_exit();
    // // Part 3
    // } else if (args[0] == SYS_INCREMENT) {
    //     int i = args[1];
    //     f->eax = i + 1;
    // // Part 4
    // } else if (args[0] == SYS_WRITE) {
    //     int fd = args[1];
    //     const void *buf = (const void *) args[2];
    //     unsigned size = args[3];

    //     if (fd == 1) {    
    //         putbuf (buf, size);             
    //         f->eax = size;                  
    //     } else {
    //         f->eax = -1;                    
    //     }
    // }