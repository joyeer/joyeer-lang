"""Generate bounded ownership cases and run each through the native leak check."""

from pathlib import Path
import subprocess
import sys
import tempfile


CASES = ((-1, -1), (-1, 0), (-1, 2), (0, -1), (2, -1),
         (4, 1), (3, 3), (4, 4), (5, -1))


def source(parity):
    calls = "".join(f"    report(reject: {reject}, stop: {stop})\n" for reject, stop in CASES)
    return """
func advance(value: consuming String, reject: Bool): Result<String, String> {
    if reject { return .Err(value + "?") }
    return .Ok(value + "+")
}

func exercise(reject: Int, stop: Int): Result<Int, String> {
    var sum = 0
    var i = 0
    while i < 6 {
        let draft = "owned:" + byteToString(value: (65 + i) as UInt8)
        var chosen: String
        if i % 2 == PARITY {
            chosen = consume draft
        } else {
            chosen = draft + "!"
        }
        let value = advance(value: consume chosen, reject: i == reject)?
        sum = sum + value.count
        if i == stop { return .Ok(sum) }
        i = i + 1
    }
    return .Ok(sum)
}

func report(reject: Int, stop: Int) {
    match exercise(reject: reject, stop: stop) {
        .Ok(checksum) => print(value: checksum),
        .Err(reason) => print(value: reason),
    }
}
""".replace("PARITY", str(parity)) + "func main() {\n" + calls + "}\n"


def expected(parity, reject, stop):
    total = 0
    for i in range(6):
        value = "owned:" + chr(65 + i) + ("" if i % 2 == parity else "!")
        if i == reject:
            return value + "?"
        total += len(value) + 1
        if i == stop:
            break
    return str(total)


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, encoding="utf-8",
                            errors="backslashreplace", timeout=30)
    if result.returncode or result.stderr:
        raise RuntimeError(f"{command!r} returned {result.returncode}:\n{result.stdout}{result.stderr}")
    return result.stdout


def main():
    compiler = Path(sys.argv[1]).resolve(strict=True)
    suffix = ".exe" if sys.platform == "win32" else ""
    with tempfile.TemporaryDirectory(prefix="joyeer-ownership-") as work:
        root = Path(work)
        for parity in (0, 1):
            program = root / f"ownership-{parity}.joyeer"
            program.write_text(source(parity), encoding="utf-8")
            wanted = "\n".join(expected(parity, reject, stop) for reject, stop in CASES) + "\n"
            for level in ("-O0", "-O2"):
                executable = root / f"ownership-{parity}{level}{suffix}"
                run([str(compiler), level, "-g0", "-o", str(executable), "--", str(program)], root)
                actual = run([str(executable)], root)
                if actual != wanted:
                    raise RuntimeError(f"{executable.name}: expected {wanted!r}, got {actual!r}")


if __name__ == "__main__":
    main()
