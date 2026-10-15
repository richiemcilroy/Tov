"""Run the compiled tty fixture with real terminal descriptors and typed input."""
import fcntl
import os
import pty
import select
import socket
import struct
import subprocess
import tempfile
import sys
import termios

def settings(fd):
    attrs = termios.tcgetattr(fd)
    # macOS sets PENDIN (pending input reprocessing) on raw-to-canonical
    # transitions. It is kernel state, not a changed terminal setting.
    attrs[3] &= ~getattr(termios, "PENDIN", 0)
    return attrs


binary = sys.argv[1]
unix, peer = socket.socketpair()
with unix, peer, socket.socket() as tcp, socket.socket(type=socket.SOCK_DGRAM) as udp:
    sockets = dict(PIPE=unix, TCP=tcp, UDP=udp)
    env = dict(os.environ, TOV_TTY_TEST="types")
    env.update({"TOV_FD_" + kind: str(sock.fileno()) for kind, sock in sockets.items()})
    result = subprocess.run([binary], input=b"pipe keys", capture_output=True, env=env,
                            pass_fds=tuple(sock.fileno() for sock in sockets.values()), timeout=15)
    assert result.returncode == 0, result.stderr
    assert result.stdout == b"types ok\n", result.stdout

with tempfile.TemporaryFile() as file:
    file.write(b"file keys")
    file.seek(0)
    for source, expected in ((file, "file keys"), (subprocess.DEVNULL, "")):
        env = dict(os.environ, TOV_TTY_TEST="plain", TOV_TTY_EXPECTED=expected)
        result = subprocess.run([binary], stdin=source, capture_output=True, env=env, timeout=15)
        assert result.returncode == 0, result.stderr
        assert result.stdout == b"plain ok\n", result.stdout

for mode in ("stdin", "fd", "exit", "master", "readonly"):
    master, slave = pty.openpty()
    other_master, other_slave = pty.openpty()
    other_original = settings(other_slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    original = settings(slave)
    env = dict(os.environ, TOV_TTY_TEST=mode, TOV_TTY_FD=str(slave), TOV_TTY_PATH=os.ttyname(slave),
               TOV_TTY_MASTER=str(master), TOV_TTY_OTHER=str(other_master))
    readonly = os.open(os.ttyname(slave), os.O_RDONLY | os.O_NOCTTY) if mode == "readonly" else None
    if readonly is not None:
        source = readonly
    elif mode == "master":
        source = master
    elif mode == "fd":
        source = subprocess.DEVNULL
    else:
        source = slave
    child = subprocess.Popen([binary], stdin=source,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=env, pass_fds=(slave, master, other_master))
    try:
        # The producer acknowledges raw mode before the parent types un-delimited keys.
        ready, _, _ = select.select([child.stdout], [], [], 15)
        assert ready, "fixture did not acknowledge raw mode"
        line = child.stdout.readline()
        assert line == b"READY\n", (line, child.poll())
        raw = termios.tcgetattr(slave)
        assert not raw[3] & (termios.ICANON | termios.ECHO | termios.ISIG), raw
        if mode != "master":
            assert not fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK, "stdin changed shared stdio flags"
        if mode != "readonly":
            output_fd = slave if mode == "master" else master
            ready, _, _ = select.select([output_fd], [], [], 15)
            assert ready, "TTY handle did not write output"
            assert os.read(output_fd, 6) == b"output"
        other_raw = termios.tcgetattr(other_slave)
        assert not other_raw[3] & termios.ICANON, "second terminal was not independently made raw"
        os.write(slave if mode == "master" else master, b"keys")
        out, err = child.communicate(timeout=15)
        assert child.returncode == 0, err
        assert out == b"read ok\n", (out, err)
        assert settings(slave) == original, "terminal settings were not restored"
        assert settings(other_slave) == other_original, "second terminal was not restored"
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
        os.close(master)
        os.close(slave)
        os.close(other_master)
        os.close(other_slave)
        if readonly is not None:
            os.close(readonly)
master, slave = pty.openpty()
try:
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    child = subprocess.run([binary], stdin=slave, stdout=slave, stderr=slave,
                           env=dict(os.environ, TOV_TTY_TEST="stdio"), timeout=15)
    assert child.returncode == 0
    output = b""
    for _ in range(4):
        ready, _, _ = select.select([master], [], [], 15)
        assert ready, "terminal stdout/stderr did not write"
        output += os.read(master, 4096)
        if output.endswith(b"stderr ok\r\n"):
            break
    assert output == b"stdout ok\r\nstderr ok\r\n", output
    assert not fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK
finally:
    os.close(master)
    os.close(slave)
subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "tty_lifecycle.py"), binary],
               check=True, stdout=subprocess.PIPE, timeout=120)
print("tty types, stdin, fd and exit ok")
