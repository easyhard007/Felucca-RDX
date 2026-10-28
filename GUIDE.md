# Felucca RDX 使用与开发指南

## 开发环境

- 项目目录：克隆仓库后生成的 `Felucca-RDX` 目录
- 构建入口：`build-rdx.ps1`
- 本地服务入口：`serve-rdx.ps1`
- JieLi 工具链：`.toolchains\jieli`
- AC79 SDK：`.toolchains\fw-AC79_AIoT_SDK`
- 当前版本：Felucca RDX 1.0.3
- 固件独立身份：`FM-1_947`（与上游 Felucca 的 `FM-1_900` 区分，同时保留 9xx Loader 恢复兼容性）
- 默认配置：单音色、单轨、RDX 8 复音、双核、128-frame 音频缓冲

`.toolchains` 和 `build` 均被 Git 忽略。

版本号只在 `firmware\src\rdx_version.h` 中维护，并按语义化版本规则发布。启动画面、模拟器和安装器会读取同一版本。

## 编译固件

在 PowerShell 中运行：

```powershell
git clone https://github.com/easyhard007/Felucca-RDX.git
cd Felucca-RDX
.\build-rdx.ps1
```

脚本会完成真机固件、WebAssembly 模拟器、RDX 回归测试和本地安装器构建。主要输出为：

- `build\felucca.fwsc`：模拟器和安装脚本默认读取的固件包；
- `build\felucca-clean.fwsc`：全新安装包；写入成功后清除工程、用户音色、用户采样、自动保存和系统设置；
- `build\felucca-rdx-dualcore.fwsc`：双核固件的具名副本；
- `build\felucca.bin`：FM-1 应用镜像；
- `build\emu`：本地源码模拟器；
- `build\site\webapp\installer`：本地 Web MIDI 安装器；
- `build\site\webapp\emulator`：可发布到 GitHub Pages 的源码 WebAssembly 模拟器。

普通包也是提交到 The FM-1 Firmware Hub 的唯一固件包。它会在首次启动时检查
Felucca-RDX 用户区标记：从旧版 RDX 升级时保留 User 音色和兼容设置；从原版
Felucca、其他分支或未知固件进入时，仅废弃不兼容的工程、自动保存和用户音色对象，
保留面板校准。迁移使用原子 A/B 存储提交；中途断电时，下次启动会继续迁移。

如需排查双核问题，可以生成单核诊断版本：

```powershell
.\build-rdx.ps1 -Cpu2 0
```

## 在网页 FM-1 模拟器中测试

1. 打开 <https://joakimeriksson.github.io/fm1-emu/?fw=local&mhz=100>。
2. 选择 `Open .fwsc`，加载项目下的 `build\felucca.fwsc`。
3. 点击 `Start`，然后使用页面琴键、电脑键盘或 MIDI 输入演奏。
4. 打开 `SYSTEM > INFO`，观察 Core 0、Core 1、RDX 每声部耗时、WALL、WAIT 和 AUDIO LATE 数据。

网页 FM-1 模拟器执行打包后的真实 pi32v2 固件，适合检查界面、MIDI、音频缓冲和接近真机的 CPU 时序。

也可以启动源码 WebAssembly 模拟器：

```powershell
.\serve-rdx.ps1 emulator
```

然后访问 <http://localhost:8790/>。它适合快速检查音色、SysEx、声音路由和界面逻辑，但不模拟真实 USB、DMA 和 AC79 双核时序。

公开在线版为 <https://easyhard007.github.io/Felucca-RDX/webapp/emulator/>。GitHub Actions
每次发布都从当前 RDX 源码重新编译它，因此打开网页就是最新版，不需要手动选择
`.fwsc` 文件。如需检查接近真机的 CPU 时序与音频缓冲，仍应使用上述执行真实
pi32v2 固件的 FM-1 模拟器。

## 烧录到 FM-1

公开网页可直接使用，无需在本机启动服务器：

- 安装器：<https://easyhard007.github.io/Felucca-RDX/webapp/installer/>
- 音色编辑器：<https://easyhard007.github.io/Felucca-RDX/webapp/editor/#library>
- 在线模拟器：<https://easyhard007.github.io/Felucca-RDX/webapp/emulator/>

`main` 分支每次推送后，GitHub Actions 会自动编译双核 RDX 固件，生成普通安装包和
全新安装包，再将安装器、编辑器与模拟器一起发布到 GitHub Pages。网页安装器和它提供的
固件来自同一次构建。

如需本地使用，先编译固件，再运行：

```powershell
.\serve-rdx.ps1 installer
```

用 Chrome 或 Edge 打开 <http://localhost:8000/webapp/installer/>，点击 `Install`，授权访问 FM-1 的 MIDI 端口并等待写入完成。需要完全清除旧用户区时，勾选 `Fresh install` 后再安装。该模式会清除工程、用户音色、用户采样、自动保存以及全部系统设置，内置 Factory/Selected 音色不受影响。

启动安装器时，脚本会在发现新固件包后自动重建网页安装目录，避免 `build\site\firmware` 继续提供旧版本。普通安装使用 `build\felucca.fwsc` 并保留用户区；全新安装使用 `build\felucca-clean.fwsc`。

普通包已经能够安全处理来自其他 Felucca 固件的不兼容用户区。`Fresh install` 仍然保留，适合用户明确希望彻底恢复初始状态、清除所有系统设置和用户数据的情况；正常的 Felucca-RDX 版本升级不应勾选它。

写入期间保持设备供电，不要拔出 USB 线或关闭页面。若要恢复官方固件，可使用 Felucca 官方安装器的 `Return to official V15`。

