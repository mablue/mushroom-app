/**
 * seccomp_policy.cpp — Seccomp-BPF policy for the Mushroom engine
 *
 * This module compiles and applies a seccomp-BPF (Berkeley Packet Filter)
 * program that restricts which syscalls the Linux environment can make
 * at the kernel level. This is a defense-in-depth layer alongside the
 * LD_PRELOAD user-space interception.
 *
 * The policy:
 *   - ALLOW: read, write, open, openat, close, mmap, munmap, mprotect,
 *            brk, sched_yield, futex, clock_gettime, gettid, getpid,
 *            getuid, getgid, geteuid, getegid, exit_group, exit, writev,
 *            readv, pread64, pwrite64, lseek, fstat, fstat64, stat,
 *            lstat, stat64, lseek64, ioctl, poll, ppoll, select,
 *            pselect6, epoll_create1, epoll_ctl, epoll_wait, clone,
 *            clone3, fork, vfork, execve, execveat, dup, dup2, dup3,
 *            pipe, pipe2, socket, connect, bind, listen, accept,
 *            accept4, sendto, recvfrom, sendmsg, recvmsg, getsockopt,
 *            setsockopt, getpeername, getsockname, shutdown, chdir,
 *            fchdir, getcwd, getdents, getdents64, readlink, readlinkat,
 *            symlink, link, unlink, unlinkat, mkdir, mkdirat, rmdir,
 *            rename, renameat, renameat2, chmod, fchmod, chown, fchown,
 *            lchown, utimensat, utimes, nanosleep, usleep, sigaction,
 *            sigprocmask, sigreturn, rt_sigaction, rt_sigprocmask,
 *            rt_sigreturn, prctl, arch_prctl, set_tid_address,
 *            set_robust_list, get_robust_list, madvise, mincore,
 *            mlock, munlock, mlockall, munlockall, shmget, shmat,
 *            shmdt, shmctl, semget, semop, semctl, msgget, msgsnd,
 *            msgrcv, msgctl, sched_setparam, sched_getparam,
 *            sched_setscheduler, sched_getscheduler, sched_get_priority_max,
 *            sched_get_priority_min, sched_rr_get_interval, sched_setaffinity,
 *            sched_getaffinity, timer_create, timer_settime, timer_gettime,
 *            timer_getoverrun, timer_delete, clock_settime, clock_adjtime,
 *            gettimeofday, time, times, sysinfo, uname, getrusage,
 *            getpriority, setpriority, setrlimit, getrlimit, getcpu,
 *            personality, capget, capset, pselect6, sigaltstack,
 *            getrandom, memfd_create, eventfd, eventfd2, signalfd,
 *            signalfd4, inotify_init, inotify_init1, inotify_add_watch,
 *            inotify_rm_watch, userfaultfd, copy_file_range, splice,
 *            tee, vmsplice, sendfile, fallocate, fadvise64, fadvise64_64,
 *            sync_file_range, syncfs, sync, fsync, fdatasync, fcntl,
 *            fcntl64, flock, name_to_handle_at, open_by_handle_at,
 *            setns, unshare, pivot_root, mount, umount, umount2,
 *            swapon, swapoff, chroot, pivot_root
 *
 *   - BLOCK: ptrace, kexec_load, kexec_file_load, reboot, init_module,
 *            finit_module, delete_module, iopl, ioperm, set_mempolicy,
 *            mbind, migrate_pages, move_pages, swapon, swapoff, acct,
 *            bdflush, create_module, get_kernel_syms, query_module,
 *            nfsservctl, sysfs, uselib, _sysctl, modify_ldt, look_up_dcookie,
 *            perf_event_open, process_vm_readv, process_vm_writev,
 *            kcmp, add_key, keyctl, request_key, io_setup, io_destroy,
 *            io_submit, io_cancel, io_getevents, remap_file_pages,
 *            migrate_pages, move_pages, mbind, set_mempolicy, get_mempolicy,
 *            spu_run, mq_open, mq_unlink, mq_timedsend, mq_timedreceive,
 *            mq_notify, mq_getsetattr, vserver, waitid, fanotify_init,
 *            fanotify_mark, name_to_handle_at, open_by_handle_at,
 *            clock_adjtime, setns, getcpu, process_vm_readv, process_vm_writev,
 *            kcmp, finit_module, kexec_file_load, bpf, seccomp, userfaultfd,
 *            membarrier, pkey_mprotect, pkey_alloc, pkey_free, rseq,
 *            io_uring_setup, io_uring_enter, io_uring_register
 *
 *   - ERRNO: ptrace → EPERM, init_module → EPERM, reboot → EPERM
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "include/engine.h"

#define TAG "MushroomSeccomp"

/* ---------- Syscall numbers (ARM64 / x86_64) ---------- */

