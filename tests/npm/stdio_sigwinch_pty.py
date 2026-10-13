"""Verify stdio resize ordering and unreferenced signal subscriptions on a real PTY."""
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios


def resize(fd, columns, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))


def acknowledge(reader, expected):
    # External test deadline, not a delay before sending or inspecting an event.
    ready, _, _ = select.select([reader], [], [], 15)
    assert ready, 'fixture did not acknowledge: ' + repr(expected)
    actual = reader.readline()
    assert actual == expected, (expected, actual)


binary = sys.argv[1]
for mode in ('stdout', 'stderr', 'both', 'exit'):
    master, slave = pty.openpty()
    ack_read, ack_write = os.pipe()
    child = None
    try:
        resize(slave, 80, 24)
        env = dict(os.environ, TOV_STDIO_SIGWINCH=mode, TOV_SIGWINCH_ACK_FD=str(ack_write))
        child = subprocess.Popen(
            [binary], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE if mode == 'stderr' else slave,
            stderr=subprocess.PIPE if mode == 'stdout' else slave,
            env=env, pass_fds=(ack_write,))
        os.close(ack_write)
        ack_write = None
        with os.fdopen(ack_read, 'rb', buffering=0) as reader:
            ack_read = None
            if mode != 'exit':
                acknowledge(reader, b'READY\n')
                for count, (columns, rows) in enumerate(((101, 37), (119, 43), (119, 43)), 1):
                    resize(slave, columns, rows)
                    # openpty does not give the child a controlling terminal, so deliver the
                    # kernel notification explicitly after updating the terminal's durable size.
                    os.kill(child.pid, signal.SIGWINCH)
                    acknowledge(reader, ('SIGNAL %d\n' % count).encode())
            out, err = child.communicate(timeout=15)
            assert child.returncode == 0, (mode, child.returncode, out, err)
    finally:
        if child is not None:
            if child.poll() is None:
                child.kill()
            child.wait()
        for fd in (master, slave, ack_read, ack_write):
            if fd is not None:
                os.close(fd)
print('stdio SIGWINCH resize and exit ok')
