#!/usr/bin/env python3
"""Generate the deterministic tileable 3D noise texture used by OpenMWSE clouds."""

from __future__ import annotations

import argparse
import hashlib
import math
import struct
from pathlib import Path


BASE_TEXTURE_SIZE = 64
DETAIL_TEXTURE_SIZE = 32
WEATHER_TEXTURE_SIZE = 256
RANDOM_SEED = 0x4F4D5753


def hash_u32(x: int, y: int, z: int, seed: int = RANDOM_SEED) -> int:
    """Return a stable 32-bit integer hash for a periodic lattice coordinate."""
    value = (x * 0x8DA6B343) ^ (y * 0xD8163841) ^ (z * 0xCB1AB31F) ^ seed
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    return value ^ (value >> 16)


def hash_unit(x: int, y: int, z: int, channel: int = 0) -> float:
    """Map a lattice coordinate and channel to a deterministic unit interval."""
    return hash_u32(x, y, z, RANDOM_SEED + channel * 0x9E3779B9) / 0xFFFFFFFF


def lerp(first: float, second: float, amount: float) -> float:
    """Interpolate between two scalar values."""
    return first + (second - first) * amount


def perlin_fade(value: float) -> float:
    """Apply Perlin's quintic interpolation curve to a unit coordinate."""
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0)


def periodic_gradient_noise(x: float, y: float, z: float, frequency: int) -> float:
    """Evaluate tileable three-dimensional gradient noise at one lattice frequency."""
    scaled_x = x * frequency
    scaled_y = y * frequency
    scaled_z = z * frequency
    cell_x = math.floor(scaled_x)
    cell_y = math.floor(scaled_y)
    cell_z = math.floor(scaled_z)
    fractional_x = scaled_x - cell_x
    fractional_y = scaled_y - cell_y
    fractional_z = scaled_z - cell_z
    blend_x = perlin_fade(fractional_x)
    blend_y = perlin_fade(fractional_y)
    blend_z = perlin_fade(fractional_z)

    gradients = (
        (1.0, 1.0, 0.0),
        (-1.0, 1.0, 0.0),
        (1.0, -1.0, 0.0),
        (-1.0, -1.0, 0.0),
        (1.0, 0.0, 1.0),
        (-1.0, 0.0, 1.0),
        (1.0, 0.0, -1.0),
        (-1.0, 0.0, -1.0),
        (0.0, 1.0, 1.0),
        (0.0, -1.0, 1.0),
        (0.0, 1.0, -1.0),
        (0.0, -1.0, -1.0),
    )

    planes: list[float] = []
    for corner_z in range(2):
        rows: list[float] = []
        for corner_y in range(2):
            values: list[float] = []
            for corner_x in range(2):
                wrapped_x = (cell_x + corner_x) % frequency
                wrapped_y = (cell_y + corner_y) % frequency
                wrapped_z = (cell_z + corner_z) % frequency
                gradient = gradients[hash_u32(wrapped_x, wrapped_y, wrapped_z) % len(gradients)]
                displacement = (
                    fractional_x - corner_x,
                    fractional_y - corner_y,
                    fractional_z - corner_z,
                )
                values.append(sum(component * distance for component, distance in zip(gradient, displacement)))
            lower, upper = values
            rows.append(lerp(lower, upper, blend_x))
        planes.append(lerp(rows[0], rows[1], blend_y))
    return 0.5 + lerp(planes[0], planes[1], blend_z) * 0.5


def periodic_worley_inverse(x: float, y: float, z: float, frequency: int) -> float:
    """Evaluate one minus the clamped squared tileable Worley feature-point distance."""
    scaled_x = x * frequency
    scaled_y = y * frequency
    scaled_z = z * frequency
    cell_x = math.floor(scaled_x)
    cell_y = math.floor(scaled_y)
    cell_z = math.floor(scaled_z)
    nearest_squared = 3.0

    for offset_z in (-1, 0, 1):
        for offset_y in (-1, 0, 1):
            for offset_x in (-1, 0, 1):
                candidate_x = cell_x + offset_x
                candidate_y = cell_y + offset_y
                candidate_z = cell_z + offset_z
                wrapped_x = candidate_x % frequency
                wrapped_y = candidate_y % frequency
                wrapped_z = candidate_z % frequency
                point_x = candidate_x + hash_unit(wrapped_x, wrapped_y, wrapped_z, 1)
                point_y = candidate_y + hash_unit(wrapped_x, wrapped_y, wrapped_z, 2)
                point_z = candidate_z + hash_unit(wrapped_x, wrapped_y, wrapped_z, 3)
                distance_squared = (
                    (point_x - scaled_x) ** 2
                    + (point_y - scaled_y) ** 2
                    + (point_z - scaled_z) ** 2
                )
                nearest_squared = min(nearest_squared, distance_squared)

    return 1.0 - min(max(nearest_squared, 0.0), 1.0)