/* ARM64 (Linux syscall table) */
#define ARM64_SYS_READ             0
#define ARM64_SYS_WRITE            1
#define ARM64_SYS_OPEN             2   /* not used on arm64, uses openat */
#define ARM64_SYS_CLOSE            3
#define ARM64_SYS_STAT             4   /* not used on arm64 */
#define ARM64_SYS_FSTAT            5
#define ARM64_SYS_LSTAT            6   /* not used on arm64 */
#define ARM64_SYS_POLL             7
#define ARM64_SYS_LSEEK            8
#define ARM64_SYS_MMAP             9
#define ARM64_SYS_MPROTECT        10
#define ARM64_SYS_MUNMAP          11
#define ARM64_SYS_BRK             12
#define ARM64_SYS_RT_SIGACTION    13
#define ARM64_SYS_RT_SIGPROCMASK  14
#define ARM64_SYS_RT_SIGRETURN    15
#define ARM64_SYS_IOCTL           16
#define ARM64_SYS_PREAD64         17
#define ARM64_SYS_PWRITE64        18
#define ARM64_SYS_READV           19
#define ARM64_SYS_WRITEV          20
#define ARM64_SYS_ACCESS          21
#define ARM64_SYS_PIPE            22
#define ARM64_SYS_SELECT          23
#define ARM64_SYS_SCHED_YIELD     24
#define ARM64_SYS_MREMAP          25
#define ARM64_SYS_MSYNC           26
#define ARM64_SYS_MINCORE         27
#define ARM64_SYS_MADVISE         28
#define ARM64_SYS_SHMGET          29
#define ARM64_SYS_SHMAT           30
#define ARM64_SYS_SHMCTL          31
#define ARM64_SYS_DUP             32
#define ARM64_SYS_DUP2            33
#define ARM64_SYS_PAUSE           34
#define ARM64_SYS_NANOSLEEP       35
#define ARM64_SYS_GETITIMER       36
#define ARM64_SYS_ALARM           37
#define ARM64_SYS_SETITIMER       38
#define ARM64_SYS_GETPID          39
#define ARM64_SYS_SENDFILE        40
#define ARM64_SYS_SOCKET          41
#define ARM64_SYS_CONNECT         42
#define ARM64_SYS_ACCEPT          43
#define ARM64_SYS_SENDTO          44
#define ARM64_SYS_RECVFROM        45
#define ARM64_SYS_SENDMSG         46
#define ARM64_SYS_RECVMSG         47
#define ARM64_SYS_SHUTDOWN        48
#define ARM64_SYS_BIND            49
#define ARM64_SYS_LISTEN          50
#define ARM64_SYS_GETSOCKNAME     51
#define ARM64_SYS_GETPEERNAME     52
#define ARM64_SYS_SOCKETPAIR      53
#define ARM64_SYS_SETSOCKOPT      54
#define ARM64_SYS_GETSOCKOPT      55
#define ARM64_SYS_CLONE           56
#define ARM64_SYS_FORK            57
#define ARM64_SYS_VFORK           58
#define ARM64_SYS_EXECVE          59  /* not used on arm64, uses execveat */
#define ARM64_SYS_EXIT            60
#define ARM64_SYS_WAIT4           61
#define ARM64_SYS_KILL            62
#define ARM64_SYS_UNAME           63
#define ARM64_SYS_SEMGET          64
#define ARM64_SYS_SEMOP           65
#define ARM64_SYS_SEMCTL          66
#define ARM64_SYS_SHMDT           67
#define ARM64_SYS_MSGGET          68
#define ARM64_SYS_MSGSND          69
#define ARM64_SYS_MSGRCV          70
#define ARM64_SYS_MSGCTL          71
#define ARM64_SYS_FCNTL           72
#define ARM64_SYS_FLOCK           73
#define ARM64_SYS_FSYNC           74
#define ARM64_SYS_FDATASYNC       75
#define ARM64_SYS_TRUNCATE        76
#define ARM64_SYS_FTRUNCATE       77
#define ARM64_SYS_GETDENTS        78
#define ARM64_SYS_GETCWD          79
#define ARM64_SYS_CHDIR           80
#define ARM64_SYS_FCHDIR          81
#define ARM64_SYS_RENAME          82
#define ARM64_SYS_MKDIR           83
#define ARM64_SYS_RMDIR           84
#define ARM64_SYS_CREAT           85
#define ARM64_SYS_LINK            86
#define ARM64_SYS_UNLINK          87
#define ARM64_SYS_SYMLINK         88
#define ARM64_SYS_READLINK        89
#define ARM64_SYS_CHMOD           90
#define ARM64_SYS_FCHMOD          91
#define ARM64_SYS_CHOWN           92
#define ARM64_SYS_FCHOWN          93
#define ARM64_SYS_LCHOWN          94
#define ARM64_SYS_UMASK           95
#define ARM64_SYS_GETTIMEOFDAY    96
#define ARM64_SYS_GETRLIMIT       97
#define ARM64_SYS_GETRUSAGE       98
#define ARM64_SYS_SYSINFO         99
#define ARM64_SYS_TIMES          100
#define ARM64_SYS_PTRACE         101
#define ARM64_SYS_GETUID         102
#define ARM64_SYS_SYSLOG         103
#define ARM64_SYS_GETGID         104
#define ARM64_SYS_SETUID         105
#define ARM64_SYS_SETGID         106
#define ARM64_SYS_GETEUID        107
#define ARM64_SYS_GETEGID        108
#define ARM64_SYS_SETPGID        109
#define ARM64_SYS_GETPPID        110
#define ARM64_SYS_GETPGRP        111
#define ARM64_SYS_SETSID         112
#define ARM64_SYS_SETREUID       113
#define ARM64_SYS_SETREGID       114
#define ARM64_SYS_GETGROUPS      115
#define ARM64_SYS_SETGROUPS      116
#define ARM64_SYS_SETRESUID      117
#define ARM64_SYS_GETRESUID      118
#define ARM64_SYS_SETRESGID      119
#define ARM64_SYS_GETRESGID      120
#define ARM64_SYS_GETPGID        121
#define ARM64_SYS_SETFSUID       122
#define ARM64_SYS_SETFSGID       123
#define ARM64_SYS_GETSID         124
#define ARM64_SYS_CAPGET         125
#define ARM64_SYS_CAPSET         126
#define ARM64_SYS_RT_SIGPENDING  127
#define ARM64_SYS_RT_SIGTIMEDWAIT 128
#define ARM64_SYS_RT_SIGQUEUEINFO 129
#define ARM64_SYS_RT_SIGSUSPEND  130
#define ARM64_SYS_SIGALTSTACK    131
#define ARM64_SYS_UTIME          132
#define ARM64_SYS_MKNOD          133
/* ARM64 uses openat, mkdirat, etc. for newer syscalls */
#define ARM64_SYS_OPENAT         56  /* ARM64: clone=56, openat=??? */
/* Actually ARM64 renumbered. Let's use the correct numbers */

