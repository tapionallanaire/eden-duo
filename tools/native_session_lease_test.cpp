// SPDX-License-Identifier: GPL-3.0-or-later
// Linux/Android: c++ -std=c++17 -Wall -Wextra -Werror tools/native_session_lease_test.cpp -o /tmp/native-session-lease-test && /tmp/native-session-lease-test
#include "../src/android/app/src/main/jni/duet_session_lock.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/wait.h>

static void SendFd(int socket, int fd) {
    char byte = 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    auto* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *reinterpret_cast<int*>(CMSG_DATA(cmsg)) = fd;
    assert(sendmsg(socket, &msg, 0) == 1);
}

static int ReceiveFd(int socket) {
    char byte;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    assert(recvmsg(socket, &msg, 0) == 1);
    const auto* cmsg = CMSG_FIRSTHDR(&msg);
    assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
    return *reinterpret_cast<const int*>(CMSG_DATA(cmsg));
}

static void ExpectBusy(const char* path) {
    const int fd = DuetOpenSessionLease(path);
    assert(fd == -EAGAIN || fd == -EWOULDBLOCK);
}

int main() {
    char root[] = "/tmp/duet-lease-XXXXXX";
    assert(mkdtemp(root));
    char path[256];
    assert(std::snprintf(path, sizeof(path), "%s/session.lock", root) > 0);

    // Prelaunch reservation excludes even another open in the same process.
    int reservation = DuetOpenSessionLease(path);
    assert(reservation >= 0);
    ExpectBusy(path);
    int copy = dup(reservation);
    assert(copy >= 0);
    close(reservation);
    ExpectBusy(path); // Activity cancellation cannot unlock an authorized copy still in progress.
    close(copy);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const pid_t owner = fork();
    assert(owner >= 0);
    if (owner == 0) {
        close(sockets[0]);
        const int lease = DuetOpenSessionLease(path);
        assert(lease >= 0);
        SendFd(sockets[1], lease); // Same shared open description semantics as Binder FD handover.
        _exit(0); // Parent-host process death after handover must not release the child's lease.
    }
    close(sockets[1]);
    const int child_lease = ReceiveFd(sockets[0]);
    int status;
    assert(waitpid(owner, &status, 0) == owner && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ExpectBusy(path);
    const int recreated_activity_lease = dup(child_lease);
    assert(recreated_activity_lease >= 0);
    close(child_lease);
    ExpectBusy(path); // A stopped/destroyed Activity is not proof its native writers have exited.
    close(recreated_activity_lease);
    const int maintenance = DuetOpenSessionLease(path);
    assert(maintenance >= 0); // Recovery/sync/maintenance can proceed only after the final description closes.
    close(maintenance);
    close(sockets[0]);

    // Do not follow a replaced stable-guard path.
    assert(unlink(path) == 0);
    assert(symlink("missing-target", path) == 0);
    assert(DuetOpenSessionLease(path) == -ELOOP);
    assert(unlink(path) == 0);
    assert(rmdir(root) == 0);
}
