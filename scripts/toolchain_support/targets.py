from __future__ import annotations

from dataclasses import dataclass
import platform

from .process import ToolchainError


@dataclass(frozen=True)
class Host:
    operating_system: str
    architecture: str

    @property
    def key(self) -> str:
        return f"{self.operating_system}-{self.architecture}"


def normalize_host(system: str, machine: str) -> Host:
    systems = {
        "darwin": "macos",
        "linux": "linux",
        "windows": "windows",
    }
    architectures = {
        "aarch64": "arm64",
        "arm64": "arm64",
        "amd64": "x86_64",
        "x86_64": "x86_64",
    }
    operating_system = systems.get(system.lower())
    architecture = architectures.get(machine.lower())
    if operating_system is None or architecture is None:
        raise ToolchainError(f"unsupported host platform '{system}-{machine}'")
    return Host(operating_system, architecture)


def detect_host() -> Host:
    return normalize_host(platform.system(), platform.machine())