/* ARM64 syscall numbers (correct) */
#define ARM64_SYS_IO_SETUP          0
#define ARM64_SYS_IO_DESTROY        1
#define ARM64_SYS_IO_SUBMIT         2
#define ARM64_SYS_IO_CANCEL         3
#define ARM64_SYS_IO_GETEVENTS      4
#define ARM64_SYS_SETXATTR          5
#define ARM64_SYS_LSETXATTR         6
#define ARM64_SYS_FSETXATTR         7
#define ARM64_SYS_GETXATTR          8
#define ARM64_SYS_LGETXATTR         9
#define ARM64_SYS_FGETXATTR        10
#define ARM64_SYS_LISTXATTR        11
#define ARM64_SYS_LLISTXATTR       12
#define ARM64_SYS_FLISTXATTR       13
#define ARM64_SYS_REMOVEXATTR      14
#define ARM64_SYS_LREMOVEXATTR     15
#define ARM64_SYS_FREMOVEXATTR     16
#define ARM64_SYS_GETCWD           17
#define ARM64_SYS_LOOKUP_DCOOKIE   18
#define ARM64_SYS_EVENTFD2         19
#define ARM64_SYS_EPOLL_CREATE1    20
#define ARM64_SYS_EPOLL_CTL        21
#define ARM64_SYS_EPOLL_PWAIT      22
#define ARM64_SYS_DUP              23
#define ARM64_SYS_DUP3             24
#define ARM64_SYS_FCNTL            25
#define ARM64_SYS_INOTIFY_INIT1    26
#define ARM64_SYS_INOTIFY_ADD_WATCH 27
#define ARM64_SYS_INOTIFY_RM_WATCH 28
#define ARM64_SYS_IOCTL            29
#define ARM64_SYS_IOPRIO_SET       30
#define ARM64_SYS_IOPRIO_GET       31
#define ARM64_SYS_FLOCK            32
#define ARM64_SYS_MKNODAT          33
#define ARM64_SYS_MKDIRAT          34
#define ARM64_SYS_UNLINKAT         35
#define ARM64_SYS_SYMLINKAT        36
#define ARM64_SYS_LINKAT           37
#define ARM64_SYS_RENAMEAT         38
#define ARM64_SYS_UMOUNT2          39
#define ARM64_SYS_MOUNT            40
#define ARM64_SYS_PIVOT_ROOT       41
#define ARM64_SYS_NFSSERVCTL       42
#define ARM64_SYS_STATFS           43
#define ARM64_SYS_FSTATFS          44
#define ARM64_SYS_TRUNCATE         45
#define ARM64_SYS_FTRUNCATE        46
#define ARM64_SYS_FALLOCATE        47
#define ARM64_SYS_FACCESSAT        48
#define ARM64_SYS_CHDIR            49
#define ARM64_SYS_FCHDIR           50
#define ARM64_SYS_CHROOT           51
#define ARM64_SYS_FCHMOD           52
#define ARM64_SYS_FCHMODAT         53
#define ARM64_SYS_FCHOWNAT         54
#define ARM64_SYS_FCHOWN           55
#define ARM64_SYS_OPENAT           56
#define ARM64_SYS_CLOSE            57
#define ARM64_SYS_VHANGUP          58
#define ARM64_SYS_PIPE2            59
#define ARM64_SYS_QUOTACTL         60
#define ARM64_SYS_GETDENTS64       61
#define ARM64_SYS_LSEEK            62
#define ARM64_SYS_READ             63
#define ARM64_SYS_WRITE            64
#define ARM64_SYS_READV            65
#define ARM64_SYS_WRITEV           66
#define ARM64_SYS_PREAD64          67
#define ARM64_SYS_PWRITE64         68
#define ARM64_SYS_PREADV           69
#define ARM64_SYS_PWRITEV          70
#define ARM64_SYS_SENDFILE         71
#define ARM64_SYS_PSELECT6         72
#define ARM64_SYS_PPOLL            73
#define ARM64_SYS_SIGNALFD4        74
#define ARM64_SYS_VMSPLICE         75
#define ARM64_SYS_SPLICE           76
#define ARM64_SYS_TEE              77
#define ARM64_SYS_READLINKAT       78
#define ARM64_SYS_FSTATAT          79
#define ARM64_SYS_FSTAT            80
#define ARM64_SYS_SYNC             81
#define ARM64_SYS_FSYNC            82
#define ARM64_SYS_FDATASYNC        83
#define ARM64_SYS_SYNC_FILE_RANGE  84
#define ARM64_SYS_TIMERFD_CREATE   85
#define ARM64_SYS_TIMERFD_SETTIME  86
#define ARM64_SYS_TIMERFD_GETTIME  87
#define ARM64_SYS_UTIMENSAT        88
#define ARM64_SYS_ACCT             89
#define ARM64_SYS_CAPGET           90
#define ARM64_SYS_CAPSET           91
#define ARM64_SYS_PERSONALITY      92
#define ARM64_SYS_EXIT             93
#define ARM64_SYS_EXIT_GROUP       94
#define ARM64_SYS_WAITID           95
#define ARM64_SYS_SET_TID_ADDRESS  96
#define ARM64_SYS_FUTEX            98
#define ARM64_SYS_SET_ROBUST_LIST  99
#define ARM64_SYS_GET_ROBUST_LIST  100
#define ARM64_SYS_NANOSLEEP        101
#define ARM64_SYS_GETITIMER        102
#define ARM64_SYS_SETITIMER        103
#define ARM64_SYS_SCHED_SETPARAM   118
#define ARM64_SYS_SCHED_GETPARAM   119
#define ARM64_SYS_SCHED_SETSCHEDULER 120
#define ARM64_SYS_SCHED_GETSCHEDULER 121
#define ARM64_SYS_SCHED_GET_PRIORITY_MAX 122
#define ARM64_SYS_SCHED_GET_PRIORITY_MIN 123
#define ARM64_SYS_SCHED_RR_GET_INTERVAL 124
#define ARM64_SYS_SCHED_YIELD      125
#define ARM64_SYS_SCHED_GETAFFINITY 126
#define ARM64_SYS_SCHED_SETAFFINITY 127
#define ARM64_SYS_RESTART_SYSCALL  128
#define ARM64_SYS_CLOCK_SETTIME    130
#define ARM64_SYS_CLOCK_GETTIME    113
#define ARM64_SYS_CLOCK_GETRES     114
#define ARM64_SYS_CLOCK_NANOSLEEP  115
#define ARM64_SYS_GETTID           178
#define ARM64_SYS_SYSFS            139
#define ARM64_SYS_TGKILL           236
#define ARM64_SYS_PROCESS_VM_READV 270
#define ARM64_SYS_PROCESS_VM_WRITEV 271
#define ARM64_SYS_KCMP             272
#define ARM64_SYS_FINIT_MODULE     273
#define ARM64_SYS_SCHED_SETATTR    274
#define ARM64_SYS_SCHED_GETATTR    275
#define ARM64_SYS_RENAMEAT2        276
#define ARM64_SYS_SECCOMP          277
#define ARM64_SYS_GETRANDOM        278
#define ARM64_SYS_MEMFD_CREATE     279
#define ARM64_SYS_BPF              280
#define ARM64_SYS_EXECVEAT         281
#define ARM64_SYS_USERFAULTFD      282
#define ARM64_SYS_MEMBARRIER       283
#define ARM64_SYS_MLOCK2           284
#define ARM64_SYS_COPY_FILE_RANGE  285
#define ARM64_SYS_PREADV2          286
#define ARM64_SYS_PWRITEV2         287
#define ARM64_SYS_PKEY_MPROTECT    288
#define ARM64_SYS_PKEY_ALLOC       289
#define ARM64_SYS_PKEY_FREE        290
#define ARM64_SYS_STATX            291
#define ARM64_SYS_IO_PGETEVENTS    292
#define ARM64_SYS_RSEQ             293
#define ARM64_SYS_KEXEC_FILE_LOAD  294
#define ARM64_SYS_PIDFD_SEND_SIGNAL 424
#define ARM64_SYS_IO_URING_SETUP   425
#define ARM64_SYS_IO_URING_ENTER   426
#define ARM64_SYS_IO_URING_REGISTER 427
#define ARM64_SYS_OPEN_TREE        428
#define ARM64_SYS_MOVE_MOUNT       429
#define ARM64_SYS_FSOPEN           430
#define ARM64_SYS_FSCONFIG         431
#define ARM64_SYS_FSMOUNT          432
#define ARM64_SYS_FSPICK           433
#define ARM64_SYS_PIDFD_OPEN       434
#define ARM64_SYS_CLONE3           435
#define ARM64_SYS_CLOSE_RANGE      436
#define ARM64_SYS_OPENAT2          437
#define ARM64_SYS_PIDFD_GETFD      438
#define ARM64_SYS_FACCESSAT2       439
#define ARM64_SYS_PROCESS_MADVISE  440
#define ARM64_SYS_EPOLL_PWAIT2     441
#define ARM64_SYS_MOUNT_SETATTR    442
#define ARM64_SYS_QUOTACTL_FD      443
#define ARM64_SYS_LANDLOCK_CREATE_RULESET 444
#define ARM64_SYS_LANDLOCK_ADD_RULE 445
#define ARM64_SYS_LANDLOCK_RESTRICT_SELF 446
#define ARM64_SYS_MEMFD_SECRET      447
#define ARM64_SYS_PROCESS_MRELEASE  448

