Independent Python ABI fixture
==============================

This optional maintenance fixture packages the unchanged user-supplied
cpython-3.14.7+20260901-x86_64-unknown-linux-musl-lto+static-full.tar.zst.
SHA256: e5a76e5893c39c89ed268ad71c3d6794c3be6b7236ec613449140c57686fc17f.
Original licenses and PYTHON.json are preserved under /usr/share/licenses/python.
No interpreter or standard-library source is patched.

The payload lives under /opt/python; relative command aliases select the
original interpreter. This fixture is not a normal image or SDK input and
does not certify every Python module against ReliefOS. Use apk for normal
system package installation. See Documentation/kbuild/third-party.rst.
