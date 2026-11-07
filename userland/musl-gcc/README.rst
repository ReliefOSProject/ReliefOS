Independent static musl toolchain fixture
========================================

This optional ABI fixture uses the unchanged Dyne musl 2.2.0 x86-64
binary distribution, with GCC 15.1.0, binutils and musl libraries.
Archive SHA256: 31420e4f978e7ccbcc597ca5e18c2dcbe640ea6985ed6325112d8a171a72ece3.
Host executables require x86-64-v2. Original contents live under /opt/dyne.
It does not replace the ReliefOS SDK or add itself to normal images.

The upstream sources and recipes are at https://github.com/dyne/musl/tree/2.2.0.
GCC/binutils are GPL software; designated runtime libraries use the GCC
Runtime Library Exception. Musl uses MIT. All bundled notices retain their
original terms; package.json records upstream file hashes. COPYING3,
COPYING.RUNTIME and musl-COPYRIGHT are shipped with this notice.

For normal development consume the configured SDK described in
Documentation/kbuild/sdk.rst, or install tools with apk inside ReliefOS.
