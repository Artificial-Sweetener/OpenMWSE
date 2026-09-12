"""Check architecture governance for OpenMWSE-owned source areas."""

from __future__ import annotations

from pathlib import Path
import sys
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from architecture_governance.loading import load_policy, load_state
from architecture_governance.validation import validate_repository


def main(argv: Sequence[str] | None = None) -> int:
    """Validate current owned-area architecture and print stable diagnostics."""

    if argv:
        raise ValueError("the architecture checker accepts no arguments")
    root = Path(__file__).resolve().parents[1]
    diagnostics = validate_repository(root)
    for diagnostic in diagnostics:
        print(diagnostic.render())
    errors = [item for item in diagnostics if item.severity == "error"]
    if errors:
        print(f"FAILED: Found {len(errors)} architecture governance errors.")
        return 1
    policy = load_policy(root / "ARCHITECTURE_POLICY.toml")
    state = load_state(root, policy)
    warnings = [item for item in diagnostics if item.severity == "warning"]
    print(
        "SUCCESS: Owned architecture is valid "
        f"({len(state.inventories)} area inventories, {len(state.debts)} debt items, "
        f"{len(state.waivers)} waivers, "
        f"{len(warnings)} structural warnings)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
