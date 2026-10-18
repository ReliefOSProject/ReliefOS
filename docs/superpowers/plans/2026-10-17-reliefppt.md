# ReliefPPT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 内置可编辑、保存和放映中文幻灯片的 C++/FLTK ReliefPPT，并完成真实 ReliefOS 截图验收。

**Architecture:** rp 核心模型与几何独立于 GUI；原生文件和原子文件操作独立；编辑画布、缩略图、PNG 和放映共用一个 FLTK 渲染器。通过候选 Deck 提交保障操作失败不改变文档。

**Tech Stack:** C++17, existing FLTK 1.3, libpng, musl, gettext, Xorg; no exceptions/RTTI or new runtime dependencies.

**Spec:** docs/superpowers/specs/2026-10-17-reliefppt.md

## Global Constraints

- 已获用户“可以，开始吧”执行授权；直接完成批准范围内可逆实现。不得自行提交、推送或建分支。
- 使用当前 rewrite/desktop-apps，保留所有原有无关未跟踪文件。不改内核、主题、背景、ABI/audio。
- 子代理只用 GPT-6-Luna Max；不得再派子代理。实现子代理按任务顺序派发，任务完成后独立审查。
- 临时文件 build/reliefppt-dev，TMPDIR=out/tmp；不得提交 out/build/.zcode/result/tools/dependencies.py。
- 所有数值须有限、范围校验；文字有效 UTF-8；错误原子性；history revision 高水位；新文件清空旧历史。
- 上限 500 slides / 256 objects per slide / 20,000 objects total / 256 images / 64 MiB total RGBA / 4096×4096 per image / 32 KiB text / 64 KiB notes / 96 MiB native file。
- VMware 验证未完成。所有完成声明由实际构建、测试、运行证据支撑。

## Review Focus

1. 同一张片中重叠/多选/粘贴图片应保持图层和索引；测试模型 duplicate/paste/z-order 和原生回读。
2. 撤销后修改或 Save As 后继续修改仍提示未保存；测试修订号并在 QEMU 检查关闭提示。
3. 中文多字节和换行、连续空行在编辑/渲染/导出一致；测试布局和实际剪贴板输入。
4. NaN/Inf/截断长度/尺寸乘法溢出不能损坏现有演示；测试模型和解码器拒绝且候选对象不变。
5. 模态框、输入框、Esc 和放映键盘焦点不能误关应用或丢字；在 QEMU 以外部 X11 焦点探针驱动并读回精确文字。

---

### Task 1: Atomic model and geometry

**Files:** userland/apps/reliefppt/{storage.h,model.h,model.cpp,geometry.h,geometry.cpp}; tests/host/test_reliefppt_model.cpp.
**Interfaces:** Defined in build/reliefppt-dev/interfaces.md; implementer owns headers, must settle/emit stable headers first and report once available. Other tasks consume these without semantic changes.

- [x] Write model behavioral tests first, run and retain red output; cover add/duplicate/reorder/delete slides (last becomes blank), ID uniqueness, UTF-8, atomic rejection, move/resize, selected alignment, layer order, embedded image copy/paste remap, notes and whole-deck resize.
- [x] Implement rp::Deck deep clone/validate; rp::Editor with bounded atomic history, independent revision high water, selection and current-page history, replace_deck clearing history; input validation before mutations.
- [x] Implement geometry bounds/hit-test/resize and wrapping helper using measure callback, preserving UTF-8 boundaries and explicit empty lines; finite clipping-safe coordinates.
- [x] Execute host tests with -Wall -Wextra -Werror -fno-exceptions -fno-rtti and ASan/UBSan, report exact command and outcomes. Independent task review; fix all Important findings.

### Task 2: Native files, atomic save and recovery

**Files:** userland/apps/reliefppt/{document.h,document.cpp}; tests/host/test_reliefppt_document.cpp.
**Consumes:** model.h and storage.h from Task 1. **Produces:** encode/decode/load/save_deck, FileStamp detection, atomic_write, recovery directory/reservation/flock; exact interfaces in interfaces.md.

