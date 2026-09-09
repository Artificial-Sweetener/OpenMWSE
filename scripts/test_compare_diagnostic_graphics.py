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


if __name__ == "__main__":
    unittest.main()
