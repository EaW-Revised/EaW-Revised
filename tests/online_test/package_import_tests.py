"""The package import check must catch missing app-local dependencies, including delay imports."""
import pathlib
import runpy
import struct
import unittest
from pe_imports import pe_imports, verify_pe_imports


def image(import_name, delay=False, pe32=False):
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    opt = 152
    opt_size = 224 if pe32 else 240
    struct.pack_into('<H', data, 134, 1)
    struct.pack_into('<H', data, 148, opt_size)
    struct.pack_into('<H', data, opt, 0x10b if pe32 else 0x20b)
    struct.pack_into('<I', data, opt + 60, 512)
    dirs = opt + (96 if pe32 else 112)
    struct.pack_into('<I', data, dirs - 4, 16)
    struct.pack_into('<IIII', data, opt + opt_size + 8, 1536, 4096, 1536, 512)
    index = 13 if delay else 1
    desc_size = 32 if delay else 20
    struct.pack_into('<II', data, dirs + index * 8, 4096, desc_size * 2)
    struct.pack_into('<I', data, 512 + (4 if delay else 12), 4352)
    if delay:
        struct.pack_into('<I', data, 512, 1)
    encoded = import_name.encode() + b'\0'
    data[768:768 + len(encoded)] = encoded
    return bytes(data)


class ConfigTests(unittest.TestCase):
    def test_packaging_address_parity(self):
        module = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[2] / 'apps/online_test/validate_package_config.py'))
        valid = module['valid_address']
        validate = module['validate']
        for address in ('bad host', '-bad.example', 'bad-.example', 'a..b', '999.1.2.3',
                        'host:27020', 'bad/host', 'bad\nhost', '127.0.0.1\0evil', 'a' * 64 + '.org', 'a' * 254):
            with self.subTest(address=repr(address)):
                self.assertFalse(valid(address))
                self.assertTrue(validate(address, '27020', 'pin:' + 'a' * 64))
        for address in ('localhost', 'test.example.org', '127.0.0.1', '::1', '2001:db8::1'):
            self.assertTrue(valid(address))
            self.assertEqual(validate(address, '27020', 'pin:' + 'a' * 64), '')
        self.assertEqual(validate('test.example.org', '27020', 'system-ca'), '')
        self.assertTrue(validate('127.0.0.1', '27020', 'system-ca'))
        self.assertTrue(validate('::1', '27020', 'system-ca'))
        self.assertTrue(validate('play.example.org', '27020', 'system-ca'))


class ImportTests(unittest.TestCase):
    def test_non_system_runtime_must_ship(self):
        for delay in (False, True):
            for pe32 in (False, True):
                with self.subTest(delay=delay, pe32=pe32):
                    exe = image('VCRUNTIME140.dll', delay, pe32)
                    self.assertEqual(pe_imports(exe), {'vcruntime140.dll'})
                    with self.assertRaisesRegex(AssertionError, 'not in the zip'):
                        verify_pe_imports({'kit.exe': exe})
                    receipt = verify_pe_imports({'kit.exe': exe, 'vcruntime140.dll': image('KERNEL32.dll')})
                    self.assertEqual(receipt['kit.exe'], ['vcruntime140.dll'])

    def test_transitive_dependency_is_checked(self):
        with self.assertRaisesRegex(AssertionError, 'MSVCP140.dll'):
            verify_pe_imports({'kit.exe': image('MSVCP140.dll'), 'MSVCP140.dll': image('VCRUNTIME140_1.dll')})

    def test_system_import_and_malformed_executable(self):
        verify_pe_imports({'kit.exe': image('api-ms-win-core-file-l1-1-0.dll')})
        with self.assertRaises(ValueError):
            pe_imports(b'not a PE')


if __name__ == '__main__':
    unittest.main()
