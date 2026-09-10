"""Verify that generated cloud volumes remain complete OpenGL sampling resources."""

import struct
import unittest

import generate_atmosphere_cloud_noise as noise


class CloudVolumeEncodingTest(unittest.TestCase):
    """Exercise the resource encoder with the production shape and detail dimensions."""

    def test_mipmapped_volumes_include_the_one_texel_level(self):
        """Require every level through 1x1x1 for mipmapped texture completeness."""
        for size in (noise.BASE_TEXTURE_SIZE, noise.DETAIL_TEXTURE_SIZE):
            with self.subTest(size=size):
                texels = bytes((27, 94, 161, 228)) * size**3
                encoded = noise.dds_volume(texels, size)
                header = struct.unpack("<31I", encoded[4:128])
                expected_levels = size.bit_length()
                self.assertEqual(header[6], expected_levels)
                expected_texels = sum((size >> level) ** 3 for level in range(expected_levels))
                self.assertEqual(len(encoded), 128 + expected_texels * 4)
                self.assertEqual(encoded[-4:], texels[:4])


if __name__ == "__main__":
    unittest.main()
