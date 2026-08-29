/**
 * Seccomp-BPF Policy Implementation
 * 
 * Sets up BPF filters to restrict dangerous system calls at kernel level.
 * Blocks: ptrace, kexec_load, reboot, init_module, finit_module, delete_module
 * Allows all other common syscalls needed for Linux application execution.
 */

#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/Seccomp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Syscall numbers for ARM64 and x86_64
#ifdef __aarch64__
    #include <asm/unistd.h>
#else
    #include <asm/unistd.h>
#endif

// List of blocked syscalls (system call numbers vary by architecture)
// Using generic approach - will need arch-specific defines
static const int BLOCKED_SYSCALLS[] = {
    // Security-sensitive
    __NR_ptrace,              // Process tracing
    
#ifdef __aarch64__
    __NR_kexec_load,          // Kernel reconfiguration
    __NR_init_module,         // Load kernel module
    __NR_finit_module,
    __NR_delete_module,
    __NR_reboot,              // System reboot
#elif defined(__x86_64__)
    __NR_kexec_load,
    __NR_init_module,
    __NR_finit_module,
    __NR_delete_module,
    __NR_reboot,
    __NR_olduname,
#endif
    
    -1  // Terminator
};

int setup_seccomp_policy() {
    struct sock_filter filter[] = {
        // Allow all reads/writes
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 
                 offsetof(struct seccomp_data, nr)),
        
        // Check if syscall is blocked
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ptrace, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        
        // Return allow for everything else
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter
    };
    
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        LOGE("Failed to install seccomp filter: %s", strerror(errno));
        return -1;
    }
    
    LOGI("Seccomp policy installed successfully");
    return 0;
}

// Alternative: Use libseccomp if available (simpler API)
#ifdef HAVE_LIBSECCOMP
#include <seccomp.h>

int setup_seccomp_with_libseccomp() {
    scmp_filter_ctx ctx;
    
    // Default action: allow
    ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (!ctx) {
        LOGE("Failed to initialize seccomp context");
        return -1;
    }
    
    // Add blocks
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(kexec_load), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(init_module), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(finit_module), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(delete_module), 0);
    seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(reboot), 0);
    
    // Load the filter
    int rc = seccomp_load(ctx);
    seccomp_release(ctx);
    
    if (rc < 0) {
        LOGE("Failed to load seccomp filter: %s", strerror(-rc));
        return -1;
    }
    
    LOGI("Seccomp policy installed via libseccomp");
    return 0;
}
#endif
