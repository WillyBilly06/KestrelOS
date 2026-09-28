/* Protected user I/O. Never let filesystem/device callbacks retain a pointer
 * into a mapping another application thread can remove while the driver waits.
 * Lock order: description position -> filesystem -> bounded VMM copy.
 * No VMM lock is retained across a driver operation or scheduler wait. */
enum { VFS_IOV_CHECK_WRITE, VFS_IOV_FROM_USER, VFS_IOV_TO_USER };
static bool vfs_iov_access(u64 pml4, const vfs_iovec_t *iov, size_t count,
                           size_t index, u64 offset, void *buffer, size_t bytes,
                           int mode, size_t *copied) {
    *copied = 0;
    while (*copied < bytes && index < count) {
        if (offset == iov[index].len) { index++; offset = 0; continue; }
        size_t piece = bytes - *copied;
        if ((u64)piece > iov[index].len - offset) piece = (size_t)(iov[index].len - offset);
        u64 address = iov[index].base + offset;
        bool ok = mode == VFS_IOV_CHECK_WRITE
            ? vmm_user_range_ok(pml4, address, piece, true)
            : vmm_user_copy(pml4, (u8 *)buffer + *copied, address, piece, mode == VFS_IOV_TO_USER);
        if (!ok) return false;
        *copied += piece;
        offset += piece;
    }
    return *copied == bytes;
}
static void vfs_iov_advance(const vfs_iovec_t *iov, size_t count,
                            size_t *index, u64 *offset, size_t bytes) {
    while (*index < count) {
        u64 remaining = iov[*index].len - *offset;
        if ((u64)bytes < remaining) { *offset += bytes; return; }
        bytes -= (size_t)remaining;
        (*index)++; *offset = 0;
    }
}
ssize_t_k vfs_user_iov(file_t *f, u64 pml4, const vfs_iovec_t *iov, size_t count, bool write) {
    if (!f || !f->in_use || !f->vn || !f->vn->ops) return -E_BADF;
    if ((f->flags & O_ACCMODE) == (write ? O_RDONLY : O_WRONLY)) return -E_BADF;
    if ((write && !f->vn->ops->write) || (!write && !f->vn->ops->read)) return -E_NOSYS;
    if (!pml4 || count > VFS_IOV_MAX || (count && !iov)) return -E_INVAL;
    u64 total = 0, user_end = 1ull << 47;
    for (size_t i = 0; i < count; i++) {
        if (iov[i].len > (~0ull >> 1) - total) return -E_INVAL;
        if (iov[i].len && (iov[i].base < PAGE_SIZE || iov[i].base >= user_end ||
                          iov[i].len > user_end - iov[i].base)) return -E_INVAL;
        total += iov[i].len;
    }
    if (!total) return 0;
    size_t capacity = total < VMM_USER_COPY_MAX ? (size_t)total : VMM_USER_COPY_MAX;
    void *scratch = kmalloc(capacity);
    if (!scratch) return -E_NOMEM;
    bool position = file_position_enter(f);
    bool guard = f->vn->fs && !f->vn->fs->reentrant;
    if (guard) fs_enter();
    bool stream = f->vn->type == VN_CHR;
    size_t index = 0;
    u64 offset = 0, done = 0;
    ssize_t_k result = 0;
    /* Keep one description's offset transaction across every vector/chunk,
     * including copy failures. A dup cannot interleave a second write halfway
     * through this writev. Character streams intentionally remain duplex. */
    while (done < total) {
        size_t chunk = total - done < capacity ? (size_t)(total - done) : capacity;
        if (!stream) {
            if (write && (f->flags & O_APPEND)) f->pos = f->vn->size;
            if (f->pos > (~0ull >> 1) || chunk > (~0ull >> 1) - f->pos) {
                result = -E_INVAL; break;
            }
        }
        size_t copied;
        if (!vfs_iov_access(pml4, iov, count, index, offset, scratch, chunk,
                            write ? VFS_IOV_FROM_USER : VFS_IOV_CHECK_WRITE, &copied)) {
            result = -E_INVAL; break;
        }
        ssize_t_k n = write ? f->vn->ops->write(f->vn, scratch, chunk, f->pos)
                            : f->vn->ops->read(f->vn, scratch, chunk, f->pos);
        if (n < 0) { result = n; break; }
        if ((u64)n > chunk) { result = -E_IO; break; }
        if (!n) break;
        if (!write && !vfs_iov_access(pml4, iov, count, index, offset, scratch, (size_t)n,
                                     VFS_IOV_TO_USER, &copied)) {
            /* Only report bytes actually copied to the caller. Regular-file
             * position follows that prefix; a stream's consumed bytes cannot
             * be put back after a concurrent destination unmap. */
            if (!stream) f->pos += copied;
            done += copied;
            result = -E_INVAL; break;
        }
        if (!stream) f->pos += (u64)n;
        done += (u64)n;
        vfs_iov_advance(iov, count, &index, &offset, (size_t)n);
        if ((size_t)n < chunk || (!write && stream)) break;
    }
    if (guard) fs_leave();
    if (position) file_position_leave(f);
    kfree(scratch);
    return done ? (ssize_t_k)done : result;
}
ssize_t_k vfs_user_io(file_t *f, u64 pml4, u64 address, size_t len, bool write) {
    const vfs_iovec_t iov = {address, len};
    return vfs_user_iov(f, pml4, &iov, 1, write);
}

int vfs_user_ioctl(file_t *f, u64 pml4, u32 cmd, u64 address) {
    if (!f || !f->in_use || !f->vn || !f->vn->ops) return -E_BADF;
    const vnode_ops_t *ops = f->vn->ops;
    if (!ops->ioctl || !ops->ioctl_shape) return -E_NOSYS;
    vfs_ioctl_shape_t shape = {0};
    int result = ops->ioctl_shape(f->vn, cmd, &shape);
    if (result < 0) return result;
    size_t capacity = shape.in_bytes > shape.out_bytes ? shape.in_bytes : shape.out_bytes;
    if (capacity > VMM_USER_COPY_MAX || !pml4) return -E_INVAL;
    if (!capacity || (!address && !shape.required)) return ops->ioctl(f->vn, cmd, NULL);
    if (!address) return -E_INVAL;
    void *scratch = kmalloc(capacity);
    if (!scratch) return -E_NOMEM;
    /* Some enumeration commands leave unused slots untouched. Never copy
     * uninitialized kernel storage back to the application. */
    memset(scratch, 0, capacity);
    if ((shape.out_bytes && !vmm_user_range_ok(pml4, address, shape.out_bytes, true)) ||
        (shape.in_bytes && !vmm_user_copy(pml4, scratch, address, shape.in_bytes, false))) {
        result = -E_INVAL;
    } else {
        result = ops->ioctl(f->vn, cmd, scratch);
        /* Driver may sleep, or the caller may unmap its argument concurrently.
         * Recheck and copy under the VMM lock, never retain it across callback. */
        if (result >= 0 && shape.out_bytes &&
            !vmm_user_copy(pml4, scratch, address, shape.out_bytes, true)) result = -E_INVAL;
    }
    kfree(scratch);
    return result;
}
