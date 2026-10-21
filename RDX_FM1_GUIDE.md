# RDX / FM-1 开发基线

本目录基于 Felucca 1.6.1，增加了 `FELUCCA_FM6_ONLY=1` 构建模式。该模式仅在界面、编辑器协议和运行时暴露 FM6 与它的 8 个工厂音色；四个轨道开机后也全部使用 FM6。FM6 仍保留 Felucca 的持久化编号 12，因此旧工程、用户音色和后续 RDX 引擎迁移不会因为重新编号而损坏。

## 当前开发环境

- Windows 项目目录：`E:\Projects\fm1\RDX`
- WSL：Ubuntu 22.04，原生运行 JieLi Linux x86-64 工具链
- 项目内工具链：`.toolchains/jieli/jieli-linux-toolchains-20260730.1`
- 项目内 SDK：`.toolchains/fw-AC79_AIoT_SDK`，版本 `AC79NN_SDK_V1.2.1_2023-12-13`
- WSL 构建依赖：Pillow、fontTools、Emscripten

`.toolchains` 已加入 `.gitignore`，不会误提交大体积第三方工具。

## 编译

在 PowerShell 中运行：

```powershell
cd E:\Projects\fm1\RDX
.\build-fm6.ps1
```

脚本会依次编译真机固件、网页模拟器，运行模拟器自测，并生成本地网页安装器。主要输出：

- `build/felucca.bin`：FM-1 应用镜像
- `build/loader/ota.bin`：Felucca 更新加载器
- `build/felucca.fwsc`：模拟器和真机安装使用的完整包
- `build/emu/`：从同一份 FM6-only 源码编译的本地网页模拟器
- `build/site/`：包含本次自编译固件的本地 Web MIDI 安装器

## 在网页 FM-1 指令级模拟器中测试 `.fwsc`

1. 用 Chrome、Edge 或 Firefox 打开 <https://joakimeriksson.github.io/fm1-emu/>。
2. 点击 `Open .fwsc…`，选择 `E:\Projects\fm1\RDX\build\felucca.fwsc`。
3. 点击 `Start`。浏览器第一次启动音频必须由用户点击触发。
4. 用页面琴键、电脑键盘或 `MIDI in` 选择的 MIDI 键盘演奏。Felucca 的四轨对应 MIDI 1–4 通道。
5. 检查 `PRESETS` 中只有 FM6，且工厂音色为 TINE EP、BELL、FM BASS、BRASS、PAD、MARIMBA、ORGAN、PLUCK。

这个模拟器执行打包后的真实 pi32v2 固件，适合检查启动、屏幕、按键、旋钮、MIDI、音频和 CPU 负载。

## 在本地 Felucca 源码模拟器中测试

```powershell
.\serve-fm6.ps1 emulator
```

然后打开 <http://localhost:8790/>。此版本直接运行从当前源码编译的 WebAssembly，适合快速改 UI 和合成器代码。它不模拟 USB、TRS MIDI、更新流程和用户 sample slot。

## 烧录到 FM-1

1. 先在当前固件自己的编辑器或安装器中备份音色、工程和设置。
2. 给 FM-1 保持充足电量，用可靠的数据线直接连接电脑 USB 口，避免 USB Hub。
3. 在 PowerShell 中运行：

   ```powershell
   .\serve-fm6.ps1 installer
   ```

4. 用 Chrome 或 Edge 打开 <http://localhost:8000/webapp/installer/>。`localhost` 属于 Web MIDI 允许的安全上下文。
5. 点击 `Install`，按浏览器提示授权并选择 FM-1 的 MIDI 端口。等待写入和设备重启完成；写入期间不要拔线、关机或关闭页面。
6. 重启后确认 `PRESETS` 只显示 FM6，并逐个试听 8 个工厂音色。

本地安装器已经嵌入 `build/felucca.fwsc`，不会误装 Felucca 官方发布版。若要恢复原厂 V15，可使用 Felucca 官方安装器的 `Return to official V15`，并提供未修改的官方 `FM-1.fwsc`。如果写入中断后设备无法进入更新模式，恢复需要 FM-1 Transporter：<https://github.com/kurogedelic/FM-1-transporter>。

## 本阶段的边界

当前改动建立的是“只有 FM6 可见和可运行”的稳定基线；尚未接入 ESP32-S3 上的 RDX/PicoFaceDX 代码。下一阶段应把 RDX 做成新的 `engine_t` 实现，先替换 FM6 的渲染回调但保留 MIDI、音色存储、UI、效果器与音频 DMA，再逐步加入 Reface DX 的四算子参数和 SysEx。

## 从早期 FM6-only 固件升级

2026-10-19 的首版 FM6-only 固件会沿用设备里旧 Felucca 的 `FAV`/分类筛选和自动保存工程。实体机因此可能只循环少数音色，或显示 FM6 名称却没有载入相应 FM6 patch。修正版首次启动时会把音色列表恢复为 `ALL`，跳过一次不兼容的旧自动保存；之后产生的 FM6-only 自动保存仍会照常恢复。手动载入旧工程时，非 FM6 轨道也会迁移成完整的 FM6 工厂音色。
