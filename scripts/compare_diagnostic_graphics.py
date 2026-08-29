"""Compare production graphics evidence from two OpenMWSE diagnostic runs."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path

import numpy


MAGIC = b"OMWATMD1"
VERSION = 1
STAGE_PATTERN = re.compile(r"atmosphere-(.+)-frame-\d+-\d+\.omwdiag")
COMPARED_IMAGES = {
    "scene-color-before-post-processing",
    "scene-opaque-depth",
    "atmosphere-volume-resolved",
    "atmosphere-volume-metadata",
    "atmosphere-shafts",
    "final-composite",
}
MAXIMUM_MEAN_ABSOLUTE_ERROR = {
    "scene-color-before-post-processing": 0.002,
    "scene-opaque-depth": 0.00001,
    "atmosphere-volume-resolved": 0.002,
    "atmosphere-volume-metadata": 0.0001,
    "atmosphere-shafts": 0.0001,
    "final-composite": 0.001,
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
            if label in COMPARED_IMAGES:
                pixels = numpy.fromfile(stream, dtype="<f4", count=value_count)
                if pixels.size != value_count:
                    raise ValueError(f"truncated pixels for {label} in {path}")
                images[label] = pixels.reshape(height, width, channels)
            else:
                stream.seek(value_count * 4, 1)
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


def compare_directories(reference: Path, replay: Path) -> dict:
    """Return enforcing numerical parity evidence for matching production stages."""
    reference_stages = index_stages(reference)
    replay_stages = index_stages(replay)
    if reference_stages.keys() != replay_stages.keys():
        raise ValueError("reference and replay stages do not match")

    report: dict = {
        "reference": str(reference),
        "replay": str(replay),
        "passed": True,
        "limits": {"maximum_mean_absolute": MAXIMUM_MEAN_ABSOLUTE_ERROR},
        "stages": {},
    }
    for stage in sorted(reference_stages):
        reference_images = read_images(reference_stages[stage])
        replay_images = read_images(replay_stages[stage])
        if reference_images.keys() != replay_images.keys():
            raise ValueError(f"attachment sets differ for stage {stage}")
        stage_report: dict = {}
        for name in sorted(reference_images):
            reference_pixels = reference_images[name]
            replay_pixels = replay_images[name]
            if reference_pixels.shape != replay_pixels.shape:
                raise ValueError(f"attachment dimensions differ for {stage}/{name}")
            finite = numpy.isfinite(reference_pixels) & numpy.isfinite(replay_pixels)
            if not finite.all():
                raise ValueError(f"non-finite pixels found in {stage}/{name}")
            difference = numpy.abs(reference_pixels - replay_pixels)
            stage_report[name] = {
                "mean_absolute": float(difference.mean()),
                "p999_absolute": float(numpy.quantile(difference, 0.999)),
                "maximum_absolute": float(difference.max()),
            }
            if stage_report[name]["mean_absolute"] > MAXIMUM_MEAN_ABSOLUTE_ERROR[name]:
                report["passed"] = False
        report["stages"][stage] = stage_report
    return report


def parse_arguments() -> argparse.Namespace:
    """Parse the two production capture directories and optional output path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("replay", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    """Compare both runs, emit evidence, and fail when replay parity regresses."""
    arguments = parse_arguments()
    report = compare_directories(arguments.reference, arguments.replay)
    encoded = json.dumps(report, indent=2)
    if arguments.output:
        arguments.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