- [x] Write tests first for native roundtrip with Chinese, every object kind, style, background/notes, layer order and RGBA; malformed/truncated/unknown version/enum/UTF-8/NaN/oversize; decode failure preserves existing Deck.
- [x] Implement explicit little-endian RPPT version 1 binary serializer with input-size and cross-field validation, no struct dumps or unaligned reads; load candidates before replace.
- [x] Test real temp files: external-modification conflict, cancellation/failed temp preserves original, symlink/identity handling, mode and directory durability; reserve and flock recovery with two processes/instances and verify active copy cannot be acquired.
- [x] Implement same-directory atomic writes, FileStamp and recovery using existing Word patterns without changing Word; host sanitizers and independent review.

### Task 3: Shared renderer and functional editor

**Files:** userland/apps/reliefppt/{render.h,render.cpp,canvas.h,canvas.cpp,window.h,window.cpp,dialogs.cpp,files.cpp,slideshow.cpp,main.cpp}; tests/host/test_reliefppt_layout.cpp; tools/tests/reliefppt_guest_verify.cpp.
**Consumes:** Tasks 1–2 headers. **Produces:** render_slide(Deck,index,geometry); renderer PNG callback and HTML export; PptWindow UI.

- [x] Add failing wrapping/hit-test tests, and guest helper that prepares a Chinese deck and independently validates exact saved content (native and exported PNG/HTML). Guest acceptance written before dependent UI actions.
- [x] Build compact ribbon, thumbnail sidebar with page reorder controls, canvas, text edit overlay, resize handles/multi-select/grid/snap, status/zoom and collapsible notes; native FLTK controls retain theme and CJK IME. No duplicate large title.
- [x] Bind all visible actions to real atomic Editor commands: object/text styles, position/size dialog, clipboard, slide layouts/background, undo/redo, select/drag/resize, alignment/layers, page aspect, example and native save/load/conflict/recovery.
- [x] Implement one shared rendering path for thumbnails/editor/fullscreen/PNG; render images with transparency and text wrap/align/bullets. Export current PNG and standalone HTML with raster slides, keyboard navigation, escaped metadata, no notes/resources; prevent export over open native inode.
- [x] Verify strict GUI syntax, normal target build, sanitizer host suites, native focus transitions and actual Chinese editing.

### Task 4: Desktop integration

**Files:** components.toml, apk-ownership.json, app-icons.list, mk/userland.mk, installer-console-profile.sh, fileman boundary/main/host test, reliefppt.desktop, make_app_icons.py and generated icon assets, gettext catalogs, relevant existing staging tests.

- [x] Extend component/ownership/desktop/installer/fileman behavior fixtures for ReliefPPT first and observe expected failures.
- [x] Add default office app, .rppt routing only, hicolor 16/32/48/64/128/256 and BMP WIP presentation icon, standard WM_CLASS and icon properties, Chinese strings.
- [x] Run app-icons/components/desktop-backend/installer-stage/apk-ownership/fileman-boundary plus app model/document/layout suites, and target build.

### Task 5: System acceptance, review and documentation

**Files:** tools/test_reliefppt_qemu.py; docs/RELIEFPPT.md; docs/RELIEFPPT-VALIDATION.md.

- [x] Write QEMU driver using a fresh private clone of existing Xorg image; validate target/dependency and payload SHA; use external focus probe and acknowledge key events; keep source unchanged.
- [x] Exercise Chinese input, style, text editing, object movement/resize, multi-page duplicate/reorder, native save/reopen, branch-close prompt, PNG/HTML export, fullscreen navigation/exit, 60s autosave then SIGKILL/recover/save. Parse actual guest-produced files independently outside VM.
- [x] Capture editor, properties/minimum size, fullscreen and recovered content; inspect actual pixels. Final independent broad review and bounded fixes/re-review.
- [x] Write manual/format/limits and layered validation report with hashes, commands, outcomes and VMware 验证未完成. git diff --check, no commits/pushes.
