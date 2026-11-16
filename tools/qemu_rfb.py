# SPDX-License-Identifier: Apache-2.0
"""Minimal RFC 6143 client for real QEMU display captures over a private socket."""

import struct
import time


class QemuRfb:
    """Read Raw framebuffer updates, including mode changes, without GUI packages."""

    def __init__(self, stream, deadline):
        self.stream = stream
        self.deadline = deadline
        if self.read(12) != b"RFB 003.008\n":
            raise RuntimeError("QEMU did not offer RFB 3.8")
        stream.sendall(b"RFB 003.008\n")
        security = self.read(self.read(1)[0])
        if 1 not in security:
            raise RuntimeError("private QEMU VNC socket did not offer None security")
        stream.sendall(b"\x01")
        if self.read(4) != bytes(4):
            raise RuntimeError("QEMU RFB security negotiation failed")
        stream.sendall(b"\x01")  # Shared ClientInit; never disconnect another viewer.
        initial = self.read(24)
        self.width, self.height = struct.unpack(">HH", initial[:4])
        name_size = struct.unpack(">I", initial[20:])[0]
        if name_size > 1024 * 1024:
            raise RuntimeError("oversized RFB desktop name")
        self.read(name_size)
        # Big-endian XRGB8888. The unused first byte is discarded for PPM.
        pixel_format = struct.pack(">BBBBHHHBBBxxx", 32, 24, 1, 1, 255, 255, 255, 16, 8, 0)
        stream.sendall(bytes(4) + pixel_format)
        stream.sendall(struct.pack(">BBHii", 2, 0, 2, 0, -223))  # Raw, DesktopSize.
        self.resize(self.width, self.height)

    def read(self, size):
        """Receive a bounded protocol field before the caller's absolute deadline."""
        data = bytearray()
        while len(data) < size:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("QEMU RFB deadline")
            self.stream.settimeout(min(remaining, 30))
            part = self.stream.recv(size - len(data))
            if not part:
                raise ConnectionError("QEMU closed its RFB connection")
            data.extend(part)
        return data

    def resize(self, width, height):
        """Discard incomplete old-mode pixels on a DesktopSize notification."""
        if not 0 < width <= 8192 or not 0 < height <= 8192:
            raise RuntimeError(f"invalid RFB size {width}x{height}")
        self.width, self.height = width, height
        self.pixels = bytearray(width * height * 3)
        self.covered = bytearray(width * height)

    def request(self):
        """Request the entire current framebuffer, independently of prior damage."""
        self.stream.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, self.width, self.height))

    def capture(self, destination):
        """Write an actual complete framebuffer, never a reconstructed test image."""
        self.covered[:] = bytes(len(self.covered))
        self.request()
        while True:
            message = self.read(1)[0]
            if message == 2:  # Bell.
                continue
            if message == 3:  # ServerCutText may arrive independently of updates.
                size = struct.unpack(">I", self.read(7)[3:])[0]
                if size > 1024 * 1024:
                    raise RuntimeError("oversized RFB clipboard")
                self.read(size)
                continue
            if message != 0:
                raise RuntimeError(f"unexpected RFB server message {message}")
            rectangles = struct.unpack(">H", self.read(3)[1:])[0]
            resized = False
            for _ in range(rectangles):
                x, y, width, height, encoding = struct.unpack(">HHHHi", self.read(12))
                if encoding == -223:
                    self.resize(width, height)
                    resized = True
                    continue
                if encoding != 0 or x + width > self.width or y + height > self.height:
                    raise RuntimeError("invalid or unrequested RFB rectangle encoding")
                raw = self.read(width * height * 4)
                rgb = bytearray(width * height * 3)
                rgb[0::3], rgb[1::3], rgb[2::3] = raw[1::4], raw[2::4], raw[3::4]
                for row in range(height):
                    start = (y + row) * self.width + x
                    self.pixels[start * 3:(start + width) * 3] = rgb[row * width * 3:(row + 1) * width * 3]
                    self.covered[start:start + width] = b"\x01" * width
            if resized:
                self.request()
            elif self.covered and 0 not in self.covered:
                destination.write_bytes(f"P6\n{self.width} {self.height}\n255\n".encode() + self.pixels)
                return self.width, self.height
