import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import stage_release


class StageReleaseTests(unittest.TestCase):
    components = {
        "JoyeerRuntime": ("joyeer", "libjoyeer-backend.so", "lib/libJoyeerRuntime.a", "licenses/LICENSE"),
        "StandardLibrary": ("lib/index.toml", "lib/System/project.toml", "lib/System/main.joyeer",
                            "lib/Foundation/project.toml", "lib/Foundation/main.joyeer"),
        "PRODUCT": ("fern",),
    }

    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name).resolve()
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.notices = self.root / "notices"
        self.destination = self.root / "stage"
        self.build.mkdir()
        self.notices.mkdir()
        for module in ("System", "Foundation"):
            directory = self.source / "lib" / module
            directory.mkdir(parents=True)
            (directory / "main.joyeer").write_bytes(b"func sample() {}\n")
        self.write_cache()
        for name in ("LLVM", "LLD", "Clang", "Platform"):
            (self.notices / f"{name}.txt").write_bytes(f"Test notice: {name}\n".encode())

    def write_cache(self, config="Release", hooks="OFF", extra=""):
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_PROJECT_NAME:STATIC=joyeer\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source.as_posix()}\n"
            f"CMAKE_BUILD_TYPE:STRING={config}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n"
            f"JOYEER_BACKEND_TEST_HOOKS:BOOL={hooks}\n"
            "JOYEER_BUILD_COMPRESSION:BOOL=OFF\n"
            "JOYEER_BUILD_FERN:BOOL=ON\n"
            + extra,
            encoding="utf-8",
        )
        (self.build / "cmake_install.cmake").touch()

    def stage(self):
        return stage_release.stage_release(
            self.build, self.destination, self.notices, self.source, "Linux"
        )

    def test_requires_notices_before_creating_a_stage(self):
        (self.notices / "Clang.txt").unlink()
        with self.assertRaisesRegex(stage_release.InstallError, "Clang"):
            self.stage()
        self.assertFalse(self.destination.exists())

    def test_rejects_non_product_builds_before_installing(self):
        for config, hooks, extra in (
            ("Debug", "OFF", ""), ("Release", "ON", ""), ("Release", "2", ""),
            ("Debug", "OFF", "CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release\n"),
            ("Release", "OFF", "JOYEER_SANITIZE_RUNTIME:BOOL=ON\n"),
        ):
            with self.subTest(config=config, hooks=hooks, extra=extra):
                self.write_cache(config, hooks, extra)
                with self.assertRaisesRegex(stage_release.InstallError, "Release|hooks|BOOL"):
                    self.stage()
                self.assertFalse(self.destination.exists())

    def test_never_overwrites_an_existing_prefix(self):
        self.destination.mkdir()
        sentinel = self.destination / "keep"
        sentinel.write_bytes(b"untouched")
        with self.assertRaisesRegex(stage_release.InstallError, "already exists"):
            self.stage()
        self.assertEqual(list(self.destination.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"untouched")

    def test_stages_only_product_components_and_records_notice_bytes(self):
        def install(command, *, check, env):
            self.assertTrue(check)
            self.assertEqual(command[:5], ["cmake", "--install", str(self.build), "--config", "Release"])
            self.assertEqual(command[5], "--component")
            self.assertEqual(command[7:], ["--prefix", str(self.destination)])
            self.assertNotIn("DESTDIR", env)
            self.install_fixture(command[6])

        with patch.object(stage_release.shutil, "which", return_value="cmake"), \
                patch.object(stage_release.subprocess, "run", side_effect=install) as run, \
                patch.dict(os.environ, {"DESTDIR": str(self.root / "other-install")}):
            self.stage()
        self.assertEqual([call.args[0][6] for call in run.call_args_list], list(self.components))
        inventory = json.loads((self.destination / "release-inventory.json").read_text(encoding="utf-8"))
        notices = {f"licenses/{path.name}" for path in self.notices.iterdir()}
        expected = set().union(*self.components.values()) | notices
        self.assertEqual(set(inventory["files"]), expected)
        for name, metadata in inventory["files"].items():
            contents = (self.destination / name).read_bytes()
            self.assertEqual(metadata, {"sha256": hashlib.sha256(contents).hexdigest(), "size": len(contents)})
            if name in notices:
                self.assertEqual(contents, (self.notices / Path(name).name).read_bytes())

    def install_fixture(self, component, omit=""):
        for name in self.components[component]:
            if name == omit:
                continue
            path = self.destination / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())

    def test_refuses_inventory_if_install_omits_a_standard_library_source(self):
        missing = "lib/System/main.joyeer"
        with patch.object(stage_release.shutil, "which", return_value="cmake"), \
                patch.object(stage_release.subprocess, "run",
                             side_effect=lambda command, **_: self.install_fixture(command[6], missing)):
            with self.assertRaisesRegex(stage_release.InstallError, missing):
                self.stage()
        self.assertFalse((self.destination / "release-inventory.json").exists())

    def test_does_not_record_an_inventory_when_cmake_fails(self):
        error = subprocess.CalledProcessError(1, ["cmake"])
        with patch.object(stage_release.shutil, "which", return_value="cmake"), \
                patch.object(stage_release.subprocess, "run", side_effect=error):
            with self.assertRaisesRegex(stage_release.InstallError, "JoyeerRuntime"):
                self.stage()
        self.assertFalse((self.destination / "release-inventory.json").exists())


if __name__ == "__main__":
    unittest.main()
