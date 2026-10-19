"""Application artwork and freedesktop installation contract (stdlib only)."""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('app_icons', ROOT / 'tools/make_app_icons.py')
icons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icons)
APPS = [line.strip() for line in (ROOT / 'configs/app-icons.list').read_text().splitlines()
        if line.strip() and not line.startswith('#')]
SIZES = (16, 32, 48, 64, 128, 256)


def decode_png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    at, compressed, dimensions = 8, bytearray(), None
    while at < len(data):
        length = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        content = data[at + 8:at + 8 + length]
        assert zlib.crc32(kind + content) == struct.unpack_from('>I', data, at + 8 + length)[0]
        if kind == b'IHDR':
            w, h, bits, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', content)
            assert (bits, color, compression, filtering, interlace) == (8, 6, 0, 0, 0)
            dimensions = (w, h)
        if kind == b'IDAT': compressed += content
        at += length + 12
        if kind == b'IEND': break
    assert at == len(data) and dimensions
    w, h = dimensions
    pixels = zlib.decompress(compressed)
    assert len(pixels) == h * (w * 4 + 1)
    assert all(pixels[y * (w * 4 + 1)] == 0 for y in range(h))
    rgba = b''.join(pixels[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h))
    return dimensions, rgba


class AppIcons(unittest.TestCase):
    def test_reliefmath_has_distinct_math_artwork(self):
        self.assertIn('reliefmath', APPS)
        pixels = icons.draw_icon('reliefmath')
        rgba = bytes(channel for row in pixels for pixel in row for channel in pixel)
        self.assertIn(0, rgba[3::4])
        self.assertIn(255, rgba[3::4])
        self.assertNotEqual(pixels, icons.draw_icon('reliefword'))

    def test_only_reliefos_graphical_applications_ship_artwork(self):
        directory = ROOT / 'resources/build-art/app-icons'
        self.assertEqual({p.stem for p in directory.glob('*.bmp')}, set(APPS))
        for size in SIZES:
            self.assertEqual({p.stem.removeprefix('reliefos-') for p in
                              (directory / f'hicolor/{size}x{size}/apps').glob('reliefos-*.png')}, set(APPS))

    def test_generator_rejects_console_and_third_party_apps(self):
        with tempfile.TemporaryDirectory() as directory:
            for app in ('sudo', 'fastfetch', 'nano', 'doom', 'shell', 'soundctl', 'stardusthello'):
                result = subprocess.run([sys.executable, str(ROOT / 'tools/make_app_icons.py'),
                                         '--out-dir', directory, '--apps', app],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, app)
            self.assertFalse(list(Path(directory).iterdir()))
            subprocess.run([sys.executable, str(ROOT / 'tools/make_app_icons.py'),
                            '--out-dir', directory], check=True)
            self.assertEqual({p.stem for p in Path(directory).glob('*.bmp')}, set(APPS))

    def test_every_graphical_app_has_distinct_transparent_artwork(self):
        seen = {}
        for app in APPS:
            with self.subTest(app=app):
                pixels = icons.draw_icon(app)
                rgba = bytes(channel for row in pixels for pixel in row for channel in pixel)
                self.assertTrue(rgba not in seen, f'{app} shares artwork with {seen.get(rgba)}')
                seen[rgba] = app
                self.assertIn(0, rgba[3::4])
                self.assertIn(255, rgba[3::4])

    def test_png_dimensions_alpha_and_pixel_colors(self):
        with tempfile.TemporaryDirectory() as directory:
            for size in SIZES:
                path = Path(directory) / f'{size}.png'
                pixels = icons.draw_icon('fileman')
                icons.write_png(path, pixels, size)
                dimensions, rgba = decode_png(path)
                self.assertEqual(dimensions, (size, size))
                self.assertEqual(rgba[:4], bytes(pixels[0][0]))
                middle = ((size // 2) * size + size // 2) * 4
                self.assertEqual(rgba[middle:middle + 4], bytes(pixels[8][8]))

    def test_shipped_theme_and_desktop_icons_resolve(self):
        for app in APPS:
            for size in SIZES:
                with self.subTest(app=app, size=size):
                    path = ROOT / f'resources/build-art/app-icons/hicolor/{size}x{size}/apps/reliefos-{app}.png'
                    dimensions, rgba = decode_png(path)
                    self.assertEqual(dimensions, (size, size))
                    self.assertIn(0, rgba[3::4])
                    if size == 32:
                        bmp = (ROOT / f'resources/build-art/app-icons/{app}.bmp').read_bytes()
                        self.assertEqual(struct.unpack_from('<ii', bmp, 18), (32, -32))
                        raw = bmp[54:]
                        converted = bytes(channel for at in range(0, len(raw), 4)
                                          for channel in (raw[at+2], raw[at+1], raw[at], raw[at+3]))
                        self.assertEqual(rgba, converted, f'{app}: menu and window artwork differ')
        for desktop in (ROOT / 'userland/apps').glob('*/*.desktop'):
            app = desktop.parent.name
            fields = dict(line.split('=', 1) for line in desktop.read_text().splitlines()
                          if '=' in line and not line.startswith('#'))
            if app in APPS:
                self.assertEqual(fields['Icon'], 'reliefos-' + app)
            else:
                self.assertFalse(fields.get('Icon', '').startswith('reliefos-'), app)


if __name__ == '__main__': unittest.main()
