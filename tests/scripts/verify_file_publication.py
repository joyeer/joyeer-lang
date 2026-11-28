"""Verify that concurrent publishers replace the target without exposing partial bytes."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import threading


def publish(program, source, target):
    child = subprocess.run([str(program), str(source), str(target)],
                           capture_output=True, timeout=15)
    if child.returncode != 0 or child.stdout or child.stderr:
        raise RuntimeError(f"publisher exited {child.returncode}: {child.stdout!r} {child.stderr!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    args = parser.parse_args()
    old, first, second = (bytes([value]) * 65536 for value in (0x10, 0x20, 0x30))
    with tempfile.TemporaryDirectory(prefix="joyeer-publish-") as directory:
        root = Path(directory)
        target = root / "published"
        target.write_bytes(old)
        stop = threading.Event()
        ready = threading.Event()
        replaced = threading.Event()
        problems = []

        def watch():
            try:
                while not stop.is_set():
                    contents = target.read_bytes()
                    if contents not in (old, first, second):
                        raise RuntimeError(f"observed unexpected content of {len(contents)} bytes")
                    if contents != old:
                        replaced.set()
                    ready.set()
            except (OSError, RuntimeError) as error:
                problems.append(error)
                ready.set()

        paths = []
        for index in range(8):
            source = root / f"stage-{index}"
            source.write_bytes(first if index % 2 == 0 else second)
            paths.append(source)
        watcher = threading.Thread(target=watch)
        watcher.start()
        try:
            if not ready.wait(5):
                raise RuntimeError("reader did not start")
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(publish, args.program, source, target) for source in paths]
                for future in futures:
                    future.result()
            if not replaced.wait(5):
                raise RuntimeError("reader did not observe a published version")
        finally:
            stop.set()
            watcher.join()
        if problems:
            raise RuntimeError(f"concurrent reader failed: {problems[0]}")
        if target.read_bytes() not in (first, second) or any(path.exists() for path in paths):
            raise RuntimeError("publication did not leave exactly the final target")


if __name__ == "__main__":
    main()
