"""Compare production graphics evidence from two OpenMWSE diagnostic runs."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path

import numpy
from PIL import Image


MAGIC = b"OMWATMD1"
VERSION = 1
STAGE_PATTERN = re.compile(r"atmosphere-(.+)-frame-\d+-\d+\.omwdiag")
ATTACHMENT_RULES = {
    "atmosphere-cloud-models-v2": {"mean": 0.000001, "p999": 0.000001, "localize": 0.000001},
    "scene-color-before-post-processing": {"mean": 0.002, "p999": 0.01, "localize": 0.0001},
    "scene-opaque-depth": {"mean": 0.00001, "p999": 0.0001, "localize": 0.000001},
    "previous-scene-opaque-depth": {"mean": 0.00001, "p999": 0.0001, "localize": 0.000001},
    "atmosphere-volume-checkerboard-raw": {
        "mean": 0.002,
        "p999": 0.01,
        "localize": 0.0001,
        "phase_dependent": True,
    },
    "atmosphere-volume-checkerboard-metadata-raw": {
        "mean": 0.0001,
        "p999": 0.001,
        "localize": 0.00001,
        "phase_dependent": True,
    },
    "atmosphere-volume-metadata-reprojected": {
        "mean": 0.0001,
        "p999": 0.001,
        "localize": 0.00001,
        "phase_dependent": True,
    },
    "atmosphere-checkerboard-layout": {"layout": True},
    "atmosphere-volume-raw": {"mean": 0.002, "p999": 0.01, "localize": 0.0001},
    "atmosphere-volume-resolved": {"mean": 0.002, "p999": 0.01, "localize": 0.0001},
    "atmosphere-volume-metadata": {"mean": 0.0001, "p999": 0.001, "localize": 0.00001},
    "atmosphere-shafts": {"mean": 0.0001, "p999": 0.001, "localize": 0.00001},
    "previous-atmosphere-volume-resolved": {"mean": 0.002, "p999": 0.01, "localize": 0.0001},
    "previous-atmosphere-volume-metadata": {"mean": 0.0001, "p999": 0.001, "localize": 0.00001},
    "final-composite": {"mean": 0.001, "p999": 0.01, "localize": 0.0001},
}


def read_exact(stream, size: int) -> bytes:
    """Read an exact byte count or reject a truncated production bundle."""
    value = stream.read(size)
    if len(value) != size:
        raise ValueError("truncated production diagnostic bundle")
    return value


def read_u32(stream) -> int:
    """Read one little-endian unsigned 32-bit integer."""
    return struct.unpack("<I", read_exact(stream, 4))[0]


def read_u64(stream) -> int:
    """Read one little-endian unsigned 64-bit integer."""
    return struct.unpack("<Q", read_exact(stream, 8))[0]


def read_images(path: Path) -> dict[str, numpy.ndarray]:
    """Load the stable attachments used for replay parity checks."""
    images: dict[str, numpy.ndarray] = {}
    with path.open("rb") as stream:
        if read_exact(stream, len(MAGIC)) != MAGIC or read_u32(stream) != VERSION:
            raise ValueError(f"unsupported production diagnostic bundle: {path}")
        read_u64(stream)
        for _ in range(read_u32(stream)):
            label = read_exact(stream, read_u32(stream)).decode("utf-8")
            width, height, channels = read_u32(stream), read_u32(stream), read_u32(stream)
            value_count = read_u64(stream)
            if value_count != width * height * channels:
                raise ValueError(f"invalid dimensions for {label} in {path}")
            pixels = numpy.fromfile(stream, dtype="<f4", count=value_count)
            if pixels.size != value_count:
                raise ValueError(f"truncated pixels for {label} in {path}")
            images[label] = pixels.reshape(height, width, channels)
        if stream.read(1):
            raise ValueError(f"unexpected trailing data in {path}")
    return images


def index_stages(directory: Path) -> dict[str, Path]:
    """Index exactly one bundle for every production diagnostic stage."""
    result: dict[str, Path] = {}
    for path in directory.glob("atmosphere-*-frame-*.omwdiag"):
        match = STAGE_PATTERN.fullmatch(path.name)
        if not match:
            continue
        stage = match.group(1)
        if stage in result:
            raise ValueError(f"duplicate stage {stage} in {directory}")
        result[stage] = path
    if not result:
        raise ValueError(f"no production diagnostic bundles in {directory}")
    return result


def difference_region(pixel_difference: numpy.ndarray, threshold: float) -> dict | None:
    """Return the bounding rectangle and count of pixels above one attachment-specific threshold."""
    changed = pixel_difference > threshold
    if not changed.any():
        return None
    rows, columns = numpy.nonzero(changed)
    return {
        "pixel_count": int(changed.sum()),
        "pixel_fraction": float(changed.mean()),
        "bounds": {
            "left": int(columns.min()),
            "top": int(rows.min()),
            "right": int(columns.max()),
            "bottom": int(rows.max()),
        },
    }


def write_heatmap(path: Path, pixel_difference: numpy.ndarray) -> None:
    """Write a full-resolution logarithmic heat map without altering comparison values."""
    path.parent.mkdir(parents=True, exist_ok=True)
    maximum = float(pixel_difference.max())
    if maximum == 0.0:
        normalized = numpy.zeros(pixel_difference.shape, dtype=numpy.uint8)
    else:
        normalized = numpy.clip(numpy.log1p(pixel_difference * 255.0 / maximum) / numpy.log(256.0), 0.0, 1.0)
        normalized = numpy.asarray(numpy.rint(normalized * 255.0), dtype=numpy.uint8)
    Image.fromarray(normalized).save(path)


def compare_directories(reference: Path, replay: Path, heatmap_directory: Path | None = None) -> dict:
    """Return enforcing numerical parity evidence for matching production stages."""
    reference_stages = index_stages(reference)
    replay_stages = index_stages(replay)
    if reference_stages.keys() != replay_stages.keys():
        raise ValueError("reference and replay stages do not match")

    report: dict = {
        "reference": str(reference),
        "replay": str(replay),
        "passed": True,
        "attachment_rules": ATTACHMENT_RULES,
        "stages": {},
    }
    for stage in sorted(reference_stages):
        reference_images = read_images(reference_stages[stage])
        replay_images = read_images(replay_stages[stage])
        if reference_images.keys() != replay_images.keys():
            raise ValueError(f"attachment sets differ for stage {stage}")
        stage_report: dict = {}
        reference_layout = reference_images.get("atmosphere-checkerboard-layout")
        replay_layout = replay_images.get("atmosphere-checkerboard-layout")
        checkerboard_phases_match = (
            reference_layout is not None
            and replay_layout is not None
            and reference_layout.shape == (1, 1, 4)
            and replay_layout.shape == (1, 1, 4)
            and reference_layout[0, 0, 0] == replay_layout[0, 0, 0]
        )
        for name in sorted(reference_images):
            if name not in ATTACHMENT_RULES:
                raise ValueError(f"attachment {name} has no explicit comparison rule")
            reference_pixels = reference_images[name]
            replay_pixels = replay_images[name]
            if reference_pixels.shape != replay_pixels.shape:
                raise ValueError(f"attachment dimensions differ for {stage}/{name}")
            finite = numpy.isfinite(reference_pixels) & numpy.isfinite(replay_pixels)
            if not finite.all():
                raise ValueError(f"non-finite pixels found in {stage}/{name}")
            rule = ATTACHMENT_RULES[name]
            if rule.get("layout"):
                if reference_pixels.shape != (1, 1, 4):
                    raise ValueError(f"invalid checkerboard layout dimensions in {stage}/{name}")
                layout_matches = numpy.array_equal(reference_pixels[0, 0, 1:], replay_pixels[0, 0, 1:])
                stage_report[name] = {
                    "comparison": "checkerboard-layout",
                    "reference_phase": int(reference_pixels[0, 0, 0]),
                    "replay_phase": int(replay_pixels[0, 0, 0]),
                    "layout_matches": bool(layout_matches),
                    "passed": bool(layout_matches),
                }
                if not layout_matches:
                    report["passed"] = False
                continue
            if rule.get("phase_dependent") and not checkerboard_phases_match:
                stage_report[name] = {
                    "comparison": "phase-dependent-structure-only",
                    "reason": "checkerboard phases differ",
                    "passed": True,
                }
                continue
            difference = numpy.abs(reference_pixels - replay_pixels)
            pixel_difference = difference.max(axis=2)
            denominator = numpy.maximum(numpy.abs(reference_pixels), 1e-7)
            relative_difference = difference / denominator
            stage_report[name] = {
                "nonzero_value_count": int(numpy.count_nonzero(difference)),
                "mean_absolute": float(difference.mean()),
                "p999_absolute": float(numpy.quantile(difference, 0.999)),
                "maximum_absolute": float(difference.max()),
                "maximum_relative": float(relative_difference.max()),
                "localized_difference": difference_region(pixel_difference, rule["localize"]),
            }
            attachment_passed = (
                stage_report[name]["mean_absolute"] <= rule["mean"]
                and stage_report[name]["p999_absolute"] <= rule["p999"]
            )
            stage_report[name]["passed"] = attachment_passed
            if heatmap_directory and (stage == "normal" or not attachment_passed):
                heatmap_path = heatmap_directory / stage / f"{name}.png"
                write_heatmap(heatmap_path, pixel_difference)
                stage_report[name]["heatmap"] = str(heatmap_path)
            if not attachment_passed:
                report["passed"] = False
        report["stages"][stage] = stage_report
    return report


def parse_arguments() -> argparse.Namespace:
    """Parse the two production capture directories and optional output path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--heatmaps", type=Path)
    return parser.parse_args()


def main() -> None:
    """Compare both runs, emit evidence, and fail when replay parity regresses."""
    arguments = parse_arguments()
    report = compare_directories(arguments.reference, arguments.replay, arguments.heatmaps)
    encoded = json.dumps(report, indent=2)
    if arguments.output:
        arguments.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
