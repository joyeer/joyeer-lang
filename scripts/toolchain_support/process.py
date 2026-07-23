from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import shlex
import shutil
import subprocess
from typing import Mapping, Optional, Sequence


class ToolchainError(RuntimeError):
    """Raised for an expected, user-actionable automation failure."""


@dataclass(frozen=True)
class ExecutionContext:
    dry_run: bool = False
    verbose: bool = False

    def log(self, message: str) -> None:
        if self.verbose or self.dry_run:
            print(message)


def find_program(name: str) -> Optional[str]:
    return shutil.which(name)


def require_program(name: str) -> str:
    path = find_program(name)
    if path is None:
        raise ToolchainError(
            f"required program '{name}' was not found in PATH; install it and retry"
        )
    return path


def format_command(command: Sequence[str]) -> str:
    return shlex.join(str(argument) for argument in command)


def run(
    command: Sequence[str],
    *,
    context: ExecutionContext,
    cwd: Optional[Path] = None,
    environment: Optional[Mapping[str, str]] = None,
) -> None:
    normalized = [str(argument) for argument in command]
    context.log(f"+ {format_command(normalized)}")
    if context.dry_run:
        return
    try:
        subprocess.run(
            normalized,
            check=True,
            cwd=str(cwd) if cwd is not None else None,
            env=dict(environment) if environment is not None else None,
        )
    except OSError as error:
        raise ToolchainError(f"cannot launch '{normalized[0]}': {error}") from error
    except subprocess.CalledProcessError as error:
        raise ToolchainError(
            f"command failed with exit code {error.returncode}: {format_command(normalized)}"
        ) from error