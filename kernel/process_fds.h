/* Descriptor-table ownership. Included by proc.c after VMM/VFS declarations.
 * Lock order: descriptor metadata -> file-pool metadata / bounded VMM copy.
 * No driver operation, allocation, close callback or sleep under this lock.
 * A syscall's kernel-entry lifetime retains its process and leader. */
static spinlock_t descriptor_lock;
static bool descriptors_lock(void) {
    bool irq = irq_save();
    while (__atomic_exchange_n(&descriptor_lock.held, 1u, __ATOMIC_ACQUIRE)) {
        smp_tlb_poll();
        __asm__ volatile("pause");
    }
    return irq;
}
static void descriptors_unlock(bool irq) { spin_unlock_irqrestore(&descriptor_lock, irq); }

file_t *proc_fd_acquire(proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    bool irq = descriptors_lock();
    file_t *f = vfs_file_ref(proc_shared(p)->fds[fd]);
    descriptors_unlock(irq);
    return f;
}

int proc_fd_alloc(proc_t *p, file_t *f) {
    if (!p || !f) return -E_INVAL;
    bool irq = descriptors_lock();
    p = proc_shared(p);
    int fd = -E_MFILE;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i]) continue;
        p->fds[i] = f; /* transfer caller's existing reference */
        fd = i;
        break;
    }
    descriptors_unlock(irq);
    return fd;
}

int proc_fd_close(proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= PROC_MAX_FDS) return -E_BADF;
    bool irq = descriptors_lock();
    p = proc_shared(p);
    file_t *f = p->fds[fd];
    p->fds[fd] = NULL;
    descriptors_unlock(irq);
    if (!f) return -E_BADF;
    vfs_close(f); /* in-flight users hold independent references */
    return 0;
}

int proc_fd_dup(proc_t *p, int source, int target) {
    if (!p || source < 0 || source >= PROC_MAX_FDS) return -E_BADF;
    if (target < -1 || target >= PROC_MAX_FDS) return -E_INVAL;
    bool irq = descriptors_lock();
    p = proc_shared(p);
    file_t *f = p->fds[source], *old = NULL;
    if (!f) { descriptors_unlock(irq); return -E_BADF; }
    if (source == target) { descriptors_unlock(irq); return target; }
    if (target == -1) {
        for (int i = 0; i < PROC_MAX_FDS; i++) if (!p->fds[i]) { target = i; break; }
        if (target == -1) { descriptors_unlock(irq); return -E_MFILE; }
    }
    if (!vfs_file_ref(f)) { descriptors_unlock(irq); return -E_MFILE; }
    old = p->fds[target];
    p->fds[target] = f; /* shared description, flags and position, not a copy */
    descriptors_unlock(irq);
    vfs_close(old);
    return target;
}

int proc_fd_install_pipe(proc_t *p, file_t *reader, file_t *writer, u64 address) {
    if (!p || !reader || !writer) return -E_INVAL;
    bool irq = descriptors_lock();
    proc_t *lead = proc_shared(p);
    int pair[2], count = 0;
    for (int i = 0; i < PROC_MAX_FDS && count < 2; i++)
        if (!lead->fds[i]) pair[count++] = i;
    int result = count == 2 ? 0 : -E_MFILE;
    /* Publish neither descriptor if the result cannot be copied. Holding this
     * metadata lock across an eight-byte protected copy prevents a sibling
     * closing/reusing a half-published pair during failure rollback. */
    if (!result && !vmm_user_copy(p->pml4, pair, address, sizeof pair, true)) result = -E_INVAL;
    if (!result) { lead->fds[pair[0]] = reader; lead->fds[pair[1]] = writer; }
    descriptors_unlock(irq);
    return result; /* success transfers both references; failure transfers none */
}

bool proc_fd_inherit_stdio(proc_t *child, proc_t *parent) {
    if (!child || !parent) return false;
    file_t *refs[3] = {0};
    bool irq = descriptors_lock(), ok = true;
    parent = proc_shared(parent);
    for (int i = 0; i < 3; i++) {
        if (child->fds[i]) panic("process: inheriting into occupied stdio");
        if (parent->fds[i] && !(refs[i] = vfs_file_ref(parent->fds[i]))) { ok = false; break; }
    }
    if (ok) for (int i = 0; i < 3; i++) child->fds[i] = refs[i];
    descriptors_unlock(irq);
    if (!ok) for (int i = 0; i < 3; i++) vfs_close(refs[i]);
    return ok;
}

void proc_fd_close_all(proc_t *p) {
    if (!p || p->leader) return; /* only the drained leader owns the table */
    file_t *retired[PROC_MAX_FDS];
    bool irq = descriptors_lock();
    for (int i = 0; i < PROC_MAX_FDS; i++) { retired[i] = p->fds[i]; p->fds[i] = NULL; }
    descriptors_unlock(irq);
    for (int i = 0; i < PROC_MAX_FDS; i++) vfs_close(retired[i]);
}
