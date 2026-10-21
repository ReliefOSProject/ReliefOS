# ReliefNT kernel modularisation

Approved goal: reorganise every self-authored kernel component by responsibility, split mixed-responsibility implementation files, and use recursive directory-local Kbuild lists without changing behaviour or ABI.

- [x] Record source inventory, build/test baseline and red structural checks.
- [x] Migrate recursive Kbuild with generated header ordering, host fixtures, vDSO, compiler identity and product compatibility.
- [x] Modularise core: init, scheduler, process, signals, IPC, time, device, syscalls, execution and lib.
- [x] Decompose network protocols, socket lifecycle and service helpers.
- [x] Organise VFS/filesystems, block/storage, console/graphics/audio, x86 and memory by responsibility.
- [x] Update parent source consumers, tests and documentation; audit stale paths/duplicate and unbuilt implementations.
- [x] Verify clean debug/release builds, incremental rebuilds, host regressions, UAPI and guest QEMU/runtime; review all changes.

Keep public ABI, locks, interrupt constraints, init order, algorithms and output paths. No arbitrary line count cap. Preserve upstream/protocol/generated data. Native source files compiled separately; implementation inclusion is not modularisation. Parent test-only white-box fixtures may link or compile real module sources explicitly. No commits, push or branch creation. TMPDIR in parent out/tmp. One 4 GiB task VM at a time. VMware and physical hardware validation reported separately.

Cross-task contracts: root agent owns root/ancestor Kbuild, global source map, include root and parent fixtures. Delegated module worker only edits assigned subtree and records moves in an ignored JSON map. Integration consumes those maps to update source references and compile lists. All module-private interfaces remain private, Doxygen documents changed ownership/lock requirements.
