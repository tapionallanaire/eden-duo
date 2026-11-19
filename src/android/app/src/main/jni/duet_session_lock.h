// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

inline int DuetOpenSessionLease(const char* path) {
    const int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
    const int error = errno;
    // Never LOCK_UN: Binder duplicates share this open file description.
    close(fd);
    return -error;
}
