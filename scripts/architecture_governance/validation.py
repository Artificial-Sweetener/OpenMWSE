"""Validate structural limits and current architecture state."""

from __future__ import annotations

from collections import Counter
from datetime import UTC, date, datetime
from pathlib import Path

from .loading import load_policy, load_state
from .metrics import governed_source_areas, production_line_count, source_fingerprint
from .model import ArchitectureDebt, ArchitecturePolicy, ArchitectureState, ArchitectureWaiver, Diagnostic

_OWNERSHIP_RESPONSE = (
    "Assess the file's authoritative owner, responsibilities, dependency direction, public boundary, "
    "behavior contract, and change cadence. Record mixed ownership as fingerprinted debt with a bounded "
    "remediation waiver, or document a file-specific cohesion case as a structural waiver."
)
_MINIMUM_STRUCTURAL_JUSTIFICATION_LENGTH = 160


def validate_repository(
    root: Path, *, policy_path: Path | None = None, today: date | None = None
) -> list[Diagnostic]:
    """Return every architecture diagnostic for the current repository."""

    try:
        policy = load_policy(policy_path or root / "ARCHITECTURE_POLICY.toml")
        state = load_state(root, policy)
        governed = governed_source_areas(root, policy)
    except (OSError, TypeError, ValueError) as error:
        return [Diagnostic("STATE001", "ARCHITECTURE_POLICY.toml", str(error))]
    current_date = today or datetime.now(UTC).date()
    diagnostics = [
        *_validate_policy(root, policy, governed),
        *_validate_state(root, policy, state, governed, current_date),
        *_validate_structure(root, policy, state, governed, current_date),
    ]
    return sorted(
        diagnostics,
        key=lambda item: (item.path, item.rule, item.severity, item.message),
    )


def _validate_policy(
    root: Path, policy: ArchitecturePolicy, governed: dict[Path, str]
) -> list[Diagnostic]:
    """Validate coherent limits and exact configured ownership scope."""

    diagnostics: list[Diagnostic] = []
    if policy.soft_lines >= policy.hard_lines:
        diagnostics.append(Diagnostic("POLICY001", "ARCHITECTURE_POLICY.toml", "soft_lines must be lower than hard_lines"))
    area_ids = [area.identifier for area in policy.areas]
    for identifier, count in Counter(area_ids).items():
        if count > 1:
            diagnostics.append(Diagnostic("POLICY002", "ARCHITECTURE_POLICY.toml", f"owned area {identifier} is duplicated"))
    for area in policy.areas:
        if not area.source_roots and not area.source_files:
            diagnostics.append(
                Diagnostic(
                    "POLICY009",
                    "ARCHITECTURE_POLICY.toml",
                    f"owned area {area.identifier} defines no source boundary",
                )
            )
        if area.status == "reserved":
            for source_path in (*area.source_roots, *area.source_files):
                if (root / source_path).exists():
                    diagnostics.append(
                        Diagnostic(
                            "POLICY008",
                            "ARCHITECTURE_POLICY.toml",
                            f"reserved area {area.identifier} contains {source_path.as_posix()}; activate its governance before adding source",
                        )
                    )
            continue
        for source_root in area.source_roots:
            if not (root / source_root).is_dir():
                diagnostics.append(Diagnostic("POLICY003", "ARCHITECTURE_POLICY.toml", f"source root {source_root.as_posix()} does not exist"))
        for source_file in area.source_files:
            path = root / source_file
            if not path.is_file():
                diagnostics.append(Diagnostic("POLICY004", "ARCHITECTURE_POLICY.toml", f"source file {source_file.as_posix()} does not exist"))
            elif path.suffix not in policy.source_extensions:
                diagnostics.append(Diagnostic("POLICY005", "ARCHITECTURE_POLICY.toml", f"source file {source_file.as_posix()} has an ungoverned extension"))
    for excluded_path in sorted(policy.excluded_paths):
        if not (root / excluded_path).is_file():
            diagnostics.append(Diagnostic("POLICY006", "ARCHITECTURE_POLICY.toml", f"excluded source {excluded_path} does not exist"))
    if not governed:
        diagnostics.append(Diagnostic("POLICY007", "ARCHITECTURE_POLICY.toml", "owned areas contain no governed source"))
    return diagnostics


