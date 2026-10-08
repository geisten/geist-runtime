#pragma once
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

struct file_identity {
    dev_t device;
    ino_t inode;
    off_t bytes;
    struct timespec modified, changed;
};
/* Platform differences stay at this file boundary. Enabled model loading
 * refuses symlinks/non-regular files and any identity change during setup. */
[[nodiscard]] static inline bool file_identity_read(const char *path, struct file_identity *out) {
    if (out)
        *out = (struct file_identity){};
    struct stat s;
    if (!path || !out || lstat(path, &s) || !S_ISREG(s.st_mode) || s.st_size <= 0)
        return false;
    *out = (struct file_identity){.device = s.st_dev, .inode = s.st_ino, .bytes = s.st_size};
#ifdef __APPLE__
    out->modified = s.st_mtimespec;
    out->changed = s.st_ctimespec;
#else
    out->modified = s.st_mtim;
    out->changed = s.st_ctim;
#endif
    return true;
}
[[nodiscard]] static inline bool file_identity_same(const struct file_identity *a,
                                                    const struct file_identity *b) {
    return a->device == b->device && a->inode == b->inode && a->bytes == b->bytes &&
           a->modified.tv_sec == b->modified.tv_sec && a->modified.tv_nsec == b->modified.tv_nsec &&
           a->changed.tv_sec == b->changed.tv_sec && a->changed.tv_nsec == b->changed.tv_nsec;
}
