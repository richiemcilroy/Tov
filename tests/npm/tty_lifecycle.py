"""Check lazy terminal reads, PTY EOF and default signal mode restoration."""
import os
import pty
import select
import signal
import subprocess
import sys
import termios


def settings(fd):
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~getattr(termios, "PENDIN", 0)
    return attrs


def line(child, mode):
    ready, _, _ = select.select([child.stdout], [], [], 15)
    assert ready, f"{mode}: fixture did not acknowledge its state"
    return child.stdout.readline()


binary = sys.argv[1]
cases = sys.argv[2:] or ["probe", "master-eof", "signal-term", "signal-int", "signal-listener", "signal-removed",
                             "signal-busy", "signal-cleared", "signal-plain", "signal-prior"]
for mode in cases:
    master, slave = pty.openpty()
    original = settings(slave)
    env = dict(os.environ, TOV_TTY_TEST=mode, TOV_TTY_MASTER=str(master))
    # EOF requires the reader to inherit only the master, never a slave fd.
    child = subprocess.Popen([binary], stdin=subprocess.DEVNULL if mode == "master-eof" else slave,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                             pass_fds=(master,) if mode == "master-eof" else ())
    try:
        if mode == "probe":
            out, err = child.communicate(timeout=15)
            assert child.returncode == 0, err
            assert out == b"true\n", out
            continue
        assert line(child, mode) == b"READY\n"
        if mode in ("signal-cleared", "signal-plain"):
            assert settings(slave) == original
        else:
            assert not settings(slave)[3] & termios.ICANON
        if mode == "master-eof":
            os.close(slave)
            slave = None
            out, err = child.communicate(timeout=15)
            assert child.returncode == 0, err
            assert out == b"EOF\n", out
            continue
        sig = signal.SIGINT if mode == "signal-int" else signal.SIGTERM
        os.kill(child.pid, sig)
        if mode == "signal-listener":
            assert line(child, mode) == b"LISTENER\n"
            assert not settings(slave)[3] & termios.ICANON, "listener must own terminal reset"
            os.write(master, b"x")
            out, err = child.communicate(timeout=15)
            assert child.returncode == 0, err
            assert out == b"DONE\n", out
        else:
            out, err = child.communicate(timeout=15)
            assert child.returncode == -sig, (child.returncode, out, err)
        assert settings(slave) == original, (mode, "terminal settings were not restored")
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
        os.close(master)
        if slave is not None:
            os.close(slave)
print("tty lifecycle ok")
