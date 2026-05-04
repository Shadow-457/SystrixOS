/* ================================================================
 *  Systrix OS — kernel/fork_exec.c
 *  sys_fork + sys_execve + required syscall stubs
 * ================================================================ */
#include "../include/kernel.h"

static inline PCB *get_pcb(u64 pid)
{
    return (PCB *)(PROC_TABLE + pid * PROC_PCB_SIZE);
}

/* ────────────────────────────────────────────────────────────────
 *  sys_fork
 * ──────────────────────────────────────────────────────────────── */
i64 sys_fork(void)
{
    print_str("[fork] A\n");
    PCB *parent = process_current_pcb();
    if (!parent) return -1;

    print_str("[fork] B\n");
    /* process_create() already calls vmm_create_space + vmm_map_kernel
       and stores the result in child->cr3. We just use that. */
    i64 child_pid = process_create(parent->entry, parent->name);
    if (child_pid < 0) return -1;

    print_str("[fork] C\n");
    PCB *child = get_pcb((u64)child_pid);

    print_str("[fork] C2\n");
    /* Copy all user pages from parent into child's already-prepared cr3.
       vmm_cow_fork also calls vmm_map_kernel internally — that's fine,
       it will just overwrite PML4[0] with a fresh copy. */
    vmm_cow_fork(parent->cr3, child->cr3);

    print_str("[fork] cow done\n");

    /* copy user-space CPU state */
    child->ursp  = parent->ursp;
    child->entry = parent->entry;
    child->brk   = parent->brk;

    print_str("[fork] enqueue\n");
    process_run((u64)child_pid);

    return child_pid;
}

/* ────────────────────────────────────────────────────────────────
 *  sys_execve
 * ──────────────────────────────────────────────────────────────── */
i64 sys_execve(const char *path, char **argv, char **envp)
{
    (void)argv; (void)envp;
    print_str("[exec] loading\n");

    char name83[12];
    format_83_name(path, name83);

    u32 fsize = 0;
    if (fat32_find_file(name83, &fsize) < 0 || fsize == 0) {
        print_str("[exec] not found\n");
        return -1;
    }

    u32 npages = (fsize + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 buf_phys = pmm_alloc_n(npages);
    if (!buf_phys) return -1;

    i64 fd = vfs_open(path);
    if (fd < 0) { print_str("[exec] open failed\n"); return -1; }
    vfs_read((u64)fd, (void *)buf_phys, fsize);

    i64 r = elf_load((void *)buf_phys, (usize)fsize, path);
    if (r < 0) { print_str("[exec] elf failed\n"); return -1; }

    print_str("[exec] ok\n");
    return 0;
}

/* ────────────────────────────────────────────────────────────────
 *  Syscall stubs required by isr.S
 * ──────────────────────────────────────────────────────────────── */
i64 sys_clone(u64 flags, void *stack, void *ptid, void *ctid, void *newtls)
{
    (void)flags; (void)stack; (void)ptid; (void)ctid; (void)newtls;
    return sys_fork();
}

i64 sys_wait4(u64 pid, int *wstatus, u64 options, void *ru)
{
    (void)pid; (void)wstatus; (void)options; (void)ru;
    return -1;
}

i64 sys_exit_group(i64 code)
{
    (void)code;
    PCB *pcb = process_current_pcb();
    if (pcb) process_destroy(pcb);
    for (;;) __asm__ volatile ("hlt");
    return 0;
}