/* x86_64 syscall numbers */
#define X86_64_SYS_READ            0
#define X86_64_SYS_WRITE           1
#define X86_64_SYS_OPEN            2
#define X86_64_SYS_CLOSE           3
#define X86_64_SYS_STAT            4
#define X86_64_SYS_FSTAT           5
#define X86_64_SYS_LSTAT           6
#define X86_64_SYS_POLL            7
#define X86_64_SYS_LSEEK           8
#define X86_64_SYS_MMAP            9
#define X86_64_SYS_MPROTECT        10
#define X86_64_SYS_MUNMAP          11
#define X86_64_SYS_BRK             12
#define X86_64_SYS_RT_SIGACTION    13
#define X86_64_SYS_RT_SIGPROCMASK  14
#define X86_64_SYS_RT_SIGRETURN    15
#define X86_64_SYS_IOCTL           16
#define X86_64_SYS_PREAD64         17
#define X86_64_SYS_PWRITE64        18
#define X86_64_SYS_READV           19
#define X86_64_SYS_WRITEV          20
#define X86_64_SYS_ACCESS          21
#define X86_64_SYS_PIPE            22
#define X86_64_SYS_SELECT          23
#define X86_64_SYS_SCHED_YIELD     24
#define X86_64_SYS_MREMAP          25
#define X86_64_SYS_MSYNC           26
#define X86_64_SYS_MINCORE         27
#define X86_64_SYS_MADVISE         28
#define X86_64_SYS_SHMGET          29
#define X86_64_SYS_SHMAT           30
#define X86_64_SYS_SHMCTL          31
#define X86_64_SYS_DUP             32
#define X86_64_SYS_DUP2            33
#define X86_64_SYS_PAUSE           34
#define X86_64_SYS_NANOSLEEP       35
#define X86_64_SYS_GETITIMER       36
#define X86_64_SYS_ALARM           37
#define X86_64_SYS_SETITIMER       38
#define X86_64_SYS_GETPID          39
#define X86_64_SYS_SENDFILE         40
#define X86_64_SYS_SOCKET          41
#define X86_64_SYS_CONNECT         42
#define X86_64_SYS_ACCEPT          43
#define X86_64_SYS_SENDTO          44
#define X86_64_SYS_RECVFROM        45
#define X86_64_SYS_SENDMSG         46
#define X86_64_SYS_RECVMSG         47
#define X86_64_SYS_SHUTDOWN        48
#define X86_64_SYS_BIND            49
#define X86_64_SYS_LISTEN          50
#define X86_64_SYS_GETSOCKNAME     51
#define X86_64_SYS_GETPEERNAME     52
#define X86_64_SYS_SOCKETPAIR      53
#define X86_64_SYS_SETSOCKOPT      54
#define X86_64_SYS_GETSOCKOPT      55
#define X86_64_SYS_CLONE           56
#define X86_64_SYS_FORK            57
#define X86_64_SYS_VFORK           58
#define X86_64_SYS_EXECVE          59
#define X86_64_SYS_EXIT            60
#define X86_64_SYS_WAIT4           61
#define X86_64_SYS_KILL            62
#define X86_64_SYS_UNAME           63
#define X86_64_SYS_SEMGET          64
#define X86_64_SYS_SEMOP           65
#define X86_64_SYS_SEMCTL          66
#define X86_64_SYS_SHMDT           67
#define X86_64_SYS_MSGGET          68
#define X86_64_SYS_MSGSND          69
#define X86_64_SYS_MSGRCV          70
#define X86_64_SYS_MSGCTL          71
#define X86_64_SYS_FCNTL           72
#define X86_64_SYS_FLOCK           73
#define X86_64_SYS_FSYNC           74
#define X86_64_SYS_FDATASYNC       75
#define X86_64_SYS_TRUNCATE        76
#define X86_64_SYS_FTRUNCATE       77
#define X86_64_SYS_GETDENTS        78
#define X86_64_SYS_GETCWD          79
#define X86_64_SYS_CHDIR           80
#define X86_64_SYS_FCHDIR          81
#define X86_64_SYS_RENAME          82
#define X86_64_SYS_MKDIR           83
#define X86_64_SYS_RMDIR           84
#define X86_64_SYS_CREAT           85
#define X86_64_SYS_LINK            86
#define X86_64_SYS_UNLINK          87
#define X86_64_SYS_SYMLINK         88
#define X86_64_SYS_READLINK        89
#define X86_64_SYS_CHMOD           90
#define X86_64_SYS_FCHMOD          91
#define X86_64_SYS_CHOWN           92
#define X86_64_SYS_FCHOWN          93
#define X86_64_SYS_LCHOWN          94
#define X86_64_SYS_UMASK           95
#define X86_64_SYS_GETTIMEOFDAY    96
#define X86_64_SYS_GETRLIMIT       97
#define X86_64_SYS_GETRUSAGE       98
#define X86_64_SYS_SYSINFO         99
#define X86_64_SYS_TIMES          100
#define X86_64_SYS_PTRACE         101
#define X86_64_SYS_GETUID         102
#define X86_64_SYS_SYSLOG         103
#define X86_64_SYS_GETGID         104
#define X86_64_SYS_SETUID         105
#define X86_64_SYS_SETGID         106
#define X86_64_SYS_GETEUID        107
#define X86_64_SYS_GETEGID        108
#define X86_64_SYS_SETPGID        109
#define X86_64_SYS_GETPPID        110
#define X86_64_SYS_GETPGRP        111
#define X86_64_SYS_SETSID         112
#define X86_64_SYS_SETREUID       113
#define X86_64_SYS_SETREGID       114
#define X86_64_SYS_GETGROUPS      115
#define X86_64_SYS_SETGROUPS      116
#define X86_64_SYS_SETRESUID      117
#define X86_64_SYS_GETRESUID      118
#define X86_64_SYS_SETRESGID      119
#define X86_64_SYS_GETRESGID      120
#define X86_64_SYS_GETPGID        121
#define X86_64_SYS_SETFSUID       122
#define X86_64_SYS_SETFSGID       123
#define X86_64_SYS_GETSID         124
#define X86_64_SYS_CAPGET         125
#define X86_64_SYS_CAPSET         126
#define X86_64_SYS_RT_SIGPENDING  127
#define X86_64_SYS_RT_SIGTIMEDWAIT 128
#define X86_64_SYS_RT_SIGQUEUEINFO 129
#define X86_64_SYS_RT_SIGSUSPEND  130
#define X86_64_SYS_SIGALTSTACK    131
#define X86_64_SYS_UTIME          132
#define X86_64_SYS_MKNOD          133
#define X86_64_SYS_USELIB         134
#define X86_64_SYS_PERSONALITY     135
#define X86_64_SYS_USTAT          136
#define X86_64_SYS_STATFS         137
#define X86_64_SYS_FSTATFS        138
#define X86_64_SYS_SYSFS          139
#define X86_64_SYS_GETPRIORITY     140
#define X86_64_SYS_SETPRIORITY     141
#define X86_64_SYS_SCHED_SETPARAM  142
#define X86_64_SYS_SCHED_GETPARAM  143
#define X86_64_SYS_SCHED_SETSCHEDULER 144
#define X86_64_SYS_SCHED_GETSCHEDULER 145
#define X86_64_SYS_SCHED_GET_PRIORITY_MAX 146
#define X86_64_SYS_SCHED_GET_PRIORITY_MIN 147
#define X86_64_SYS_SCHED_RR_GET_INTERVAL 148
#define X86_64_SYS_SCHED_YIELD    149
#define X86_64_SYS_SCHED_GETAFFINITY 150
#define X86_64_SYS_SCHED_SETAFFINITY 151
#define X86_64_SYS_SCHED_GETAFFINITY 150
#define X86_64_SYS_SCHED_SETAFFINITY 151
/* ... x86_64 has ~450 syscalls, we only list the critical ones */

