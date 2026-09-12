"""Test owned-area architecture governance."""

from __future__ import annotations

from datetime import date
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_ROOT))

from architecture_governance.metrics import source_fingerprint
from architecture_governance.validation import validate_repository


def _write(path: Path, content: str) -> None:
    """Write one UTF-8 fixture after creating its parent."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _write_policy(root: Path) -> None:
    """Write a compact owned-area policy and empty state."""

    _write(
        root / "ARCHITECTURE_POLICY.toml",
        """schema_version = 2
[structure]
soft_lines = 4
hard_lines = 6
source_extensions = [".cpp"]
excluded_paths = []
[[areas]]
id = "owned"
owner = "fixture owner"
status = "active"
source_roots = ["owned"]
source_files = []
[registries]
inventory = "ARCHITECTURE_INVENTORY.toml"
debt = "ARCHITECTURE_DEBT.toml"
waivers = "ARCHITECTURE_WAIVERS.toml"
""",
    )
    _write(root / "owned/small.cpp", "int value = 1;\n")
    _write_inventory(root)
    _write(root / "ARCHITECTURE_DEBT.toml", "schema_version = 1\ndebts = []\n")
    _write(root / "ARCHITECTURE_WAIVERS.toml", "schema_version = 1\nwaivers = []\n")


def _large_source() -> str:
    """Return source above the compact fixture's hard limit."""

    return "\n".join(f"int value_{index} = {index};" for index in range(8)) + "\n"


def _write_inventory(root: Path) -> None:
    """Record the exact current fixture-owned source snapshot."""

    paths = tuple(path.relative_to(root).as_posix() for path in sorted((root / "owned").glob("*.cpp")))
    fingerprint = source_fingerprint(root, paths)
    _write(
        root / "ARCHITECTURE_INVENTORY.toml",
        "schema_version = 1\n[[inventories]]\n"
        'id = "INVENTORY-owned"\narea = "owned"\nowner = "fixture owner"\n'
        f'fingerprint = "{fingerprint}"\nfile_count = {len(paths)}\nassessed_on = 2026-09-04\n',
    )


class ArchitectureGovernanceTests(unittest.TestCase):
    """Verify structural policy, state freshness, and repository validity."""

    def test_unclassified_hard_overage_fails(self) -> None:
        """Require an explicit disposition for every hard overage."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            _write(root / "owned/large.cpp", _large_source())
            _write_inventory(root)
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertTrue(any(item.rule == "STRUCT003" for item in diagnostics))

    def test_unowned_source_is_not_governed(self) -> None:
        """Keep governance limited to explicitly owned areas."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            _write(root / "upstream/large.cpp", _large_source())
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertFalse(any(item.path == "upstream/large.cpp" for item in diagnostics))

    def test_fingerprinted_debt_and_tightening_waiver_pass(self) -> None:
        """Accept exact assessed debt with one bounded remediation path."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            source_path = "owned/large.cpp"
            _write(root / source_path, _large_source())
            _write_inventory(root)
            fingerprint = source_fingerprint(root, (source_path,))
            _write(
                root / "ARCHITECTURE_DEBT.toml",
                "schema_version = 1\n[[debts]]\n"
                'id = "DEBT-1"\narea = "owned"\nowner = "fixture owner"\n'
                f'paths = ["{source_path}"]\nfingerprint = "{fingerprint}"\n'
                'issue = "backlog:DEBT-1"\nreview_by = 2027-01-01\n'
                'responsibilities = ["state", "presentation"]\n'
                'next_extraction = "Extract presentation from state ownership."\n',
            )
            _write(
                root / "ARCHITECTURE_WAIVERS.toml",
                "schema_version = 1\n[[waivers]]\n"
                'id = "WAIVER-1"\narea = "owned"\nowner = "fixture owner"\n'
                'rule = "STRUCT003"\n'
                f'path = "{source_path}"\nkind = "remediation"\n'
                'justification = "Current mixed ownership is queued for extraction."\n'
                'issue = "backlog:DEBT-1"\nreview_by = 2027-01-01\n'
                f'fingerprint = "{fingerprint}"\n'
                'max_lines = 8\nnext_limit = 6\ndebt = "DEBT-1"\n',
            )
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertFalse(any(item.severity == "error" for item in diagnostics))

    def test_structural_waiver_is_fingerprinted_and_file_specific(self) -> None:
        """Accept a cohesive hard overage only for its exact assessed source."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            source_path = "owned/large.cpp"
            _write(root / source_path, _large_source())
            _write_inventory(root)
            fingerprint = source_fingerprint(root, (source_path,))
            justification = (
                "This fixture keeps one atomic table-driven implementation together because its entries share one "
                "invariant, one lifecycle, and one review cadence; extraction would split that invariant without "
                "creating an independent owner or reducing coupling."
            )
            _write(
                root / "ARCHITECTURE_WAIVERS.toml",
                "schema_version = 1\n[[waivers]]\n"
                'id = "WAIVER-STRUCTURAL"\narea = "owned"\nowner = "fixture owner"\n'
                'rule = "STRUCT003"\n'
                f'path = "{source_path}"\nkind = "structural"\n'
                f'justification = "{justification}"\n'
                'issue = "architecture:fixture-review"\nreview_by = 2027-01-01\n'
                f'fingerprint = "{fingerprint}"\nmax_lines = 8\n',
            )
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertEqual([item for item in diagnostics if item.severity == "error"], [])

            _write(root / source_path, _large_source() + "int changed = 1;\n")
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertTrue(any(item.rule == "WAIVER012" for item in diagnostics))

    def test_reserved_area_blocks_unactivated_source(self) -> None:
        """Require future ownership boundaries to become active before source lands."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            policy = (root / "ARCHITECTURE_POLICY.toml").read_text(encoding="utf-8")
            policy += """
[[areas]]
id = "future"
owner = "future owner"
status = "reserved"
source_roots = ["future"]
source_files = []
"""
            _write(root / "ARCHITECTURE_POLICY.toml", policy)
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertFalse(any(item.rule == "POLICY008" for item in diagnostics))

            _write(root / "future/implementation.cpp", "int future = 1;\n")
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertTrue(any(item.rule == "POLICY008" for item in diagnostics))

    def test_inventory_detects_owned_source_change(self) -> None:
        """Require the complete owned-area assessment to follow source changes."""

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_policy(root)
            _write(root / "owned/small.cpp", "int value = 2;\n")
            diagnostics = validate_repository(root, today=date(2026, 9, 4))
            self.assertTrue(any(item.rule == "INVENTORY004" for item in diagnostics))

    def test_current_repository_has_no_governance_errors(self) -> None:
        """Keep checked-in owned architecture exact and enforceable."""

        repository_root = SCRIPTS_ROOT.parent
        errors = [item for item in validate_repository(repository_root) if item.severity == "error"]
        self.assertEqual(errors, [])

if __name__ == "__main__":
    unittest.main()