def remap(value: float, lower: float, upper: float) -> float:
    """Map a scalar interval to the unit range with degenerate-range protection."""
    return min(max((value - lower) / max(upper - lower, 1e-6), 0.0), 1.0)


def dilate_perlin_worley(perlin: float, worley: float, blend: float) -> float:
    """Blend Perlin and Worley fields through a bounded dilation curve."""
    curve = 0.75
    if blend < 0.5:
        normalized_blend = blend / 0.5
        combined = perlin + worley * normalized_blend
        dilation = lerp(1.0, 0.5, normalized_blend**curve)
    else:
        normalized_blend = (blend - 0.5) / 0.5
        combined = worley + perlin * (1.0 - normalized_blend)
        dilation = lerp(0.5, 1.0, normalized_blend ** (1.0 / curve))
    return min(max(combined * dilation, 0.0), 1.0)


def generate_base_voxels(size: int = BASE_TEXTURE_SIZE) -> bytes:
    """Generate RGBA8 Perlin-Worley base and three Worley erosion octaves."""
    voxels = bytearray(size * size * size * 4)
    index = 0
    for z_index in range(size):
        z = (z_index + 0.5) / size
        for y_index in range(size):
            y = (y_index + 0.5) / size
            for x_index in range(size):
                x = (x_index + 0.5) / size
                worley_4 = periodic_worley_inverse(x, y, z, 4)
                worley_8 = periodic_worley_inverse(x, y, z, 8)
                worley_16 = periodic_worley_inverse(x, y, z, 16)
                worley_32 = periodic_worley_inverse(x, y, z, 32)
                worley_64 = periodic_worley_inverse(x, y, z, 64)
                value_fbm = (
                    periodic_gradient_noise(x, y, z, 8) * 0.625
                    + periodic_gradient_noise(x, y, z, 16) * 0.25
                    + periodic_gradient_noise(x, y, z, 32) * 0.125
                )
                shape_worley_fbm = worley_4 * 0.625 + worley_8 * 0.25 + worley_16 * 0.125
                perlin_worley = remap(
                    dilate_perlin_worley(value_fbm, shape_worley_fbm, 0.3), 0.24, 0.84
                )
                large_worley_fbm = worley_8 * 0.625 + worley_16 * 0.25 + worley_32 * 0.125
                medium_worley_fbm = worley_16 * 0.625 + worley_32 * 0.25 + worley_64 * 0.125
                small_worley_fbm = worley_32 * 0.75 + worley_64 * 0.25
                for value in (perlin_worley, large_worley_fbm, medium_worley_fbm, small_worley_fbm):
                    voxels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
                    index += 1
    return bytes(voxels)


def generate_detail_voxels(size: int = DETAIL_TEXTURE_SIZE) -> bytes:
    """Generate independent high-frequency Worley erosion and distortion channels."""
    voxels = bytearray(size * size * size * 4)
    index = 0
    for z_index in range(size):
        z = (z_index + 0.5) / size
        for y_index in range(size):
            y = (y_index + 0.5) / size
            for x_index in range(size):
                x = (x_index + 0.5) / size
                worley_2 = periodic_worley_inverse(x, y, z, 2)
                worley_4 = periodic_worley_inverse(x, y, z, 4)
                worley_8 = periodic_worley_inverse(x, y, z, 8)
                worley_16 = periodic_worley_inverse(x, y, z, 16)
                values = (
                    worley_2 * 0.625 + worley_4 * 0.25 + worley_8 * 0.125,
                    worley_4 * 0.625 + worley_8 * 0.25 + worley_16 * 0.125,
                    worley_8 * 0.75 + worley_16 * 0.25,
                    periodic_gradient_noise(x, y, z, 8),
                )
                for value in values:
                    voxels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
                    index += 1
    return bytes(voxels)


