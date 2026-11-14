========
ReliefOS
========

`简体中文 <README.rst>`_ | English | `日本語 <README.ja.rst>`_

ReliefOS is an operating system project for x86_64. Its kernel lives in the
kernel/reliefnt submodule. The default desktop uses Xorg, XDM and IceWM; the
system supports native applications with Chinese text, console-only
installation, and BIOS/UEFI boot. The current source manual starts at
`Documentation/index.rst <Documentation/index.rst>`_.

The build entry point is GNU Make 4.3+ at the repository root. Project sources
are compiled with a pinned Linux Kbuild/Kconfig. Initialize the submodules and
explicitly fetch locked dependencies before building; see the
`build manual <Documentation/kbuild/building.rst>`_ for the full procedure.
Build the HTML manuals with::

    make O=out/docs htmldocs

This generates Simplified Chinese, English and Japanese manuals. Existing
Chinese URLs remain the default; English is under en/ and Japanese under ja/
in the output. Every page links to the same page in the other languages.

Compilation, image generation and actual runtime behavior are verified
separately. See the `supported scope <Documentation/admin-guide/limitations.rst>`_
for known limitations. VMware validation is incomplete.
Read AGENT.md before contributing. See LICENSE for the project license;
upstream licenses and attribution retain their original text.
Thanks to VasilyZa for contributions to Linux ABI compatibility, musl and
related work.

The ReliefNT kernel manual is maintained independently in
kernel/reliefnt/Documentation/. The complete Pages/RPR site publishes separate
manuals at docs/reliefos/ and docs/reliefnt/.
