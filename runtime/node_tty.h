#ifndef TV_NODE_TTY_H
#define TV_NODE_TTY_H

#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/* Raw-mode state belongs to the terminal, not to a process-global snapshot.
   Distinct handles of the same terminal share ownership of that snapshot. */
typedef struct tv_node_tty_mode {
    struct tv_node_tty_mode *next;
    dev_t dev, rdev;
    ino_t ino;
    int fd, owners;
    int pty_number; /* Linux PTY masters can share /dev/ptmx's stat identity. */
    struct termios saved;
} tv_node_tty_mode;
static tv_node_tty_mode *tv_node_tty_modes;

static int tv_node_signal_defaults(void);
static void tv_node_signal_defaults_stop(void);

static void tv_node_tty_reset(void) {
    for (tv_node_tty_mode *m = tv_node_tty_modes; m; m = m->next)
        tcsetattr(m->fd, TCSANOW, &m->saved);
}

static void tv_node_tty_block_signals(sigset_t *before) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, before);
}

static int tv_node_tty_release(tv_node_tty_mode *m, bool closing) {
    sigset_t before;
    tv_node_tty_block_signals(&before);
    if (m->owners > 1) {
        m->owners--;
        sigprocmask(SIG_SETMASK, &before, NULL);
        return 0;
    }
    int err = tcsetattr(m->fd, TCSADRAIN, &m->saved) == 0 ? 0 : -errno;
    if (err) {
        /* A closing handle cannot retry, but its saved modes must survive:
           retain the restoration fd for default-signal/exit reset or a later
           owner of this terminal. No slave is opened for a master snapshot. */
        if (closing) m->owners = 0;
        sigprocmask(SIG_SETMASK, &before, NULL);
        return err;
    }
    tv_node_tty_mode **p = &tv_node_tty_modes;
    while (*p != m) p = &(*p)->next;
    *p = m->next;
    if (!tv_node_tty_modes) tv_node_signal_defaults_stop();
    close(m->fd);
    free(m);
    sigprocmask(SIG_SETMASK, &before, NULL);
    return err;
}

#endif
