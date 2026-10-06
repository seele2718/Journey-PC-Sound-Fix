"""PE-layout regression tests; standard Python, no game files required."""
from pathlib import Path
import struct
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE / "sound_runtime"))
import install
from build_pc_reverb_static_payload import BuildError, PeImage


def fixture(virtual_size=0x2000, second_rva=0x3000, image_size=0x4000):
    data = bytearray(0x800)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 2, 0, 0, 0, 0xf0, 0x23)
    opt = 0x98
    struct.pack_into('<H', data, opt, 0x20b)
    struct.pack_into('<Q', data, opt+24, 0x140000000)
    struct.pack_into('<II', data, opt+32, 0x1000, 0x200)
    struct.pack_into('<II', data, opt+56, image_size, 0x400)
    struct.pack_into('<I', data, opt+108, 16)
    struct.pack_into('<II', data, opt+112+24, 0x1000, 12)
    for index, (name, vs, rva, raw, flags) in enumerate([
        (b'.code', virtual_size, 0x1000, 0x400, 0x60000020),
        (b'.state', 0x80, second_rva, 0x600, 0xc0000040),
    ]):
        struct.pack_into('<8sIIIIIIHHI', data, opt+0xf0+index*40,
                         name, vs, rva, 0x200, raw, 0, 0, 0, 0, flags)
    return bytes(data)


class ImageLayoutTests(unittest.TestCase):
    def test_reserved_zero_fill_accepted(self):
        data = fixture()
        PeImage(data).validate_image_layout()
        self.assertEqual(install.parse_pe(data)['sectionCount'], 2)

    def test_gap_rejected(self):
        data = fixture(virtual_size=0x200)
        with self.assertRaises(BuildError): PeImage(data).validate_image_layout()
        with self.assertRaises(install.InstallError): install.parse_pe(data)

    def test_overlap_rejected(self):
        data = fixture(virtual_size=0x3000)
        with self.assertRaises(BuildError): PeImage(data).validate_image_layout()
        with self.assertRaises(install.InstallError): install.parse_pe(data)

    def test_image_extent_rejected(self):
        data = fixture(image_size=0x5000)
        with self.assertRaises(BuildError): PeImage(data).validate_image_layout()
        with self.assertRaises(install.InstallError): install.parse_pe(data)

    def test_unaligned_section_rejected(self):
        data = fixture(second_rva=0x3001)
        with self.assertRaises(BuildError): PeImage(data).validate_image_layout()
        with self.assertRaises(install.InstallError): install.parse_pe(data)


if __name__ == '__main__':
    unittest.main()
