"""Inspect and extract lossless OpenMWSE production atmosphere capture bundles."""

import argparse
import json
import re
import struct
from pathlib import Path

import numpy


MAGIC = b"OMWATMD1"
VERSION = 1


def read_exact(stream, size: int) -> bytes:
    """Read an exact byte count or reject a truncated diagnostic bundle."""
    value = stream.read(size)
    if len(value) != size:
        raise ValueError("truncated atmosphere diagnostic bundle")
    return value


def read_u32(stream) -> int:
    """Read one little-endian unsigned 32-bit integer."""
    return struct.unpack("<I", read_exact(stream, 4))[0]


def read_u64(stream) -> int:
    """Read one little-endian unsigned 64-bit integer."""
    return struct.unpack("<Q", read_exact(stream, 8))[0]


def read_label(stream) -> str:
    """Read and validate one length-prefixed UTF-8 attachment label."""
    return read_exact(stream, read_u32(stream)).decode("utf-8")


def safe_file_stem(label: str) -> str:
    """Convert a diagnostic attachment label into a portable file stem."""
    stem = re.sub(r"[^a-zA-Z0-9._-]+", "-", label).strip("-.")
    if not stem:
        raise ValueError("diagnostic attachment has no usable file name")
    return stem


def write_pfm(path: Path, pixels: numpy.ndarray) -> None:
    """Write bottom-up little-endian floating-point pixels without changing their values."""
    if pixels.ndim == 2:
        magic = b"Pf"
    elif pixels.ndim == 3 and pixels.shape[2] == 3:
        magic = b"PF"
    else:
        raise ValueError(f"PFM requires one or three channels, received shape {pixels.shape}")
    with path.open("wb") as stream:
        stream.write(magic + b"\n")
        stream.write(f"{pixels.shape[1]} {pixels.shape[0]}\n-1.0\n".encode("ascii"))
        stream.write(numpy.asarray(pixels, dtype="<f4").tobytes(order="C"))


def inspect_bundle(bundle_path: Path, extract_directory: Path | None) -> dict:
    """Validate a capture bundle, report numerical ranges, and optionally extract PFM attachments."""
    summary: dict = {"bundle": str(bundle_path), "images": []}
    if extract_directory:
        extract_directory.mkdir(parents=True, exist_ok=True)

    with bundle_path.open("rb") as stream:
        if read_exact(stream, len(MAGIC)) != MAGIC:
            raise ValueError("not an OpenMWSE atmosphere diagnostic bundle")
        version = read_u32(stream)
        if version != VERSION:
            raise ValueError(f"unsupported atmosphere diagnostic version {version}")
        summary["version"] = version
        summary["frame"] = read_u64(stream)

        for _ in range(read_u32(stream)):
            label = read_label(stream)
            width = read_u32(stream)
            height = read_u32(stream)
            channels = read_u32(stream)
            value_count = read_u64(stream)
            expected_count = width * height * channels
            if value_count != expected_count:
                raise ValueError(
                    f"{label} declares {value_count} values but dimensions require {expected_count}"
                )
            pixels = numpy.fromfile(stream, dtype="<f4", count=value_count)
            if pixels.size != value_count:
                raise ValueError(f"truncated pixel payload for {label}")
            pixels = pixels.reshape((height, width, channels))
            finite = numpy.isfinite(pixels)
            finite_values = pixels[finite]
            image_summary = {
                "name": label,
                "width": width,
                "height": height,
                "channels": channels,
                "finite_fraction": float(finite.mean()),
                "minimum": float(finite_values.min()) if finite_values.size else None,
                "maximum": float(finite_values.max()) if finite_values.size else None,
            }
            summary["images"].append(image_summary)

            if extract_directory:
                stem = safe_file_stem(label)
                if channels == 1:
                    write_pfm(extract_directory / f"{stem}.pfm", pixels[:, :, 0])
                elif channels >= 3:
                    write_pfm(extract_directory / f"{stem}-rgb.pfm", pixels[:, :, :3])
                    if channels >= 4:
                        write_pfm(extract_directory / f"{stem}-alpha.pfm", pixels[:, :, 3])

        if stream.read(1):
            raise ValueError("unexpected trailing data in atmosphere diagnostic bundle")
    return summary


def parse_arguments() -> argparse.Namespace:
    """Parse the capture bundle and optional extraction directory from the command line."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="Path to an .omwdiag capture bundle")
    parser.add_argument("--extract", type=Path, help="Directory for lossless PFM attachment extraction")
    return parser.parse_args()


def main() -> None:
    """Inspect the requested bundle and print a machine-readable numerical summary."""
    arguments = parse_arguments()
    print(json.dumps(inspect_bundle(arguments.bundle, arguments.extract), indent=2))


if __name__ == "__main__":
    main()
