from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Any, Dict, Mapping, Tuple
from urllib.parse import urlparse


LOCK_SCHEMA_VERSION = 1
BUILD_SCHEMA_VERSION = 1


class LockError(ValueError):
    """Raised when the checked-in toolchain lock is malformed or unsupported."""


@dataclass(frozen=True)
class SourceArchive:
    filename: str
    url: str
    size: int
    sha256: str
    provenance_url: str


@dataclass(frozen=True)
class LlvmLock:
    schema_version: int
    build_schema_version: int
    version: str
    tag: str
    commit: str
    source: SourceArchive
    projects: Tuple[str, ...]
    targets: Mapping[str, Tuple[str, ...]]
    cmake_options: Mapping[str, str]
    patch_set: str
    fingerprint: str

    def targets_for(self, host: str) -> Tuple[str, ...]:
        try:
            return self.targets[host]
        except KeyError as error:
            raise LockError(f"LLVM lock has no target configuration for '{host}'") from error


def _mapping(value: Any, name: str) -> Dict[str, Any]:
    if not isinstance(value, dict):
        raise LockError(f"{name} must be an object")
    return value


def _exact_keys(value: Mapping[str, Any], expected: set[str], name: str) -> None:
    actual = set(value)
    if actual == expected:
        return
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    details = []
    if missing:
        details.append("missing " + ", ".join(missing))
    if extra:
        details.append("unexpected " + ", ".join(extra))
    raise LockError(f"{name} has invalid fields: {'; '.join(details)}")


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise LockError(f"{name} must be a nonempty string")
    return value


def _integer(value: Any, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise LockError(f"{name} must be an integer")
    return value


def _https_url(value: Any, name: str) -> str:
    text = _string(value, name)
    parsed = urlparse(text)
    if parsed.scheme != "https" or not parsed.netloc:
        raise LockError(f"{name} must be an HTTPS URL")
    return text


def _hex_digest(value: Any, length: int, name: str) -> str:
    text = _string(value, name)
    if not re.fullmatch(f"[0-9a-f]{{{length}}}", text):
        raise LockError(f"{name} must contain {length} lowercase hexadecimal characters")
    return text


def _string_tuple(value: Any, name: str) -> Tuple[str, ...]:
    if not isinstance(value, list) or not value:
        raise LockError(f"{name} must be a nonempty array")
    result = tuple(_string(item, f"{name}[]") for item in value)
    if len(set(result)) != len(result):
        raise LockError(f"{name} must not contain duplicates")
    return result


def load_llvm_lock(path: Path) -> LlvmLock:
    try:
        raw_text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise LockError(f"cannot read LLVM lock '{path}': {error}") from error
    try:
        raw_value = json.loads(raw_text)
    except json.JSONDecodeError as error:
        raise LockError(f"cannot parse LLVM lock '{path}': {error}") from error

    root = _mapping(raw_value, "LLVM lock")
    _exact_keys(
        root,
        {"schema_version", "build_schema_version", "llvm", "patch_set"},
        "LLVM lock",
    )
    schema_version = _integer(root["schema_version"], "schema_version")
    if schema_version != LOCK_SCHEMA_VERSION:
        raise LockError(
            f"unsupported LLVM lock schema {schema_version}; expected {LOCK_SCHEMA_VERSION}"
        )
    build_schema_version = _integer(
        root["build_schema_version"], "build_schema_version"
    )
    if build_schema_version != BUILD_SCHEMA_VERSION:
        raise LockError(
            "unsupported LLVM build schema "
            f"{build_schema_version}; expected {BUILD_SCHEMA_VERSION}"
        )

    llvm = _mapping(root["llvm"], "llvm")
    _exact_keys(
        llvm,
        {"version", "tag", "commit", "source", "projects", "targets", "cmake_options"},
        "llvm",
    )
    source_value = _mapping(llvm["source"], "llvm.source")
    _exact_keys(
        source_value,
        {"filename", "url", "size", "sha256", "provenance_url"},
        "llvm.source",
    )
    source_size = _integer(source_value["size"], "llvm.source.size")
    if source_size <= 0:
        raise LockError("llvm.source.size must be positive")
    source = SourceArchive(
        filename=_string(source_value["filename"], "llvm.source.filename"),
        url=_https_url(source_value["url"], "llvm.source.url"),
        size=source_size,
        sha256=_hex_digest(source_value["sha256"], 64, "llvm.source.sha256"),
        provenance_url=_https_url(
            source_value["provenance_url"], "llvm.source.provenance_url"
        ),
    )

    target_values = _mapping(llvm["targets"], "llvm.targets")
    if not target_values:
        raise LockError("llvm.targets must not be empty")
    targets = {
        _string(host, "llvm.targets key"): _string_tuple(
            values, f"llvm.targets.{host}"
        )
        for host, values in target_values.items()
    }

    option_values = _mapping(llvm["cmake_options"], "llvm.cmake_options")
    cmake_options = {
        _string(name, "llvm.cmake_options key"): _string(
            value, f"llvm.cmake_options.{name}"
        )
        for name, value in option_values.items()
    }
    canonical = json.dumps(root, sort_keys=True, separators=(",", ":")).encode("utf-8")

    return LlvmLock(
        schema_version=schema_version,
        build_schema_version=build_schema_version,
        version=_string(llvm["version"], "llvm.version"),
        tag=_string(llvm["tag"], "llvm.tag"),
        commit=_hex_digest(llvm["commit"], 40, "llvm.commit"),
        source=source,
        projects=_string_tuple(llvm["projects"], "llvm.projects"),
        targets=targets,
        cmake_options=cmake_options,
        patch_set=_string(root["patch_set"], "patch_set"),
        fingerprint=hashlib.sha256(canonical).hexdigest(),
    )