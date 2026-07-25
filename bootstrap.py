#!/usr/bin/env python3

from pathlib import Path
import sys

from scripts.toolchain_support.pixi import activate_host_compiler, relaunch_in_pixi
from scripts.toolchain_support.process import ToolchainError


def main() -> int:
    root = Path(__file__).resolve().parent
    try:
        if any(argument in ("-h", "--help") for argument in sys.argv[1:]):
            from scripts.toolchain_support.cli import bootstrap_main

            return bootstrap_main()
        relaunched = relaunch_in_pixi(root, Path(__file__).resolve(), sys.argv[1:])
        if relaunched is not None:
            return relaunched
        activate_host_compiler()
        from scripts.toolchain_support.cli import bootstrap_main

        return bootstrap_main()
    except ToolchainError as error:
        print(f"bootstrap error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())