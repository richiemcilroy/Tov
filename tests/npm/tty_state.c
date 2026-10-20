/* Exercise the production registry's restoration-failure path without
   relying on a platform-specific terminal ioctl failure. */
#include <assert.h>
#include <fcntl.h>
#include <termios.h>

static int failures, calls, stopped;
static int tty_test_tcsetattr(int fd, int action, const struct termios *saved);
#define tcsetattr tty_test_tcsetattr
#include "node_tty.h"
#undef tcsetattr

static int tv_node_signal_defaults(void) { return 0; }
static void tv_node_signal_defaults_stop(void) { stopped++; }

static int tty_test_tcsetattr(int fd, int action, const struct termios *saved) {
    (void)action;
    assert(fcntl(fd, F_GETFD) >= 0);
    assert(saved->c_lflag == ICANON);
    calls++;
    if (failures) { failures--; errno = EINTR; return -1; }
    return 0;
}

int main(void) {
    tv_node_tty_mode *m = calloc(1, sizeof *m);
    m->fd = open("/dev/null", O_RDONLY);
    assert(m->fd >= 0);
    int fd = m->fd;
    m->owners = 2;
    m->saved.c_lflag = ICANON;
    tv_node_tty_mode *other = calloc(1, sizeof *other);
    other->fd = open("/dev/null", O_RDONLY);
    assert(other->fd >= 0);
    other->owners = 1;
    other->saved.c_lflag = ICANON;
    m->next = other;
    tv_node_tty_modes = m;
    assert(tv_node_tty_release(m, true) == 0);
    assert(m->owners == 1 && calls == 0);

    failures = 1;
    assert(tv_node_tty_release(m, true) == -EINTR);
    assert(tv_node_tty_modes == m);
    assert(m->owners == 0 && stopped == 0);
    assert(fcntl(fd, F_GETFD) >= 0);
    tv_node_tty_reset();
    assert(calls == 3);

    /* A later owner can reuse the saved snapshot and finish its release. */
    m->owners = 1;
    assert(tv_node_tty_release(m, false) == 0);
    assert(tv_node_tty_modes == other && stopped == 0);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    assert(tv_node_tty_release(other, false) == 0);
    assert(tv_node_tty_modes == NULL && stopped == 1);
    return 0;
}
