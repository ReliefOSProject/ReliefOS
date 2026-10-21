# Browser and litehtml Porting Notes

ReliefOS ships `browser.elf`, a classic IE-style browser shell. Upstream
litehtml is present as a Git submodule under `third_party/litehtml`; the browser
currently links a freestanding C `litehtml_core` adapter while the full C++
litehtml runtime prerequisites are still being built.

## Current Browser

`userland/apps/browser/main.c` is a freestanding C application that uses the
existing GUI, libc, launch, filesystem, and network ABIs.

Current features:

- IE-style menu/toolbar/address layout with toast status messages.
- Times New Roman as the browser's Latin font, with SimSun as the Chinese
  fallback; both are loaded at runtime from external font files rather than
  embedded in `browser.elf`.
- Bilingual `about:reliefos` start page. The generic `about:` route still accepts
  the old `about:leonos` URI for saved bookmarks.
- `http://` and `https://` navigation through the libc HTTP client on top of
  TCP sockets. HTTPS uses TLS 1.2 with certificate, hostname, and system-clock
  validation.
- Chrome-compatible desktop `User-Agent`, `Accept`, and language headers for
  sites that reject unknown clients.
- Local `.html` and `.htm` file loading.
- Download-link handoff: links ending in common binary/media/archive suffixes
  launch `downloadmgr.elf`, which saves the response to the current user's
  `Downloads` directory.
- A `userland/apps/browser/litehtml_core.c` document layout layer for headings,
  paragraphs and block sections, lists, blockquotes, table rows/cells, breaks,
  horizontal rules, image placeholders with alt text, entities, links, per-cell
  table alignment, and basic inline styles (`strong`/`b`, `em`/`i`, `code`).
- A small CSS v1 parser for `<style>` blocks, inline `style=""`, and up to four
  HTTP or HTTPS `<link rel="stylesheet">` stylesheets per page. It covers simple
  tag/class/id selectors plus `color`, `background-color`, `font-weight`,
  `font-style`, `text-decoration`, `text-align`, left indent properties, and
  basic borders.
- HTTP toast reporting, response content-type handling, bounded redirects,
  chunked transfer decoding, final URL reporting, and truncation indicators.
- Link hit testing, relative URL resolution, Back, Forward, Refresh, Home, and
  mouse-wheel scrolling.
- Inline form support: `form`, text/email/search/url/tel/number/password/hidden
  inputs, checkbox/radio controls, single-choice `select`/`option`, one-line
  `textarea`, placeholder text, submit/reset inputs and buttons,
  disabled/read-only states, GET and POST, and
  `application/x-www-form-urlencoded` bodies. Select controls cycle through
  options when clicked.
- Resize-aware reflow.

Current limits:

- No JavaScript.
- No inline image loading. Image links can be downloaded and opened with
  `imageview.elf` when saved as BMP or PNG.
- No full CSS cascade, box model, floats, flex/grid, or media queries.
- HTTP uses `HTTP/1.1` over TCP sockets with `Connection: close`; HTTPS uses a
  TLS 1.2 client profile backed by Mbed TLS and the bundled CA roots. Cookies,
  cache, compression, and true streaming downloads are not implemented yet.

The repository owns the font build inputs: `system/fonts/times.ttf` is copied
to `/usr/share/fonts/reliefos/times-new-roman.ttf`, and `system/fonts/simsun.ttc` is
packaged as `/usr/share/fonts/reliefos/simsun.ttc`. Local and GitHub Actions builds use
these same repository files.

## litehtml Status

Upstream litehtml is checked out as:

- submodule path: `third_party/litehtml`
- upstream URL: `https://github.com/litehtml/litehtml.git`
- current recorded commit: `b9e89f0b9494ff9a5f008800af35503efabddf59`

The current upstream tree is C++ and depends heavily on STL types and library
facilities such as strings, vectors, maps, smart pointers, variants, algorithms,
and bundled Gumbo parser support. ReliefOS userland is still freestanding C with a
small libc and no C++ runtime or STL build path.

Because of that, the browser app currently owns a small C core with a deliberately
narrow surface. The replacement point is the document layout layer: URL loading,
navigation history, window chrome, status reporting, and launch integration
should remain useful when the full C++ litehtml container becomes available.

## Next Porting Steps

To integrate real litehtml, do these in order:

1. Register renderer sources in `userland/apps/browser/Kbuild`. Native C++
   compilation is supplied by `scripts/Makefile.cxx`; application flags and
   component selection live in `scripts/reliefos/Makefile.userland`.
2. Provide a minimal C++ runtime surface for constructors, destructors,
   allocation, exceptions-disabled builds, and required ABI helpers.
3. Port or provide an STL subset/libc++ profile that satisfies litehtml.
4. Build Gumbo and upstream litehtml as userland libraries from
   `third_party/litehtml`.
5. Replace `litehtml_core.c` with a ReliefOS litehtml container that maps text measurement, drawing,
   colors, clipping, mouse hit testing, and file/network resource fetches to
   the existing GUI and network APIs.
6. Keep the browser shell, navigation, address bar, history, and app-launch
   integration stable while swapping the document engine.

## User Integration

The browser is registered as:

- `/usr/lib/reliefos/apps/browser/browser.elf`
- launch alias: `browser`
- default app for `.html` and `.htm`

Download and image companion apps are registered as:

- `/usr/lib/reliefos/apps/downloadmgr/downloadmgr.elf`, launch alias `downloadmgr`
- `/usr/lib/reliefos/apps/imageview/imageview.elf`, launch alias `imageview`
- default app for `.bmp`, `.dib`, and `.png`

The generated icon is `/usr/lib/reliefos/apps/browser/browser.bmp`, following the existing
same-directory/same-basename application icon convention.
