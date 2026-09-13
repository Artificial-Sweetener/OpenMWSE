"""Verify terrain publication parity through serialized production attachment bundles."""

import struct
import tempfile
import unittest
from pathlib import Path

import numpy

from compare_diagnostic_graphics import MAGIC, VERSION, compare_directories


class TerrainAttachmentParityTest(unittest.TestCase):
    """Require identical finite terrain fields before comparing rendered clouds."""

    def setUp(self):
        """Isolate matching production stages and a complete packed terrain publication."""
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.reference = Path(self.workspace.name) / "reference"
        self.replay = Path(self.workspace.name) / "replay"
        self.reference.mkdir()
        self.replay.mkdir()
        self.terrain = numpy.zeros((1, 59, 4), dtype="<f4")
        self.terrain[0, 0] = [-163840, -163840, 163840, 15]
        self.terrain[0, 1] = [0, 2700, 15, 0]
        self.write_bundle(self.reference, self.terrain)

    def write_bundle(self, directory, terrain):
        """Serialize the engine's terrain attachment without bypassing the bundle reader."""
        label = b"atmosphere-cloud-terrain-v1"
        with (directory / "atmosphere-normal-frame-1-1.omwdiag").open("wb") as stream:
            stream.write(MAGIC)
            stream.write(struct.pack("<IQI", VERSION, 1, 1))
            stream.write(struct.pack("<I", len(label)))
            stream.write(label)
            stream.write(struct.pack("<IIIQ", 59, 1, 4, terrain.size))
            stream.write(terrain.tobytes())

    def test_matching_terrain_passes(self):
        """Accept identical terrain origins, bounds, and interpolation nodes."""
        self.write_bundle(self.replay, self.terrain)
        self.assertTrue(compare_directories(self.reference, self.replay)["passed"])

    def test_changed_terrain_node_fails(self):
        """Reject a single changed height even when most of the terrain array is identical."""
        changed = self.terrain.copy()
        changed[0, 30, 2] = 1
        self.write_bundle(self.replay, changed)
        report = compare_directories(self.reference, self.replay)
        self.assertFalse(report["passed"])
        result = report["stages"]["normal"]["atmosphere-cloud-terrain-v1"]
        self.assertEqual(result["nonzero_value_count"], 1)
        self.assertEqual(result["maximum_absolute"], 1)

    def test_nonfinite_terrain_is_rejected(self):
        """Reject invalid terrain data before computing numerical parity."""
        invalid = self.terrain.copy()
        invalid[0, 30, 2] = numpy.nan
        self.write_bundle(self.replay, invalid)
        with self.assertRaisesRegex(ValueError, "non-finite pixels"):
            compare_directories(self.reference, self.replay)


class RouteFrameParityTest(unittest.TestCase):
    """Compare route-local frame identities without depending on engine-global frame numbers."""

    def setUp(self):
        """Create isolated reference and replay route bundles."""
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        root = Path(self.workspace.name)
        self.reference = root / "reference.omwdiag"
        self.replay = root / "replay.omwdiag"
        self.color = numpy.zeros((2, 3, 4), dtype="<f4")
        self.depth = numpy.ones((2, 3, 1), dtype="<f4")

    def write_bundle(self, path, frame_number, color, depth):
        """Serialize one final-color image and one shadow cascade using real route attachment labels."""
        attachments = {
            f"route-frame-0000-osg-{frame_number}": color,
            f"route-shadow-frame-0000-shadow-map-depth-cascade-0-{frame_number}": depth,
        }
        with path.open("wb") as stream:
            stream.write(MAGIC)
            stream.write(struct.pack("<IQI", VERSION, frame_number, len(attachments)))
            for label, pixels in attachments.items():
                encoded = label.encode("utf-8")
                height, width, channels = pixels.shape
                stream.write(struct.pack("<I", len(encoded)))
                stream.write(encoded)
                stream.write(struct.pack("<IIIQ", width, height, channels, pixels.size))
                stream.write(pixels.tobytes())

    def test_matching_route_frames_ignore_engine_frame_numbers(self):
        """Accept identical route-local images captured at different engine-global frame numbers."""
        self.write_bundle(self.reference, 100, self.color, self.depth)
        self.write_bundle(self.replay, 900, self.color, self.depth)
        self.assertTrue(compare_directories(self.reference, self.replay)["passed"])

    def test_changed_route_color_fails(self):
        """Reject a material final-color regression in a corresponding route-local frame."""
        changed = self.color.copy()
        changed[0, 0, 0] = 1
        self.write_bundle(self.reference, 100, self.color, self.depth)
        self.write_bundle(self.replay, 900, changed, self.depth)
        report = compare_directories(self.reference, self.replay)
        self.assertFalse(report["passed"])
        result = report["stages"]["route"]["frame-0000-final-color"]
        self.assertEqual(result["nonzero_value_count"], 1)
        self.assertEqual(result["maximum_absolute"], 1)

    def test_changed_route_shadow_is_observational(self):
        """Report raw cascade differences without failing when final rendered color remains within tolerance."""
        changed = self.depth.copy()
        changed[0, 0, 0] = 0
        self.write_bundle(self.reference, 100, self.color, self.depth)
        self.write_bundle(self.replay, 900, self.color, changed)
        report = compare_directories(self.reference, self.replay)
        self.assertTrue(report["passed"])
        result = report["stages"]["route"]["frame-0000-shadow-depth-cascade-0"]
        self.assertFalse(result["enforced"])
        self.assertEqual(result["nonzero_value_count"], 1)


if __name__ == "__main__":
    unittest.main()
