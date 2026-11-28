#!/usr/bin/env python3
"""List Joyeer spec sections with no explicit test citations."""

import argparse
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SECTION = re.compile(r"^#{2,6} \u00a7?((?:[0-9]+|[A-Z])(?:\.[0-9]+)*)\b")
CITATION = re.compile(r"^\s*//\s*spec:\s+(\S+\.md)\s+\u00a7([0-9A-Z]+(?:\.[0-9]+)*)\s*$")


def main() -> int:
    sys.stdout.reconfigure(errors="backslashreplace")
    sys.stderr.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    root = parser.parse_args().root
    sections = {}
    for path in sorted((root / "specs").glob("*.md")):
        for line in path.read_text(encoding="utf-8").splitlines():
            match = SECTION.match(line)
            if match:
                sections[path.name, match[1]] = line.lstrip("# ")
    if not sections:
        parser.error(f"no numbered spec sections found in {root / 'specs'}")

    covered = set()
    invalid = False
    for path in sorted((root / "tests").rglob("*")):
        if path.suffix not in {".joyeer", ".c", ".cpp"} or not path.is_file():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="surrogateescape").splitlines(), 1):
            if not line.lstrip().startswith("// spec:"):
                continue
            match = CITATION.fullmatch(line)
            if not match or match.groups() not in sections:
                print(f"{path.relative_to(root)}:{number}: invalid spec citation: {line.strip()}", file=sys.stderr)
                invalid = True
            else:
                covered.add(match.groups())
    if invalid:
        return 1
    for key, title in sections.items():
        if key not in covered:
            print(f"{key[0]} \u00a7{key[1]}: {title}")
    print(f"{len(covered)} of {len(sections)} sections have explicit test citations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
