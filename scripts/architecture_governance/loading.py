"""Load strict machine-readable architecture policy and current state."""

from __future__ import annotations

import tomllib
from collections.abc import Mapping
from datetime import date
from pathlib import Path
from typing import cast

from .model import (
    ArchitectureDebt,
    ArchitecturePolicy,
    ArchitectureState,
    ArchitectureWaiver,
    AreaInventory,
    AreaPolicy,
)


def load_policy(path: Path) -> ArchitecturePolicy:
    """Load the schema-versioned architecture policy."""

    data = _document(path, expected_version=2)
    _exact_keys(data, {"schema_version", "structure", "areas", "registries"}, str(path))
    structure = _mapping(data, "structure")
    registries = _mapping(data, "registries")
    _exact_keys(
        structure,
        {"soft_lines", "hard_lines", "source_extensions", "excluded_paths"},
        "structure policy",
    )
    _exact_keys(registries, {"inventory", "debt", "waivers"}, "architecture registries")
    return ArchitecturePolicy(
        soft_lines=_positive_int(structure, "soft_lines"),
        hard_lines=_positive_int(structure, "hard_lines"),
        source_extensions=frozenset(_strings(structure, "source_extensions")),
        excluded_paths=frozenset(_strings(structure, "excluded_paths", allow_empty=True)),
        areas=tuple(_parse_area(item) for item in _tables(data, "areas")),
        inventory_registry=Path(_string(registries, "inventory")),
        debt_registry=Path(_string(registries, "debt")),
        waiver_registry=Path(_string(registries, "waivers")),
    )


def load_state(root: Path, policy: ArchitecturePolicy) -> ArchitectureState:
    """Load all exact current architecture-state registries."""

    debt_data = _registry_document(root / policy.debt_registry, "debts")
    waiver_data = _registry_document(root / policy.waiver_registry, "waivers")
    inventory_data = _registry_document(root / policy.inventory_registry, "inventories")
    return ArchitectureState(
        inventories=tuple(_parse_inventory(item) for item in _tables(inventory_data, "inventories")),
        debts=tuple(_parse_debt(item) for item in _tables(debt_data, "debts")),
        waivers=tuple(_parse_waiver(item) for item in _tables(waiver_data, "waivers")),
    )


def _parse_inventory(data: Mapping[str, object]) -> AreaInventory:
    """Parse one complete owned-area snapshot."""

    _exact_keys(
        data,
        {"id", "area", "owner", "fingerprint", "file_count", "assessed_on"},
        "area inventory",
    )
    return AreaInventory(
        identifier=_string(data, "id"),
        area=_string(data, "area"),
        owner=_string(data, "owner"),
        fingerprint=_string(data, "fingerprint"),
        file_count=_positive_int(data, "file_count"),
        assessed_on=_date(data, "assessed_on"),
    )


def _parse_area(data: Mapping[str, object]) -> AreaPolicy:
    """Parse one owned-area policy."""

    _exact_keys(data, {"id", "owner", "status", "source_roots", "source_files"}, "owned area")
    status = _string(data, "status")
    if status not in {"active", "reserved"}:
        raise ValueError("owned area status must be active or reserved")
    return AreaPolicy(
        identifier=_string(data, "id"),
        owner=_string(data, "owner"),
        status=status,
        source_roots=tuple(Path(value) for value in _strings(data, "source_roots", allow_empty=True)),
        source_files=tuple(Path(value) for value in _strings(data, "source_files", allow_empty=True)),
    )


def _parse_debt(data: Mapping[str, object]) -> ArchitectureDebt:
    """Parse one assessed mixed-ownership record."""

    _exact_keys(
        data,
        {
            "id",
            "area",
            "owner",
            "paths",
            "fingerprint",
            "issue",
            "review_by",
            "responsibilities",
            "next_extraction",
        },
        "architecture debt",
    )
    responsibilities = _strings(data, "responsibilities")
    if len(responsibilities) < 2:
        raise ValueError("architecture debt must name at least two responsibilities")
    return ArchitectureDebt(
        identifier=_string(data, "id"),
        area=_string(data, "area"),
        owner=_string(data, "owner"),
        paths=_strings(data, "paths"),
        fingerprint=_string(data, "fingerprint"),
        issue=_string(data, "issue"),
        review_by=_date(data, "review_by"),
        responsibilities=responsibilities,
        next_extraction=_string(data, "next_extraction"),
    )


