#include "userprog/syscall.h"

#include <stdio.h>
#include <syscall-nr.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

#include "lib/kernel/stdio.h"

static void syscall_handler(struct intr_frame *);
static struct lock filesys_lock;


void syscall_init(void) {
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
}

static void syscall_handler(struct intr_frame *f UNUSED) {
    uint32_t *args = ((uint32_t *) f->esp);

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
            if (args[1] == 1) { // fd = STDOUT
                putbuf((const char *) args[2], (size_t)args[3]); 
                f->eax = args[3]; // return number of bytes written
            } else { // fd != STDOUT
                f->eax = -1; // error: invalid file descriptor
            }
            break;

        case SYS_CREATE:
            {
                const char *file = (const char *)args[1];
                off_t initial_size = (off_t)args[2];
                
                lock_acquire(&filesys_lock);
                bool success = filesys_create(file, initial_size);
                lock_release(&filesys_lock);
                
                f->eax = success;
            } 
            break;

        case SYS_REMOVE:
            {
                lock_acquire(&filesys_lock);
                bool success = filesys_remove((const char *)args[1]);
                lock_release(&filesys_lock);          
                f->eax = success;
            }
            break;
    

        case SYS_OPEN:
            {           
                lock_acquire(&filesys_lock);
                struct file *opened_file = filesys_open((const char *)args[1]);
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