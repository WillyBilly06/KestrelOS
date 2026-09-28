"""Offline byte-parser checks; no codec execution or hardware access."""
import unittest
from decode_nvenc_rc_log import DecodeError, decode_register_snapshot


class RegisterDecodeTests(unittest.TestCase):
    def test_captured_packed_values(self):
        r = decode_register_snapshot(bytes.fromhex('08011080cc82022205c21f000000'))
        self.assertEqual(r['memory_type'], 1)
        self.assertEqual(r['stride'], 4)
        self.assertEqual(r['values'], [
            {'offset': '0x40a600', 'value': '0xfc2'},
            {'offset': '0x40a604', 'value': '0x0'},
            {'offset': '0x40a608', 'value': '0x0'},
            {'offset': '0x40a60c', 'value': '0x0'}])

    def test_unpacked_and_explicit_stride(self):
        r = decode_register_snapshot(bytes.fromhex('0801108010180820012002'))
        self.assertEqual(r['values'], [{'offset': '0x800', 'value': '0x1'},
                                       {'offset': '0x808', 'value': '0x2'}])

    def test_defaults(self):
        r = decode_register_snapshot(bytes.fromhex('08012200'))
        self.assertEqual((r['offset'], r['stride'], r['values']), ('0x0', 4, []))

    def test_reject_malformed(self):
        invalid = (
            '',                        # required type absent
            '08010801',                # duplicate metadata
            '08011200',                # offset has wrong wire type
            '0801220180',              # truncated packed varint
            '0801208080808010',        # uint32 overflow
            '080122058080808010',      # packed uint32 overflow
            '08012800',                # unknown field
            '080110ffffffffffffffffff0120012002',  # uint64 address overflow
        )
        for hexdata in invalid:
            with self.subTest(hexdata=hexdata), self.assertRaises(DecodeError):
                decode_register_snapshot(bytes.fromhex(hexdata))


if __name__ == '__main__':
    unittest.main()
