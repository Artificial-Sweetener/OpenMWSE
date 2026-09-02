#!/usr/bin/env python3
"""Generate the deterministic tileable 3D noise texture used by OpenMWSE clouds."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import math
import os
import struct
from pathlib import Path


BASE_TEXTURE_SIZE = 64
DETAIL_TEXTURE_SIZE = 32
WEATHER_TEXTURE_SIZE = 256
CURL_TEXTURE_SIZE = 128
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


def gradient_noise(
    x: float, y: float, z: float, frequency: int, *, tile: bool = True
) -> float:
    """Evaluate the three-dimensional Perlin function with optional tiling."""
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

    domain = 69.0
    wrapped_cells = (
        cell_x - math.floor(cell_x / domain) * domain,
        cell_y - math.floor(cell_y / domain) * domain,
        cell_z - math.floor(cell_z / domain) * domain,
    )
    incremented_cells = tuple(value + 1.0 if value <= domain - 1.5 else 0.0 for value in wrapped_cells)
    if tile:
        incremented_cells = tuple(value % frequency for value in incremented_cells)

    def lattice_gradient(grid_x: float, grid_y: float, grid_z: float) -> tuple[float, float, float]:
        """Return the normalized gradient produced by the Perlin hash."""
        product = ((grid_x + 50.0) ** 2) * ((grid_y + 161.0) ** 2)
        components = (
            product / (635.298681 + grid_z * 48.500388),
            product / (682.357502 + grid_z * 65.294118),
            product / (668.926525 + grid_z * 63.934599),
        )
        gradient = tuple(component - math.floor(component) - 0.49999 for component in components)
        inverse_length = 1.0 / max(math.sqrt(sum(component * component for component in gradient)), 1e-8)
        return tuple(component * inverse_length for component in gradient)

    planes: list[float] = []
    for corner_z in range(2):
        rows: list[float] = []
        for corner_y in range(2):
            values: list[float] = []
            for corner_x in range(2):
                grid_x = wrapped_cells[0] if corner_x == 0 else incremented_cells[0]
                grid_y = wrapped_cells[1] if corner_y == 0 else incremented_cells[1]
                grid_z = wrapped_cells[2] if corner_z == 0 else incremented_cells[2]
                gradient = lattice_gradient(grid_x, grid_y, grid_z)
                displacement = (
                    fractional_x - corner_x,
                    fractional_y - corner_y,
                    fractional_z - corner_z,
                )
                values.append(sum(component * distance for component, distance in zip(gradient, displacement)))
            lower, upper = values
            rows.append(lerp(lower, upper, blend_x))
        planes.append(lerp(rows[0], rows[1], blend_y))
    perlin = lerp(planes[0], planes[1], blend_z) / math.sqrt(0.75)
    return (perlin * 1.5 + 1.0) * 0.5


def periodic_worley_distance(
    x: float, y: float, z: float, frequency: int, seed: float = 0.0
) -> float:
    """Evaluate the clamped nearest squared Voronoi distance."""
    scaled_x = x * frequency + 0.5
    scaled_y = y * frequency + 0.5
    scaled_z = z * frequency + 0.5
    cell_x = math.floor(scaled_x)
    cell_y = math.floor(scaled_y)
    cell_z = math.floor(scaled_z)
    fractional_x = scaled_x - cell_x
    fractional_y = scaled_y - cell_y
    fractional_z = scaled_z - cell_z
    nearest_squared = 1.0

    def voronoi_hash(
        grid_x: float, grid_y: float, grid_z: float
    ) -> tuple[float, float, float]:
        """Return the periodic Voronoi feature-point offset."""
        grid_x %= frequency
        grid_y %= frequency
        grid_z %= frequency
        hashes = (
            grid_x * 127.1 + grid_y * 311.7 + grid_z * 74.7,
            grid_x * 269.5 + grid_y * 183.3 + grid_z * 246.1,
            grid_x * 113.5 + grid_y * 271.9 + grid_z * 124.6,
        )
        return tuple((math.sin(value) * 43758.5453123) % 1.0 for value in hashes)

    for offset_z in (-1, 0, 1):
        for offset_y in (-1, 0, 1):
            for offset_x in (-1, 0, 1):
                candidate_x = cell_x + offset_x
                candidate_y = cell_y + offset_y
                candidate_z = cell_z + offset_z
                feature = voronoi_hash(
                    candidate_x + seed * 10.0,
                    candidate_y + seed * 10.0,
                    candidate_z + seed * 10.0,
                )
                point_x = offset_x + feature[0] - fractional_x
                point_y = offset_y + feature[1] - fractional_y
                point_z = offset_z + feature[2] - fractional_z
                distance_squared = (
                    point_x * point_x + point_y * point_y + point_z * point_z
                )
                nearest_squared = min(nearest_squared, distance_squared)

    return min(max(nearest_squared, 0.0), 1.0)


def periodic_perlin_fbm(
    x: float, y: float, z: float, base_frequency: int, octave_count: int
) -> float:
    """Accumulate the unnormalized octave weights expected by the cloud-noise remaps."""
    value = 0.0
    amplitude = 1.0
    frequency = base_frequency
    for _ in range(octave_count):
        value += gradient_noise(x, y, z, frequency) * amplitude
        amplitude *= 0.5
        frequency *= 2
    return value


def periodic_worley_fbm_from_distances(
    base_distance: float, second_distance: float, third_distance: float
) -> float:
    """Combine one inverse and two subtractive Worley octaves into cellular cloud tissue."""
    return (1.0 - base_distance) - second_distance * 0.3 - third_distance * 0.3


def periodic_worley_two_octaves(base_distance: float, second_distance: float) -> float:
    """Combine the two Worley octaves used by the broad weather-coverage field."""
    return (1.0 - base_distance) - second_distance * 0.25


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


def rank_values(values: list[float]) -> list[float]:
    """Convert a scalar field to a deterministic uniform-rank field while preserving topology."""
    ranks = [0.0] * len(values)
    for rank, value_index in enumerate(sorted(range(len(values)), key=values.__getitem__)):
        ranks[value_index] = rank / max(len(values) - 1, 1)
    return ranks


def generate_base_slice(arguments: tuple[int, int]) -> bytes:
    """Generate one deterministic z slice of the Perlin-Worley base volume."""
    z_index, size = arguments
    voxels = bytearray(size * size * 4)
    index = 0
    z = (z_index + 0.5) / size
    for y_index in range(size):
        y = (y_index + 0.5) / size
        for x_index in range(size):
            x = (x_index + 0.5) / size
            distances = {
                frequency: periodic_worley_distance(x, y, z, frequency)
                for frequency in (6, 12, 24, 48, 96)
            }
            perlin = remap(periodic_perlin_fbm(x, y, z, 4, 7), 0.3, 1.4)
            large_worley = periodic_worley_fbm_from_distances(
                distances[6], distances[12], distances[24]
            )
            dilating_worley = remap(large_worley, -0.3, 1.3)
            perlin_worley = dilate_perlin_worley(perlin, dilating_worley, 0.3)
            large_worley_fbm = remap(large_worley, -0.4, 1.0)
            medium_worley_fbm = remap(
                periodic_worley_fbm_from_distances(
                    distances[12], distances[24], distances[48]
                ),
                -0.4,
                1.0,
            )
            small_worley_fbm = remap(
                periodic_worley_fbm_from_distances(
                    distances[24], distances[48], distances[96]
                ),
                -0.4,
                1.0,
            )
            for value in (perlin_worley, large_worley_fbm, medium_worley_fbm, small_worley_fbm):
                voxels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
                index += 1
    return bytes(voxels)


def generate_base_voxels(size: int = BASE_TEXTURE_SIZE) -> bytes:
    """Generate RGBA8 Perlin-Worley base and three Worley erosion octaves."""
    worker_count = min(os.cpu_count() or 1, 16)
    arguments = [(z_index, size) for z_index in range(size)]
    if worker_count == 1:
        return b"".join(generate_base_slice(argument) for argument in arguments)
    with concurrent.futures.ProcessPoolExecutor(max_workers=worker_count) as executor:
        return b"".join(executor.map(generate_base_slice, arguments))


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
                distances = {
                    frequency: periodic_worley_distance(x, y, z, frequency)
                    for frequency in (5, 8, 10, 16, 20, 32, 40)
                }
                values = (
                    remap(
                        periodic_worley_fbm_from_distances(
                            distances[5], distances[10], distances[20]
                        ),
                        -1.0,
                        1.0,
                    ),
                    remap(
                        periodic_worley_fbm_from_distances(
                            distances[8], distances[16], distances[32]
                        ),
                        -1.0,
                        1.0,
                    ),
                    remap(
                        periodic_worley_fbm_from_distances(
                            distances[10], distances[20], distances[40]
                        ),
                        -1.0,
                        1.0,
                    ),
                    1.0,
                )
                for value in values:
                    voxels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
                    index += 1
    return bytes(voxels)


def generate_weather_texels(size: int = WEATHER_TEXTURE_SIZE) -> bytes:
    """Generate coverage, cloud-type, optical-depth, and base-height fields."""
    weather_values: list[tuple[float, float, float, float]] = []
    for y_index in range(size):
        y = (y_index + 0.5) / size
        for x_index in range(size):
            x = (x_index + 0.5) / size
            coverage_perlin = periodic_perlin_fbm(x, y, 0.0, 9, 7)
            coverage_worley = periodic_worley_two_octaves(
                periodic_worley_distance(x, y, 0.0, 9, 1.0),
                periodic_worley_distance(x, y, 0.0, 18, 1.0),
            )
            coverage_difference = periodic_perlin_fbm(x, y, 200.0, 9, 7)
            coverage = dilate_perlin_worley(
                remap(coverage_perlin, 1.0, 1.25),
                remap(coverage_worley, 0.65, 0.9),
                1.0,
            )
            coverage -= remap(coverage_difference, 1.0, 1.4)
            base_height = (
                gradient_noise(x, y, 0.419, 4) * 0.55
                + gradient_noise(x, y, 0.419, 8) * 0.3
                + gradient_noise(x, y, 0.419, 16) * 0.15
            )
            cloud_type = periodic_perlin_fbm(x, y, 500.0, 9, 7)
            optical_depth = periodic_perlin_fbm(x, y, 100.0, 6, 7)
            weather_values.append((
                coverage,
                remap(cloud_type, 0.85, 1.9),
                remap(optical_depth, 0.5, 1.3),
                remap(base_height, 0.3, 0.7),
            ))

    texels = bytearray(size * size * 4)
    index = 0
    for coverage, cloud_type, optical_depth, base_height in weather_values:
        for value in (
            coverage,
            cloud_type,
            optical_depth,
            base_height,
        ):
            texels[index] = round(min(max(value, 0.0), 1.0) * 255.0)
            index += 1
    return bytes(texels)


def perlin_five_octaves(x: float, y: float, z: float) -> float:
    """Accumulate the five non-tileable Perlin octaves used by the curl field."""
    value = 0.0
    amplitude = 1.0
    for scale in (1.0, 2.02, 2.02 * 2.03, 2.02 * 2.03 * 2.01, 2.02 * 2.03 * 2.01 * 2.01):
        value += amplitude * gradient_noise(x * scale, y * scale, z * scale, 1, tile=False)
        amplitude *= 0.5
    return value


def curl_noise(x: float, y: float, z: float) -> tuple[float, float, float]:
    """Evaluate the finite-difference curl of the five-octave Perlin field."""
    epsilon = 0.05

    def noise(sample_x: float, sample_y: float, sample_z: float) -> float:
        return perlin_five_octaves(sample_x, sample_y, sample_z)

    derivative_y = (noise(x, y + epsilon, z) - noise(x, y - epsilon, z)) / (2.0 * epsilon)
    derivative_z = (noise(x, y, z + epsilon) - noise(x, y, z - epsilon)) / (2.0 * epsilon)
    curl_x = derivative_y - derivative_z

    derivative_z = (noise(x, y, z + epsilon) - noise(x, y, z - epsilon)) / (2.0 * epsilon)
    derivative_x = (noise(x + epsilon, y, z) - noise(x - epsilon, y, z)) / (2.0 * epsilon)
    curl_y = derivative_z - derivative_x

    derivative_x = (noise(x + epsilon, y, z) - noise(x - epsilon, y, z)) / (2.0 * epsilon)
    derivative_y = (noise(x, y + epsilon, z) - noise(x, y - epsilon, z)) / (2.0 * epsilon)
    curl_z = derivative_x - derivative_y
    return curl_x, curl_y, curl_z


def generate_curl_texels(size: int = CURL_TEXTURE_SIZE) -> bytes:
    """Generate the encoded two-dimensional curl field consumed by cloud-detail erosion."""
    texels = bytearray(size * size * 4)
    index = 0
    for y_index in range(size):
        y = y_index / size * 4.0
        for x_index in range(size):
            x = x_index / size * 4.0
            curl = curl_noise(x, y, 0.0)
            for value in curl:
                remapped = (value + 0.5) / 3.5
                encoded = (remapped + 1.0) * 0.5
                texels[index] = round(min(max(encoded, 0.0), 1.0) * 255.0)
                index += 1
            texels[index] = 255
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
    """Write or verify the canonical cloud weather, base, detail, and curl resources."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_output", type=Path)
    parser.add_argument("detail_output", type=Path)
    parser.add_argument("weather_output", type=Path)
    parser.add_argument("curl_output", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    outputs = (
        (arguments.base_output, dds_volume(generate_base_voxels(), BASE_TEXTURE_SIZE)),
        (arguments.detail_output, dds_volume(generate_detail_voxels(), DETAIL_TEXTURE_SIZE)),
        (arguments.weather_output, dds_texture(generate_weather_texels(), WEATHER_TEXTURE_SIZE)),
        (arguments.curl_output, dds_texture(generate_curl_texels(), CURL_TEXTURE_SIZE)),
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
