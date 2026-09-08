#!/usr/bin/env python3
"""Generate cloud noise resources and OpenMW's temporal sampling tile.

The offline scalar evaluator uses Python binary64 arithmetic, so its hash realization is
independent of GPU floating-point contraction and transcendental implementations.
The Perlin gradient hash is credited to Brian Sharpe, GPU-Noise-Lib; see
files/data/licenses/GPU-Noise-Lib.txt for the original permissive notice.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import heapq
import hashlib
import math
import os
import random
import struct
from pathlib import Path


BASE_TEXTURE_SIZE = 64
DETAIL_TEXTURE_SIZE = 32
WEATHER_TEXTURE_SIZE = 1024
CURL_TEXTURE_SIZE = 128
BLUE_NOISE_TEXTURE_SIZE = 128
CLOUD_VOLUME_MIP_COUNT = 6
RANDOM_SEED = 0x4F4D5753

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
    x: float, y: float, z: float, frequency: float, seed: float = 0.0
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
        grid_x = math.fmod(grid_x, frequency)
        grid_y = math.fmod(grid_y, frequency)
        grid_z = math.fmod(grid_z, frequency)
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


def generate_base_slice(arguments: tuple[int, int]) -> bytes:
    """Generate one deterministic z slice of the Perlin-Worley base volume."""
    z_index, size = arguments
    voxels = bytearray(size * size * 4)
    index = 0
    z = z_index / size
    for y_index in range(size):
        y = y_index / size
        for x_index in range(size):
            x = x_index / size
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
        z = z_index / size
        for y_index in range(size):
            y = y_index / size
            for x_index in range(size):
                x = x_index / size
                distances = {
                    frequency: periodic_worley_distance(x, y, z, frequency)
                    for frequency in (5, 7.5, 10, 15, 20, 30, 40)
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
                            distances[7.5], distances[15], distances[30]
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


def generate_weather_row(arguments: tuple[int, int]) -> bytes:
    """Evaluate one row of the source coverage, type, rain, and constant-alpha weather map."""
    y_index, size = arguments
    y = y_index / size
    texels = bytearray(size * 4)
    for x_index in range(size):
        x = x_index / size
        perlin_coverage = remap(periodic_perlin_fbm(x, y, 0.0, 9, 7), 1.0, 1.25)
        worley_coverage = remap(periodic_worley_two_octaves(
            periodic_worley_distance(x, y, 0.0, 9, 1.0),
            periodic_worley_distance(x, y, 0.0, 18, 1.0)), 0.65, 0.9)
        subtractive_coverage = remap(periodic_perlin_fbm(x, y, 200.0, 9, 7), 1.0, 1.4)
        coverage = dilate_perlin_worley(perlin_coverage, worley_coverage, 1.0) - subtractive_coverage
        cloud_type = remap(periodic_perlin_fbm(x, y, 500.0, 9, 7), 0.85, 1.9)
        rain = remap(periodic_perlin_fbm(x, y, 100.0, 6, 7), 0.5, 1.3)
        for channel, value in enumerate((coverage, cloud_type, rain, 1.0)):
            texels[x_index * 4 + channel] = round(min(max(value, 0.0), 1.0) * 255.0)
    return bytes(texels)


def generate_weather_texels(size: int = WEATHER_TEXTURE_SIZE) -> bytes:
    """Generate the source-sized weather map in deterministic row order."""
    arguments = [(row, size) for row in range(size)]
    with concurrent.futures.ProcessPoolExecutor(max_workers=min(os.cpu_count() or 1, 16)) as executor:
        return b"".join(executor.map(generate_weather_row, arguments))


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


def generate_blue_noise_texels(size: int = BLUE_NOISE_TEXTURE_SIZE) -> bytes:
    """Generate a deterministic void-and-cluster threshold tile for ray-march sampling."""
    pixel_count = size * size
    occupied = [False] * pixel_count
    indices = list(range(pixel_count))
    random.Random(RANDOM_SEED ^ 0x424C5545).shuffle(indices)
    for index in indices[: pixel_count // 2]:
        occupied[index] = True

    radius = 5
    sigma = 1.5
    kernel = [
        (offset_x, offset_y, math.exp(-(offset_x * offset_x + offset_y * offset_y) / (2.0 * sigma * sigma)))
        for offset_y in range(-radius, radius + 1)
        for offset_x in range(-radius, radius + 1)
    ]

    def energy_field(pattern: list[bool]) -> list[float]:
        """Return periodic Gaussian occupancy energy for one binary pattern."""
        energy = [0.0] * pixel_count
        for source, present in enumerate(pattern):
            if not present:
                continue
            source_x = source % size
            source_y = source // size
            for offset_x, offset_y, weight in kernel:
                target = ((source_y + offset_y) % size) * size + (source_x + offset_x) % size
                energy[target] += weight
        return energy

    def ranked_pattern(pattern: list[bool]) -> list[int]:
        """Rank clusters below half density and voids above it using incremental periodic energy."""
        energy = energy_field(pattern)
        versions = [0] * pixel_count
        dense_heap: list[tuple[float, int, int]] = []
        void_heap: list[tuple[float, int, int]] = []

        def push(index: int) -> None:
            entry = (energy[index], versions[index], index)
            if pattern[index]:
                heapq.heappush(dense_heap, (-entry[0], entry[1], entry[2]))
            else:
                heapq.heappush(void_heap, entry)

        def rebuild_heaps() -> None:
            dense_heap.clear()
            void_heap.clear()
            for index in range(pixel_count):
                push(index)

        def update(source: int, amount: float) -> None:
            source_x = source % size
            source_y = source // size
            for offset_x, offset_y, weight in kernel:
                target = ((source_y + offset_y) % size) * size + (source_x + offset_x) % size
                energy[target] += amount * weight
                versions[target] += 1
                push(target)

        def densest() -> int:
            while True:
                negative_energy, version, index = heapq.heappop(dense_heap)
                if pattern[index] and version == versions[index] and -negative_energy == energy[index]:
                    return index

        def largest_void() -> int:
            while True:
                candidate_energy, version, index = heapq.heappop(void_heap)
                if not pattern[index] and version == versions[index] and candidate_energy == energy[index]:
                    return index

        rebuild_heaps()
        for _ in range(pixel_count * 4):
            cluster = densest()
            pattern[cluster] = False
            update(cluster, -1.0)
            void = largest_void()
            pattern[void] = True
            update(void, 1.0)
            if cluster == void:
                break

        optimized = pattern.copy()
        ranks = [-1] * pixel_count
        rebuild_heaps()
        for rank in range(pixel_count // 2 - 1, -1, -1):
            cluster = densest()
            ranks[cluster] = rank
            pattern[cluster] = False
            update(cluster, -1.0)

        pattern[:] = optimized
        energy[:] = energy_field(pattern)
        versions[:] = [0] * pixel_count
        rebuild_heaps()
        for rank in range(pixel_count // 2, pixel_count):
            void = largest_void()
            ranks[void] = rank
            pattern[void] = True
            update(void, 1.0)
        return ranks

    ranks = ranked_pattern(occupied)
    texels = bytearray(pixel_count * 4)
    for index, rank in enumerate(ranks):
        value = round(rank / max(pixel_count - 1, 1) * 255.0)
        texels[index * 4 : index * 4 + 4] = bytes((value, value, value, 255))
    return bytes(texels)


def downsample_volume_rgba8(voxels: bytes, size: int) -> bytes:
    """Box-filter one periodic RGBA8 volume mip to half resolution in all three axes."""
    next_size = max(size // 2, 1)
    next_voxels = bytearray(next_size * next_size * next_size * 4)
    output_index = 0
    for z_index in range(next_size):
        for y_index in range(next_size):
            for x_index in range(next_size):
                channel_sums = [0, 0, 0, 0]
                for z_offset in range(2 if size > 1 else 1):
                    source_z = min(z_index * 2 + z_offset, size - 1)
                    for y_offset in range(2 if size > 1 else 1):
                        source_y = min(y_index * 2 + y_offset, size - 1)
                        for x_offset in range(2 if size > 1 else 1):
                            source_x = min(x_index * 2 + x_offset, size - 1)
                            source_index = ((source_z * size + source_y) * size + source_x) * 4
                            for channel in range(4):
                                channel_sums[channel] += voxels[source_index + channel]
                sample_count = 8 if size > 1 else 1
                for channel_sum in channel_sums:
                    next_voxels[output_index] = (channel_sum + sample_count // 2) // sample_count
                    output_index += 1
    return bytes(next_voxels)


def volume_mip_chain_rgba8(voxels: bytes, size: int, mip_count: int) -> bytes:
    """Encode consecutively filtered volume levels in DDS subresource order."""
    levels = [voxels]
    level_size = size
    while len(levels) < mip_count:
        levels.append(downsample_volume_rgba8(levels[-1], level_size))
        level_size = max(level_size // 2, 1)
    return b"".join(levels)


def dds_volume(voxels: bytes, size: int, mip_count: int = CLOUD_VOLUME_MIP_COUNT) -> bytes:
    """Encode an uncompressed RGBA8 volume with the mip chain required by explicit LOD sampling."""
    mip_chain = volume_mip_chain_rgba8(voxels, size, mip_count)
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
        0x0082100F,
        size,
        size,
        size * 4,
        size,
        mip_count,
        *([0] * 11),
    )
    caps = struct.pack("<5I", 0x401008, 0x00200000, 0, 0, 0)
    return b"DDS " + header + pixel_format + caps + mip_chain


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
    """Write or verify the canonical cloud density, weather, curl, and sampling-noise resources."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_output", type=Path)
    parser.add_argument("detail_output", type=Path)
    parser.add_argument("weather_output", type=Path)
    parser.add_argument("curl_output", type=Path)
    parser.add_argument("blue_noise_output", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    outputs = (
        (arguments.base_output, dds_volume(generate_base_voxels(), BASE_TEXTURE_SIZE)),
        (arguments.detail_output, dds_volume(generate_detail_voxels(), DETAIL_TEXTURE_SIZE)),
        (arguments.weather_output, dds_texture(generate_weather_texels(), WEATHER_TEXTURE_SIZE)),
        (arguments.curl_output, dds_texture(generate_curl_texels(), CURL_TEXTURE_SIZE)),
        (arguments.blue_noise_output, dds_texture(generate_blue_noise_texels(), BLUE_NOISE_TEXTURE_SIZE)),
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