## 音色与效果

RDX 固件只有一个运行时轨道。ALGORITHM 旋钮不再切换轨道，MIDI 菜单只显示一个 `MIDI CHANNEL`，四轨 Mixer、T2–T4 和自动选轨入口均不再出现在 RDX 界面中。底层保留的旧工程读取代码只用于兼容历史文件，导入时仅接收第一条乐器轨道。

首页以“音色集 - 音色名”显示当前声音。内置音色集为：

- `Factory`：32 个 reface DX 原厂音色；
- `Selected`：21 个导入的第三方 reface DX SysEx 音色。

首页四个旋钮依次控制 `MODE`、`PORTA`、`GAIN` 和 `Trans.`。`MODE` 提供
`Poly`、`MonoFull`、`MonoLeg.` 三种 Reface DX 发声方式；`PORTA` 的范围为
0–127；`GAIN` 的范围为 -24 到 +24 dB，并在加载 Factory/Selected 音色时
使用 C3、C4、C5 起音后 1 秒内最响的连续 300 ms ITU-R BS.1770 K-weighted 感知
响度迭代标定默认值；校正会经过完整串行效果与最终限幅后复测，直到 1 dB 调整粒度内收敛，
使中高频、短促起音和慢起音都得到更符合听感的权重。用户修改后的 GAIN 会随工程或
用户音色保存。

`KNOB 4` 的主移调 `Trans.` 范围为 -24 到 +24 半音，默认值为 0。`OCT-` 和
`OCT+` 在 -3 到 +3 个八度之间调节主八度，同时按下两键回到 0。非零八度显示
在所有页面顶部正中；八度为 0 时中央保持空白。主移调和八度会同时作用于面板
键盘、MIDI 输入、琶音器和音序器；最终音符会限制在 MIDI 0 到 127 范围内。

RDX 使用串行立体声 `FX1 -> FX2` 链。Phaser 和 Flanger 可在 `AUDIO` 菜单中切换算法：

- `PHS MODEL`：`RDX` 为定点 PicoFaceDX 算法，`FELUCCA` 为保留的原算法；
- `FLG MODEL`：`RDX` 为定点 PicoFaceDX 算法，`FELUCCA` 为保留的原算法。

两项默认均为 `RDX`，选择会保存到系统设置。

按 `FX` 会在 4 页间循环：

1. `FX`：`KNOB 1/2` 调整 FX1 的类型和发送量，`KNOB 3/4` 调整 FX2；
2. `FX1`：按 FX1 类型动态显示其参数；
3. `FX2`：按 FX2 类型动态显示其参数；
4. `SLICER`：保留 Felucca 的切片门控功能。未完成的 `GLOBAL FX` 页面不再显示。

`FX` 首页在四张参数卡上方使用两个横跨双卡的 `FX1` / `FX2` 主题标题条；四张卡依次显示
`TYPE / SEND / TYPE / SEND`。名称与数值均使用小号字体，RDX 效果名称最多显示 6 个字符。TYPE 为
`THRU` 时 SEND 显示 `-` 且旋钮不响应；对应的详情页用一张整行说明卡片代替四个空参数卡片。

Reverb 可调 `TYPE / DEPTH / TIME / DAMP`，类型包括 `ROOM / SPRING / HALL`。当前 `HALL` 是 Mutable Instruments Clouds 拓扑的 Q14 定点立体声移植，每个串行槽拥有独立尾音。Delay 可调 `DEPTH / TIME / FDBK / COLOR`，其 SEND 卡片临时作为 0.1x–5.0x 湿声倍率。音色首次载入时，Chorus 的湿声量由原 Depth 除以 2，Reverb 由原 Depth 除以 2.5，Flanger 使用满额串行输入。用户修改后的参数会随工程和用户音色保存。

## 性能诊断

FM-1 在 44.1 kHz、128-frame 半缓冲下，每次必须在约 2.90 ms 内完成音频计算。INFO 页中：

- `CORE 0/1` 和 `C0/C1 PEAK` 分别记录两个核心；
- `RDX AVG/PEAK` 是实际每声部渲染时间；
- `WALL MAX` 是完整半缓冲的最长交付时间；
- `WAIT MAX` 是 Core 0 等待 Core 1 的时间；
- `L ...` 项是最近一次 AUDIO LATE 的同一半缓冲快照。

修改渲染或效果算法后，应在真机连续测试 6、7、8 复音，并同时记录 INFO 数据。浏览器无爆音并不能证明真机不会发生 AUDIO LATE。

## RDX 数据生成

固件构建直接使用已提交的 `firmware\src\felucca_rdx_data.h`，不依赖原始
Selected `.syx` 文件。维护者需要更新音色时，可从本机的 CoreS3 参考工程和
个人音色库重新生成 Factory、Selected 和查表数据：

```powershell
python tools\gen_rdx_data.py <RDX_REFERENCE> firmware\src\felucca_rdx_data.h `
  --selected "<LOCAL_REFACE_LIBRARY>\selected"
```

原始 `.syx` 文件不进入本仓库；生成后的定长参数记录位于
`firmware\src\felucca_rdx_data.h`，并直接编译进固件。

RDX 音频实现主要位于：

- `firmware\src\eng_rdx.c`
- `firmware\src\eng_rdx_opt.c`
- `firmware\src\rdx_fx.c`
- `firmware\src\rdx_fx_picoface.c`
- `firmware\src\rdx_sysex.c`
- `firmware\src\felucca_rdx_data.h`
