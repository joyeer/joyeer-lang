"""Verify that the local Debug installer can relocate and use bundled modules."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, encoding="utf-8",
                            errors="backslashreplace", timeout=60)
    if result.returncode:
        raise RuntimeError(f"{command!r} returned {result.returncode}:\n{result.stdout}{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    args = parser.parse_args()
    suffix = ".exe" if sys.platform == "win32" else ""
    with tempfile.TemporaryDirectory(prefix="joyeer-debug-install-") as work:
        root = Path(work)
        staged = root / "install"
        run([sys.executable, str(ROOT / "scripts" / "install-debug.py"),
             "--build-dir", str(args.build_dir.resolve()), "--install-dir", str(staged),
             "--skip-path-update", "--skip-skill-install"], root)
        relocated = staged.rename(root / "relocated compiler")
        compiler = relocated / f"joyeer{suffix}"
        source = root / "system.joyeer"
        source.write_text(
            'import System\nfunc main() {\n'
            '    match System.joinPath(base: ".", path: "test") {\n'
            '        .Ok(_) => print(value: "bundled System resolved"),\n'
            '        .Err(_) => fatalError(message: "join failed"),\n'
            '    }\n}\n',
            encoding="utf-8",
        )
        executable = root / f"debug-toolchain-probe{suffix}"
        run([str(compiler), "-g0", "-o", str(executable), "--", str(source)], root)
        actual = run([str(executable)], root)
        if actual != "bundled System resolved\n":
            raise RuntimeError(f"relocated Debug compiler produced unexpected output: {actual!r}")


if __name__ == "__main__":
    main()
