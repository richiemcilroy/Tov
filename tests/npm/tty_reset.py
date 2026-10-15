"""setResetOnExit: the sequence reaches the terminal once, after the program's output,
however the program ends; on a trap before the trap's message; never once cleared."""
import os
import pty
import signal
import subprocess
import sys

RESET = b"<RESET>"


def run(binary, mode):
    master, slave = pty.openpty()
    child = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave,
                             env=dict(os.environ, TOV_TTY_TEST=mode))
    os.close(slave)
    out = bytearray()
    while True:
        try:
            chunk = os.read(master, 65536)
        except OSError:  # EIO: every slave fd is closed, the program is gone
            break
        if not chunk:
            break
        out += chunk
        if mode == "signal" and b"READY" in out and child.poll() is None:
            os.kill(child.pid, signal.SIGTERM)
    os.close(master)
    return child.wait(), bytes(out).replace(b"\r\n", b"\n")


binary = sys.argv[1]
for mode, status in [("exit", 0), ("process-exit", 3), ("uncaught", 1), ("trap", 101),
                     ("signal", -signal.SIGTERM), ("cleared", 0)]:
    got, out = run(binary, mode)
    assert got == status, (mode, got, out)
    # A fatal signal ends it without flushing the runtime's stdout buffer: only the
    # directly written READY comes before the reset.
    assert out.startswith(b"READY\n" if mode == "signal" else b"before\n"), (mode, out)
    if mode == "cleared":
        assert RESET not in out, (mode, out)
        continue
    assert out.count(RESET) == 1, (mode, out)
    if mode == "trap":
        assert out.index(RESET) < out.index(b"trap: "), (mode, out)
    elif mode == "uncaught":
        assert out.index(RESET) > out.index(b"boom"), (mode, out)
    else:
        assert out.endswith(RESET), (mode, out)
print("tty reset ok")
