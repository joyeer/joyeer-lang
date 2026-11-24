"""A second request must not get queued behind a slow client while another worker is idle."""

import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def connect(port: int, server: subprocess.Popen) -> socket.socket:
    deadline = time.monotonic() + 8
    while server.poll() is None and time.monotonic() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.2)
        except OSError:
            time.sleep(0.04)
    raise RuntimeError("the example server did not start")


def verify(compiler: Path, example: Path, level: str) -> None:
    with tempfile.TemporaryDirectory(prefix="joyeer-idle-workers-") as directory:
        name = "web-server.exe" if sys.platform == "win32" else "web-server"
        executable = Path(directory) / name
        sources = [example / "src" / (name + ".joyeer") for name in ("main", "http", "site", "markdown")]
        subprocess.run(
            [compiler, "--module-name", "example.web_server", f"-O{level}", "-g0", "-o", executable, "--", *sources],
            check=True, timeout=120,
        )
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server = subprocess.Popen(
            [executable, example / "site", "--address", "127.0.0.1", "--port", str(port), "--workers", "2"]
        )
        try:
            with connect(port, server) as waiting:
                waiting.sendall(b"GET / HTTP/1.1\r\nX-Pending:")
                for index in range(2):
                    with connect(port, server) as client:
                        client.settimeout(2)
                        client.sendall(b"GET / HTTP/1.1\r\n\r\n")
                        with client.makefile("rb") as reply:
                            try:
                                status = reply.readline(128)
                            except TimeoutError as error:
                                raise RuntimeError(f"request {index + 1} stalled behind the slow client") from error
                            if status != b"HTTP/1.1 200 OK\r\n":
                                raise RuntimeError(f"request {index + 1} returned {status!r}")
        finally:
            if server.poll() is None:
                server.kill()
            server.wait(timeout=10)


if __name__ == "__main__":
    verify(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3])