def generate_weather_texels(size: int = WEATHER_TEXTURE_SIZE) -> bytes:
    """Generate broad coverage, base-height, development, and family potentials."""
    weather_values: list[tuple[float, float, float, float]] = []
    for y_index in range(size):
        y = (y_index + 0.5) / size
        for x_index in range(size):
            x = (x_index + 0.5) / size
            coverage = (
                periodic_gradient_noise(x, y, 0.173, 2) * 0.5
                + periodic_gradient_noise(x, y, 0.173, 4) * 0.3
                + periodic_worley_inverse(x, y, 0.173, 4) * 0.2
            )
            base_height = (
                periodic_gradient_noise(x, y, 0.419, 4) * 0.7
                + periodic_gradient_noise(x, y, 0.419, 8) * 0.3
            )
            development = (
                periodic_worley_inverse(x, y, 0.731, 4) * 0.55
                + periodic_gradient_noise(x, y, 0.731, 4) * 0.3
                + periodic_gradient_noise(x, y, 0.731, 8) * 0.15
            )
            family = (
                periodic_gradient_noise(x, y, 0.887, 2) * 0.65
                + periodic_gradient_noise(x, y, 0.887, 4) * 0.35
            )
            weather_values.append((
                coverage,
                remap(base_height, 0.3, 0.7),
                remap(development, 0.25, 0.75),
                remap(family, 0.3, 0.7),
            ))

    coverage_ranks = [0.0] * len(weather_values)
    for rank, weather_index in enumerate(sorted(range(len(weather_values)), key=lambda i: weather_values[i][0])):
        coverage_ranks[weather_index] = rank / max(len(weather_values) - 1, 1)

    texels = bytearray(size * size * 4)
    index = 0
    for weather_index, (_, base_height, development, family) in enumerate(weather_values):
        for value in (coverage_ranks[weather_index], base_height, development, family):
            texels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
            index += 1
    return bytes(texels)


def dds_volume(voxels: bytes, size: int) -> bytes:
    """Encode an uncompressed RGBA8 volume using the portable legacy DDS header."""
    pixel_format = struct.pack(
        "<8I",
        32,
        0x41,
        0,
        32,
        0x000000FF,
        0x0000FF00,
        0x00FF0000,
        0xFF000000,
    )
    header = struct.pack(
        "<7I11I",
        124,
        0x0080100F,
        size,
        size,
        size * 4,
        size,
        0,
        *([0] * 11),
    )
    caps = struct.pack("<5I", 0x1008, 0x00200000, 0, 0, 0)
    return b"DDS " + header + pixel_format + caps + voxels


def dds_texture(texels: bytes, size: int) -> bytes:
    """Encode an uncompressed square RGBA8 texture using the portable legacy DDS header."""
    pixel_format = struct.pack(
        "<8I",
        32,
        0x41,
        0,
        32,
        0x000000FF,
        0x0000FF00,
        0x00FF0000,
        0xFF000000,
    )
    header = struct.pack(
        "<7I11I",
        124,
        0x0000100F,
        size,
        size,
        size * 4,
        0,
        0,
        *([0] * 11),
    )
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return b"DDS " + header + pixel_format + caps + texels


def main() -> int:
    """Write or verify the canonical cloud weather, base, and detail resources."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_output", type=Path)
    parser.add_argument("detail_output", type=Path)
    parser.add_argument("weather_output", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    outputs = (
        (arguments.base_output, dds_volume(generate_base_voxels(), BASE_TEXTURE_SIZE)),
        (arguments.detail_output, dds_volume(generate_detail_voxels(), DETAIL_TEXTURE_SIZE)),
        (arguments.weather_output, dds_texture(generate_weather_texels(), WEATHER_TEXTURE_SIZE)),
    )
    if arguments.check:
        success = True
        for path, encoded in outputs:
            existing = path.read_bytes() if path.exists() else b""
            if existing != encoded:
                print(f"out of date: {path}")
                success = False
            else:
                print(f"verified {path} sha256={hashlib.sha256(encoded).hexdigest()}")
        return 0 if success else 1

    for path, encoded in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(encoded)
        print(f"wrote {path} sha256={hashlib.sha256(encoded).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