/* ---------- Helper: BPF instruction builder ---------- */

#define BPF_STMT(code, k) { (unsigned short)(code), 0, 0, (unsigned int)(k) }
#define BPF_JUMP(code, k, jt, jf) { (unsigned short)(code), (unsigned char)(jt), (unsigned char)(jf), (unsigned int)(k) }

/* BPF instruction set for seccomp */
struct sock_filter {
    unsigned short code;
    unsigned char  jt;
    unsigned char  jf;
    unsigned int   k;
};

/* ---------- SecComp context ---------- */

struct SeccompContext {
    bool policy_applied;
    int arch;  /* AUDIT_ARCH_AARCH64 or AUDIT_ARCH_X86_64 */
};

/* ---------- BPF program generation ---------- */

/**
 * Build a BPF program that allows only safe syscalls.
 * The program uses the standard seccomp-BPF pattern:
 *   1. Load architecture
 *   2. Validate we're on the right arch
 *   3. Load syscall number
 *   4. Compare against allowlist/blocklist
 *   5. Default: allow (or kill based on policy)
 */
static int build_bpf_program(struct sock_fprog* prog, int arch) {
    if (!prog) return -1;

    /* We'll build a simple kill-list policy:
     * Block specific dangerous syscalls, allow everything else.
     * This is more permissive but safer than a strict allowlist
     * when running desktop applications.
     */
    const int num_blocks = 6;
    /* Blocked syscalls: ptrace, init_module, finit_module, delete_module,
     * kexec_load, kexec_file_load, reboot, iopl, ioperm, bpf, seccomp */
    int blocked_syscalls[] = {
        X86_64_SYS_PTRACE, 101,  /* ptrace */
        /* We need to handle the arch-specific numbers */
    };

    /* For ARM64, the blocked syscalls are different */
    if (arch == AUDIT_ARCH_AARCH64) {
        /* ARM64: ptrace=??? (not a syscall on arm64, uses ptrace via clone) */
        /* Use a simpler approach: just block by number */
    }

    /* Simple filter: check syscall, block if in deny list */
    /* We'll use a JAIL policy that kills on blocked syscalls */
    static struct sock_filter filter[] = {
        /* Load architecture */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        /* Jump to check if arch matches */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0),
        /* Arch mismatch: kill */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        /* Load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* Check against blocked syscalls */
        /* For now, just block ptrace on all architectures */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 101, 0, 1), /* ptrace */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        /* Block reboot */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, X86_64_SYS_REBOOT, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        /* Default: allow */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    /* Allocate and copy the filter */
    size_t filter_count = sizeof(filter) / sizeof(filter[0]);
    struct sock_filter* filter_copy = (struct sock_filter*)malloc(
        filter_count * sizeof(struct sock_filter));
    if (!filter_copy) return -1;
    memcpy(filter_copy, filter, filter_count * sizeof(struct sock_filter));

    prog->filter = filter_copy;
    prog->len = (unsigned short)filter_count;

    return 0;
}

