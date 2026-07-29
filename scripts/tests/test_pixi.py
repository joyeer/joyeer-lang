from __future__ import annotations

import hashlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.toolchain_support.pixi import (
    PIXI_ASSETS,
    _activate_msvc,
    activate_host_compiler,
    ensure_pixi,
    relaunch_in_pixi,
)
from scripts.toolchain_support.targets import Host


class PixiBootstrapTest(unittest.TestCase):
    def test_has_pinned_assets_for_supported_hosts(self) -> None:
        self.assertEqual(
            set(PIXI_ASSETS),
            {
                "windows-x86_64",
                "macos-arm64",
                "macos-x86_64",
                "linux-arm64",
                "linux-x86_64",
            },
        )
        for _asset, digest in PIXI_ASSETS.values():
            self.assertEqual(len(digest), 64)
            int(digest, 16)

    def test_downloads_and_verifies_pixi(self) -> None:
        payload = b"pixi-test"
        digest = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as directory, patch(
            "scripts.toolchain_support.pixi._installed_pixi_candidates",
            return_value=(),
        ), patch(
            "scripts.toolchain_support.pixi.detect_host",
            return_value=Host("linux", "x86_64"),
        ), patch.dict(
            PIXI_ASSETS,
            {"linux-x86_64": ("pixi-test", digest)},
        ), patch(
            "scripts.toolchain_support.pixi.urllib.request.urlopen",
            return_value=io.BytesIO(payload),
        ):
            result = ensure_pixi(Path(directory), offline=False)

            self.assertEqual(result.read_bytes(), payload)

    def test_relaunch_guard_stops_recursion(self) -> None:
        with patch.dict(os.environ, {"JOYEER_PIXI_RELAUNCHED": "1"}), patch(
            "scripts.toolchain_support.pixi.ensure_pixi"
        ) as ensure:
            result = relaunch_in_pixi(Path.cwd(), Path("bootstrap.py"), ())

            self.assertIsNone(result)
            ensure.assert_not_called()

    def test_selects_platform_host_compilers(self) -> None:
        with patch(
            "scripts.toolchain_support.pixi.detect_host",
            return_value=Host("linux", "x86_64"),
        ):
            activate_host_compiler()
            self.assertEqual(os.environ["CC"], "gcc")
            self.assertEqual(os.environ["CXX"], "g++")

        with patch(
            "scripts.toolchain_support.pixi.detect_host",
            return_value=Host("macos", "arm64"),
        ):
            activate_host_compiler()
            self.assertEqual(os.environ["CC"], "clang")
            self.assertEqual(os.environ["CXX"], "clang++")

    def test_activates_complete_msvc_environment(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vswhere = (
                root
                / "Microsoft Visual Studio"
                / "Installer"
                / "vswhere.exe"
            )
            developer_command = root / "VS" / "Common7" / "Tools" / "VsDevCmd.bat"
            include = root / "include"
            library = root / "lib"
            for path in (vswhere, developer_command, include / "string"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            for name in ("libcmt.lib", "kernel32.lib"):
                (library / name).parent.mkdir(parents=True, exist_ok=True)
                (library / name).touch()

            query = subprocess.CompletedProcess([], 0, stdout=str(root / "VS") + "\n")
            activation = subprocess.CompletedProcess(
                [],
                0,
                stdout=f"INCLUDE={include}\nLIB={library}\nPATH={root}\n",
            )
            with patch.dict(os.environ, {"ProgramFiles(x86)": str(root)}), patch(
                "scripts.toolchain_support.pixi.subprocess.run",
                side_effect=(query, activation),
            ) as run_process, patch(
                "scripts.toolchain_support.pixi.shutil.which",
                return_value=str(root / "cl.exe"),
            ):
                _activate_msvc()
                self.assertEqual(os.environ["CC"], "cl.exe")
                self.assertEqual(os.environ["CXX"], "cl.exe")
                self.assertEqual(
                    run_process.call_args_list[1].args[0],
                    [
                        "cmd.exe",
                        "/d",
                        "/s",
                        "/c",
                        "call",
                        str(developer_command),
                        "-no_logo",
                        "-arch=x64",
                        "-host_arch=x64",
                        ">nul",
                        "&&",
                        "set",
                    ],
                )


if __name__ == "__main__":
    unittest.main()