def _validate_state(
    root: Path,
    policy: ArchitecturePolicy,
    state: ArchitectureState,
    governed: dict[Path, str],
    today: date,
) -> list[Diagnostic]:
    """Validate registry uniqueness, ownership, fingerprints, dates, and links."""

    diagnostics = _validate_unique_state(state)
    governed_by_name = {path.relative_to(root).as_posix(): area for path, area in governed.items()}
    area_owners = {area.identifier: area.owner for area in policy.areas}
    active_areas = {area.identifier for area in policy.areas if area.status == "active"}
    for area in sorted(active_areas):
        count = sum(inventory.area == area for inventory in state.inventories)
        if count != 1:
            diagnostics.append(
                Diagnostic(
                    "INVENTORY001",
                    policy.inventory_registry.as_posix(),
                    f"active area {area} must have exactly one inventory snapshot; found {count}",
                )
            )
    for inventory in state.inventories:
        registry = policy.inventory_registry.as_posix()
        if inventory.area not in active_areas or inventory.owner != area_owners.get(inventory.area):
            diagnostics.append(
                Diagnostic("INVENTORY002", registry, f"inventory {inventory.identifier} has an invalid area or owner")
            )
            continue
        paths = sorted(path for path, area in governed_by_name.items() if area == inventory.area)
        if inventory.file_count != len(paths):
            diagnostics.append(
                Diagnostic(
                    "INVENTORY003",
                    registry,
                    f"inventory {inventory.identifier} records {inventory.file_count} files "
                    f"but area {inventory.area} contains {len(paths)}",
                )
            )
        if source_fingerprint(root, paths) != inventory.fingerprint:
            diagnostics.append(
                Diagnostic(
                    "INVENTORY004",
                    registry,
                    f"inventory {inventory.identifier} no longer matches all source in area {inventory.area}; reassess the area",
                )
            )
        if inventory.assessed_on > today:
            diagnostics.append(
                Diagnostic(
                    "INVENTORY005",
                    registry,
                    f"inventory {inventory.identifier} has future assessment date {inventory.assessed_on.isoformat()}",
                )
            )
    debt_by_id = {debt.identifier: debt for debt in state.debts}
    for debt in state.debts:
        registry = policy.debt_registry.as_posix()
        if debt.area not in area_owners or debt.owner != area_owners.get(debt.area):
            diagnostics.append(Diagnostic("DEBT001", registry, f"debt {debt.identifier} has an invalid area or owner"))
        elif any(governed_by_name.get(path) != debt.area for path in debt.paths):
            diagnostics.append(Diagnostic("DEBT002", registry, f"debt {debt.identifier} must reference exact source paths in area {debt.area}"))
        elif source_fingerprint(root, debt.paths) != debt.fingerprint:
            diagnostics.append(Diagnostic("DEBT003", registry, f"debt {debt.identifier} no longer matches assessed source; reassess it or delete resolved debt"))
        if debt.review_by < today:
            diagnostics.append(Diagnostic("DEBT004", registry, f"debt {debt.identifier} review deadline expired on {debt.review_by.isoformat()}"))
        linked = tuple(waiver for waiver in state.waivers if waiver.kind == "remediation" and waiver.debt == debt.identifier)
        if len(linked) != 1:
            diagnostics.append(Diagnostic("DEBT005", registry, f"debt {debt.identifier} must have exactly one linked remediation waiver; found {len(linked)}"))
    for waiver in state.waivers:
        diagnostics.extend(_validate_waiver(root, policy, waiver, debt_by_id, governed_by_name, area_owners, today))
    return diagnostics


def _validate_unique_state(state: ArchitectureState) -> list[Diagnostic]:
    """Reject duplicate paths and record identifiers across current state."""

    diagnostics: list[Diagnostic] = []
    identifiers = [
        *(inventory.identifier for inventory in state.inventories),
        *(debt.identifier for debt in state.debts),
        *(waiver.identifier for waiver in state.waivers),
    ]
    for identifier, count in Counter(identifiers).items():
        if count > 1:
            diagnostics.append(Diagnostic("STATE002", "ARCHITECTURE_POLICY.toml", f"architecture record id {identifier} is not unique"))
    for path, count in Counter(waiver.path for waiver in state.waivers).items():
        if count > 1:
            diagnostics.append(Diagnostic("STATE003", "ARCHITECTURE_WAIVERS.toml", f"source path {path} has multiple structural dispositions"))
    for path, count in Counter(path for debt in state.debts for path in debt.paths).items():
        if count > 1:
            diagnostics.append(Diagnostic("STATE004", "ARCHITECTURE_DEBT.toml", f"source path {path} appears in multiple debt records"))
    rationales = [waiver.justification for waiver in state.waivers if waiver.kind == "structural"]
    for rationale, count in Counter(rationales).items():
        if count > 1:
            diagnostics.append(Diagnostic("WAIVER011", "ARCHITECTURE_WAIVERS.toml", "structural waiver rationales must be unique and file-specific"))
    return diagnostics