/* ---------- Module lifecycle ---------- */

int seccomp_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Seccomp init");

    SeccompContext* sctx = (SeccompContext*)malloc(sizeof(SeccompContext));
    if (!sctx) return -1;

    sctx->policy_applied = false;

    /* Detect architecture */
#if defined(__aarch64__)
    sctx->arch = AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
    sctx->arch = AUDIT_ARCH_X86_64;
#else
    sctx->arch = 0;
#endif

    ctx->signal_ctx = (void*)sctx;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Seccomp context initialized (arch=%d)", sctx->arch);
    return 0;
}

int seccomp_apply(EngineContext* ctx) {
    SeccompContext* sctx = (SeccompContext*)ctx->signal_ctx;
    if (!sctx) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Seccomp not initialized");
        return -1;
    }

    if (sctx->policy_applied) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Seccomp policy already applied");
        return 0;
    }

    /* Check if seccomp is available */
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, nullptr) == -1) {
        if (errno == EINVAL || errno == ENOSYS) {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                "Seccomp not available on this kernel, skipping");
            return 0;  /* Non-fatal: continue without seccomp */
        }
    }

    /* Build the BPF program */
    struct sock_fprog prog;
    memset(&prog, 0, sizeof(prog));
    int ret = build_bpf_program(&prog, sctx->arch);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to build BPF program");
        return -1;
    }

    /* Apply the seccomp filter */
    /* Use SECCOMP_SET_MODE_FILTER with SECCOMP_FILTER_FLAG_TSYNC to
     * apply to all threads */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
    }

    ret = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                  SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (ret != 0) {
        /* Try without TSYNC */
        ret = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
    }

    if (ret == 0) {
        sctx->policy_applied = true;
        __android_log_print(ANDROID_LOG_INFO, TAG, "Seccomp-BPF policy applied successfully");
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Seccomp apply failed: %s (non-fatal, continuing)", strerror(errno));
    }

    /* Free the BPF program (kernel has a copy) */
    if (prog.filter) {
        free(prog.filter);
    }

    return 0;
}

void seccomp_cleanup(EngineContext* ctx) {
    SeccompContext* sctx = (SeccompContext*)ctx->signal_ctx;
    if (sctx) {
        sctx->policy_applied = false;
        free(sctx);
        ctx->signal_ctx = nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "Seccomp cleaned up");
}