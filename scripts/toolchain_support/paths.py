from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RepositoryPaths:
    root: Path
    deps: Path
    downloads: Path
    sources: Path
    builds: Path
    installs: Path
    llvm_lock: Path

    @classmethod
    def discover(cls) -> "RepositoryPaths":
        root = Path(__file__).resolve().parents[2]
        deps = root / ".deps"
        return cls(
            root=root,
            deps=deps,
            downloads=deps / "downloads",
            sources=deps / "llvm-project",
            builds=deps / "llvm-build",
            installs=deps / "llvm-install",
            llvm_lock=root / "third_party" / "llvm.lock.json",
        )