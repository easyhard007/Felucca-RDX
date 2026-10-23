# Felucca RDX 使用与开发指南

## 开发环境

- 项目目录：`E:\Projects\fm1\RDX`
- 构建入口：`build-rdx.ps1`
- 本地服务入口：`serve-rdx.ps1`
- JieLi 工具链：`.toolchains\jieli`
- AC79 SDK：`.toolchains\fw-AC79_AIoT_SDK`
- 默认配置：RDX、8 复音、双核、128-frame 音频缓冲

`.toolchains` 和 `build` 均被 Git 忽略。

## 编译固件

在 PowerShell 中运行：

```powershell
cd E:\Projects\fm1\RDX
.\build-rdx.ps1
```

脚本会完成真机固件、WebAssembly 模拟器、RDX 回归测试和本地安装器构建。主要输出为：

- `build\felucca.fwsc`：模拟器和安装脚本默认读取的固件包；
- `build\felucca-rdx-dualcore.fwsc`：双核固件的具名副本；
- `build\felucca.bin`：FM-1 应用镜像；
- `build\emu`：本地源码模拟器；
- `build\site\webapp\installer`：本地 Web MIDI 安装器。

如需排查双核问题，可以生成单核诊断版本：

```powershell
.\build-rdx.ps1 -Cpu2 0
```

## 在网页 FM-1 模拟器中测试

1. 打开 <https://joakimeriksson.github.io/fm1-emu/?fw=local&mhz=100>。
2. 选择 `Open .fwsc`，加载 `E:\Projects\fm1\RDX\build\felucca.fwsc`。
3. 点击 `Start`，然后使用页面琴键、电脑键盘或 MIDI 输入演奏。
4. 打开 `SYSTEM > INFO`，观察 Core 0、Core 1、RDX 每声部耗时、WALL、WAIT 和 AUDIO LATE 数据。

网页 FM-1 模拟器执行打包后的真实 pi32v2 固件，适合检查界面、MIDI、音频缓冲和接近真机的 CPU 时序。

也可以启动源码 WebAssembly 模拟器：

```powershell
.\serve-rdx.ps1 emulator
```

然后访问 <http://localhost:8790/>。它适合快速检查音色、SysEx、声音路由和界面逻辑，但不模拟真实 USB、DMA 和 AC79 双核时序。

## 烧录到 FM-1

先编译固件，再运行：

```powershell
.\serve-rdx.ps1 installer
```

用 Chrome 或 Edge 打开 <http://localhost:8000/webapp/installer/>，点击 `Install`，授权访问 FM-1 的 MIDI 端口并等待写入完成。安装器始终读取当前 `build\felucca.fwsc`。

写入期间保持设备供电，不要拔出 USB 线或关闭页面。若要恢复官方固件，可使用 Felucca 官方安装器的 `Return to official V15`。

## 音色与效果

首页以“音色集 - 音色名”显示当前声音。内置音色集为：

- `Factory`：32 个 reface DX 原厂音色；
- `Selected`：21 个导入的第三方 reface DX SysEx 音色。

RDX 使用串行立体声 `FX1 -> FX2` 链。Phaser 和 Flanger 可在 `AUDIO` 菜单中切换算法：

- `PHS MODEL`：`RDX` 为定点 PicoFaceDX 算法，`FELUCCA` 为保留的原算法；
- `FLG MODEL`：`RDX` 为定点 PicoFaceDX 算法，`FELUCCA` 为保留的原算法。

两项默认均为 `RDX`，选择会保存到系统设置。

## 性能诊断

FM-1 在 44.1 kHz、128-frame 半缓冲下，每次必须在约 2.90 ms 内完成音频计算。INFO 页中：

- `CORE 0/1` 和 `C0/C1 PEAK` 分别记录两个核心；
- `RDX AVG/PEAK` 是实际每声部渲染时间；
- `WALL MAX` 是完整半缓冲的最长交付时间；
- `WAIT MAX` 是 Core 0 等待 Core 1 的时间；
- `L ...` 项是最近一次 AUDIO LATE 的同一半缓冲快照。

修改渲染或效果算法后，应在真机连续测试 6、7、8 复音，并同时记录 INFO 数据。浏览器无爆音并不能证明真机不会发生 AUDIO LATE。

## RDX 数据生成

从 CoreS3 参考工程重新生成 Factory、Selected 和查表数据：

```powershell
python tools\gen_rdx_data.py E:\Projects\CoreS3\reface_cp_assistant\components\rdx_reface firmware\src\felucca_rdx_data.h
```

RDX 音频实现主要位于：

- `firmware\src\eng_rdx.c`
- `firmware\src\eng_rdx_opt.c`
- `firmware\src\rdx_fx.c`
- `firmware\src\rdx_fx_picoface.c`
- `firmware\src\rdx_sysex.c`
- `firmware\src\felucca_rdx_data.h`