def _parse_waiver(data: Mapping[str, object]) -> ArchitectureWaiver:
    """Parse one bounded structural or remediation waiver."""

    kind = _string(data, "kind")
    if kind not in {"structural", "remediation"}:
        raise ValueError("architecture waiver kind must be structural or remediation")
    fields = {
        "id",
        "area",
        "owner",
        "rule",
        "path",
        "kind",
        "justification",
        "issue",
        "review_by",
        "fingerprint",
        "max_lines",
    }
    if kind == "remediation":
        fields |= {"next_limit", "debt"}
    _exact_keys(data, fields, "architecture waiver")
    return ArchitectureWaiver(
        identifier=_string(data, "id"),
        area=_string(data, "area"),
        owner=_string(data, "owner"),
        rule=_string(data, "rule"),
        path=_string(data, "path"),
        kind=kind,
        justification=_string(data, "justification"),
        issue=_string(data, "issue"),
        review_by=_date(data, "review_by"),
        fingerprint=_string(data, "fingerprint"),
        max_lines=_positive_int(data, "max_lines"),
        next_limit=_positive_int(data, "next_limit") if kind == "remediation" else None,
        debt=_string(data, "debt") if kind == "remediation" else None,
    )


def _document(path: Path, *, expected_version: int) -> Mapping[str, object]:
    """Read one TOML document and validate its schema envelope."""

    data = cast(Mapping[str, object], tomllib.loads(path.read_text(encoding="utf-8")))
    if data.get("schema_version") != expected_version:
        raise ValueError(f"{path} must declare schema_version = {expected_version}")
    return data


def _registry_document(path: Path, collection: str) -> Mapping[str, object]:
    """Read one strict registry envelope containing current state only."""

    data = _document(path, expected_version=1)
    _exact_keys(data, {"schema_version", collection}, str(path))
    return data


def _exact_keys(data: Mapping[str, object], expected: set[str], label: str) -> None:
    """Reject missing fields and history-shaped surplus fields."""

    missing = expected - set(data)
    unknown = set(data) - expected
    if missing or unknown:
        raise ValueError(
            f"{label} fields differ: missing={sorted(missing)}, unsupported={sorted(unknown)}"
        )


def _mapping(data: Mapping[str, object], key: str) -> Mapping[str, object]:
    """Return one required TOML table."""

    value = data.get(key)
    if not isinstance(value, dict):
        raise TypeError(f"{key} must be a table")
    return cast(Mapping[str, object], value)


def _tables(data: Mapping[str, object], key: str) -> tuple[Mapping[str, object], ...]:
    """Return one array of TOML tables."""

    value = data.get(key, [])
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        raise TypeError(f"{key} must be an array of tables")
    return tuple(cast(list[Mapping[str, object]], value))


def _string(data: Mapping[str, object], key: str) -> str:
    """Return one required nonempty string."""

    value = data.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{key} must be a nonempty string")
    return value


def _strings(
    data: Mapping[str, object], key: str, *, allow_empty: bool = False
) -> tuple[str, ...]:
    """Return one array of unique nonempty strings."""

    value = data.get(key)
    if not isinstance(value, list) or (not value and not allow_empty):
        raise ValueError(f"{key} must be a string array")
    if not all(isinstance(item, str) and item.strip() for item in value):
        raise ValueError(f"{key} must contain nonempty strings")
    strings = tuple(cast(list[str], value))
    if len(strings) != len(set(strings)):
        raise ValueError(f"{key} must not contain duplicates")
    return strings


def _positive_int(data: Mapping[str, object], key: str) -> int:
    """Return one required positive integer."""

    value = data.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{key} must be a positive integer")
    return value


def _date(data: Mapping[str, object], key: str) -> date:
    """Return one required TOML date."""

    value = data.get(key)
    if not isinstance(value, date):
        raise TypeError(f"{key} must be an ISO date")
    return value
