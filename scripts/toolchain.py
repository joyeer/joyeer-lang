#!/usr/bin/env python3

from pathlib import Path
import sys

from toolchain_support.pixi import activate_host_compiler, relaunch_in_pixi
from toolchain_support.process import ToolchainError


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    try:
        if any(argument in ("-h", "--help") for argument in sys.argv[1:]):
            from toolchain_support.cli import toolchain_main

            return toolchain_main()
        relaunched = relaunch_in_pixi(root, Path(__file__).resolve(), sys.argv[1:])
        if relaunched is not None:
            return relaunched
        if any(argument in ("build", "test") for argument in sys.argv[1:]):
            activate_host_compiler()
        from toolchain_support.cli import toolchain_main

        return toolchain_main()
    except ToolchainError as error:
        print(f"toolchain error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())