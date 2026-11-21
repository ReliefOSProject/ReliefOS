# VMware SVGA II 实现顺序

依据已批准的路线 A 规格，在当前工作树内推进。

- [x] 将 DRM 前端按文件绑定到后端；在宿主模型验证 FIFO、命令缓冲和 fence 的边界。
- [x] 实现 dumb BO、legacy KMS、Screen Object 和 Screen Target；已有 QEMU SVGA II 2D 与 VirtIO VirGL 回归。
- [x] 实现 GMR/GMR2、MOB、对象表与 fence；在 VMware 以未修改的 Mesa 跑通 Legacy、GB 和 DX 渲染、关键回读与同步路径。
- [ ] 完成[设计第 7 节](../specs/2026-11-21-vmware-svga2-design.md#7-分阶段验收与复现入口)的剩余验收。优先确认设备静默与复位契约，然后补实际 IRQ、各档能力、共享/显示等缺口及性能对照。

## 复现命令

从 ReliefOS 根目录运行。`BUSYBOX` 指向本次验证用的静态 BusyBox，`RUNTIME` 指向包含未修改的 Mesa、libdrm 和 musl loader 的来宾根目录。不要将宿主 glibc 探针或宿主图形库装入镜像。

```sh
export TMPDIR="$PWD/out/tmp"
mkdir -p "$TMPDIR" "$PWD/out/svga-verify"
python3 -B kernel/reliefnt/tools/test_drm.py
build="$PWD/out/svga-verify/kernel"
make -j8 O="$build" SOURCE_DATE_EPOCH=1795219200 kernel
run=$(mktemp -d "$PWD/out/svga-verify/run.XXXXXX")
ln -s "$PWD/kernel/reliefnt/include/uapi/drm" "$run/drm"
musl-gcc -std=c11 -O2 -static -Wall -Wextra -Werror -D__user= -I "$run" \
    -idirafter /usr/include -idirafter "/usr/include/$(cc -dumpmachine)" \
    kernel/reliefnt/tools/tests/svga_guest.c -o "$run/svga-probe"
sh tools/build-virtgpu-egl-probe.sh "$RUNTIME" "$run/egl-probe"
mode=legacy # 或 gb / dx；每个模式使用新目录
python3 -B tools/test_virtgpu_qemu.py --svga3d "$mode" --output "$run/$mode" \
    --kernel "$build/generated/system/kernel.sys" --busybox "$BUSYBOX" \
    --probe "$run/svga-probe" --egl-probe "$run/egl-probe" --runtime-root "$RUNTIME"
vmrun -T ws start "$run/$mode/svga.vmx" nogui
```

核对串口的 `SVGA3D_MODE`、`SVGA3D_GL_PASS`、`SVGA_KMS_PASS` 和 `SVGA3D_VMWARE_DONE:status=0`；同时保存宿主 `mksSandbox.log` 以确认是否使用物理 GPU。控制台验证可附加 `--vmware-console-hold 20`，在 `SVGA_CONSOLE_CAPTURE_READY` 后从 VMware 界面核对 `SVGA CONSOLE RESTORED`，不能用串口标记替代画面。QEMU 2D 使用同一 KMS 探针与 `--svga2`，不传 `--egl-probe`；其通过条件为 `SVGA_QEMU_DONE:status=0`。