def _validate_waiver(
    root: Path,
    policy: ArchitecturePolicy,
    waiver: ArchitectureWaiver,
    debts: dict[str, ArchitectureDebt],
    governed: dict[str, str],
    area_owners: dict[str, str],
    today: date,
) -> list[Diagnostic]:
    """Validate one exact, bounded, current architecture waiver."""

    registry = policy.waiver_registry.as_posix()
    diagnostics: list[Diagnostic] = []
    if waiver.rule != "STRUCT003":
        diagnostics.append(Diagnostic("WAIVER001", registry, f"waiver {waiver.identifier} uses an unsupported rule"))
    if governed.get(waiver.path) != waiver.area or area_owners.get(waiver.area) != waiver.owner:
        diagnostics.append(Diagnostic("WAIVER002", registry, f"waiver {waiver.identifier} has an invalid path, area, or owner"))
        return diagnostics
    lines = production_line_count(root / waiver.path)
    if source_fingerprint(root, (waiver.path,)) != waiver.fingerprint:
        diagnostics.append(
            Diagnostic(
                "WAIVER012",
                registry,
                f"waiver {waiver.identifier} no longer matches assessed source; reassess it",
            )
        )
    if waiver.review_by < today:
        diagnostics.append(Diagnostic("WAIVER003", registry, f"waiver {waiver.identifier} expired on {waiver.review_by.isoformat()}"))
    if lines > waiver.max_lines:
        diagnostics.append(Diagnostic("WAIVER004", registry, f"waiver {waiver.identifier} caps {waiver.path} at {waiver.max_lines} lines but current source has {lines}"))
    if lines <= policy.hard_lines:
        diagnostics.append(Diagnostic("WAIVER005", registry, f"waiver {waiver.identifier} matches no current hard-size finding; delete it"))
    if lines != waiver.max_lines:
        diagnostics.append(Diagnostic("WAIVER008", registry, f"waiver {waiver.identifier} must cap the exact current size of {waiver.path} at {lines}"))
    if waiver.kind == "remediation":
        debt = debts.get(waiver.debt or "")
        if debt is None or waiver.path not in debt.paths or debt.area != waiver.area:
            diagnostics.append(Diagnostic("WAIVER006", registry, f"waiver {waiver.identifier} must link assessed debt for the same path and area"))
        if waiver.next_limit is None or waiver.next_limit >= waiver.max_lines:
            diagnostics.append(Diagnostic("WAIVER007", registry, f"waiver {waiver.identifier} next_limit must be lower than max_lines"))
    else:
        if any(waiver.path in debt.paths for debt in debts.values()):
            diagnostics.append(Diagnostic("WAIVER009", registry, f"structural waiver {waiver.identifier} cannot cover mixed-responsibility debt"))
        if len(waiver.justification) < _MINIMUM_STRUCTURAL_JUSTIFICATION_LENGTH:
            diagnostics.append(Diagnostic("WAIVER010", registry, f"structural waiver {waiver.identifier} requires a substantive file-specific cohesion rationale"))
    return diagnostics


def _validate_structure(
    root: Path,
    policy: ArchitecturePolicy,
    state: ArchitectureState,
    governed: dict[Path, str],
    today: date,
) -> list[Diagnostic]:
    """Enforce structural limits against exact current dispositions."""

    waivers = {
        waiver.path: waiver
        for waiver in state.waivers
        if waiver.review_by >= today and waiver.rule == "STRUCT003"
    }
    diagnostics: list[Diagnostic] = []
    for path, area in governed.items():
        relative = path.relative_to(root).as_posix()
        lines = production_line_count(path)
        if lines > policy.hard_lines and relative not in waivers:
            diagnostics.append(Diagnostic("STRUCT003", relative, f"{lines} production lines exceed the hard gate {policy.hard_lines} in owned area {area}. {_OWNERSHIP_RESPONSE}"))
        elif lines > policy.soft_lines and relative not in waivers:
            diagnostics.append(Diagnostic("STRUCT002", relative, f"{lines} production lines exceed the soft ceiling {policy.soft_lines}; assess ownership before extending this file", severity="warning"))
    return diagnostics
