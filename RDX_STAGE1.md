# RDX 第一阶段：8 复音压力测试版

这一版本把 CoreS3 项目中的 `RDX_new` 音色格式和四算子结构接到 Felucca 的音频框架中。固件只显示 RDX 引擎，并继续占用持久化编号 12，因此已有 FM6-only 迁移逻辑不会把旧设置中的引擎编号解释成无效值。

## Felucca 音频调用链

`firmware/src/voice.c` 以 32 个采样为一个控制块。每个声部先计算音高、音量和调制值，再调用当前引擎：

- `engine_t.note_on(track_t *, voice_t *)`：一个新声部开始或被重新触发。
- `engine_t.render(track_t *, voice_t *, int32_t *out, uint32_t n, const vmod_t *)`：把最多 32 个单声道 `int32_t` 采样**累加**到 `out`，不能覆盖已有内容。
- `engine_t.done(track_t *, voice_t *)`：使用自有包络的引擎报告释放是否结束。
- `engine_t.poly`：此引擎允许使用的最大复音数。RDX 固定为 8。
- `engine_t.mem`：每个声部共享内存池中该引擎需要的字节数。

输入状态主要分成三层：

1. `track_t` 保存当前音色、8 个引擎宏参数、公共效果和该声部的 Felucca voice 槽。
2. `voice_t` 保存音符、力度、gate、当前音高、滑音和分配状态。
3. `vmod_t` 是每个 32-sample 控制块的输入，包含基础相位增量 `inc`、1/16 半音音高、块首/块尾 Q15 音量，以及滤波/形状调制。

RDX 的输出保持 Felucca 的 Q15 音频尺度，经过 `VOICE_FS` 和 `vmod_t` 的音量斜坡后累加到混音缓冲。后续声像、插入效果、chorus、delay、reverb 和主输出仍由 Felucca 处理。

## 已实现内容

- 150 字节 Reface DX patch 布局：38 字节 common、4 × 28 字节 operator。
- 参考工程的 35 个 RDX 工厂音色。
- 12 种四算子算法。
- 每个算子的启用、ratio/fixed 频率模式、fine、detune、输出电平和两种反馈形态。
- 四 rate / 四 level 包络、键盘 rate scaling 和力度灵敏度。
- 四段 Pitch EG；每个算子的 `PEG Enable` 决定是否接收音高包络。
- RDX 自有 LFO：sine、triangle、saw up/down、square、S&H 8 和 S&H，含 speed、delay/fade、PMD、逐算子 PM/AM 开关与 CC1 mod wheel。
- 以 C4（MIDI 60）为 breakpoint 的左右 keyboard level scaling，支持 Reface DX 的四种正/负、线性/指数曲线。
- Reface DX 的 Poly、Mono Full、Mono Legato 和 patch 自有 portamento time。重叠按键与 held-note fallback 保留包络，滑音在 Q16 半音精度下执行。
- Yamaha Reface DX SysEx：接收/发送 38 字节 common、4 × 28 字节 operator、header/footer，支持整音色 bulk dump、局部 block、parameter change、bulk request 和 parameter request，并验证 Yamaha 7-bit checksum。
- 8 复音限制、声部释放和 DC blocker；第 9 个同时按下的音符按 Felucca 的共享声部分配规则抢占旧声部。
- 8 个面板宏：算法、反馈、调制器电平、调制器 ratio、EG 速度、力度灵敏度、carrier detune、patch。
- 默认双核构建会在每次开机启动 Core 1。单条 RDX 音轨发声时会把整条音轨交给 Core 1；多条音轨时仍按上一音频块的实际复杂度自动分配。
- 重触发和抢占声部时使用 128 sample（约 2.9 ms）的波形连续性修正。松键保持原振荡轨迹和原有 release；包络真正结束时继续最终波形并用 256 sample（约 5.8 ms）淡出，避免块内硬切，同时保留混响尾音。
- HALL 混响返回增加两级约 6.5 kHz 的低通，并保留定点除法余数；它压低默认 DAMP 下 8–10 kHz 的宽带静电声，同时保证尾音最终回到精确的零。

逐采样热路径使用定点 C、Felucca 的正弦表和预生成查表数据。它不依赖 C++、浮点数学库、堆分配或 ESP32 运行时。

### 第一阶段性能优化

- 每个算子的反馈宏、范围限制和反馈系数查表从逐采样循环移到 32-sample 控制块开头；一个 8 复音控制块由 1024 次降为 32 次。
- 已证明不会溢出的反馈低通、反馈增益和增益表插值改用 32 位乘法；波形乘输出增益、包络增益仍保留 64 位运算，因为 RDX 内部增益可超过 1.0，不能安全缩窄。
- 算法输出归一化和 256-sample 强制终止淡出复用 Felucca `dsp.c` 的 `mulq16()`，避免目标 CPU 的软件 64 位乘法。
- 128-sample 重触发衔接改为由原始修正量计算的线性权重，不再逐采样执行有符号除法，也不会积累整数除法的舍入残差。

这一步没有降低包络更新频率、正弦插值精度或复音数，也没有改变 12 种算法的连接关系。后续性能判断应以真机 INFO 页的 `RDX AVG`、`RDX PEAK` 和 `AUDIO LATE` 为准。

## 当前边界

完整四算子图形编辑器仍未实现。SysEx 导入的 150 字节音色会成为当前 track 的 RAM 音色，可立即演奏并可由 Reface DX dump request 读回；重新选择 factory preset 或重启会离开这个临时音色。Felucca 的项目和用户 preset 目前仍保存 factory patch 编号及 8 个宏，不会把任意 SysEx 音色的 150 字节嵌入项目。

Pitch EG、LFO 和 portamento 在每个 32-sample 控制块计算端点，块内对算子相位增量做线性插值。相位转换使用 1/16 半音节点的定点查表；没有 `pow()`、浮点或 64 位除法。极短的 PEG stage crossing 会在最多 32 个采样内被线性化，这是当前定点实时实现与 CoreS3 分段浮点版本之间的主要精度差异。

## CPU 负载与过载行为

在 FM-1 上打开 `MENU > SYSTEM > INFO` 可看到：

- `CORE 0`：约 93 ms 平滑后的 Core 0 独立工作占用率。它以每个 128 sample 音频半缓冲的 2.90 ms 截止时间为 100%，双核版本会扣除等待 Core 1 的时间；完整交付耗时看 `WALL MAX` 和最近一次 `L WALL`。
- `C0 PEAK`：从本次开机起最慢的一次 Core 0 音频渲染时间，单位 `us`。
- `CORE 1`：Core 1 实际执行分配给它的声部渲染时间占截止时间的比例。
- `C1 PEAK`：从本次开机起 Core 1 最慢的一次渲染时间。没有声音时 Core 1 不接收空任务，数值会逐渐回落到 0%；任一 RDX 音轨发声后的下一个控制块开始，Core 1 应显示非零负载和峰值。

连续两个半缓冲超过 85% 时，Felucca 会请求削减一个声部，优先结束已经松键、振幅较低的声部；仍然过载时再削减可牺牲的 held voice。渲染超过 2.90 ms 时 DMA 不会等待，`audio_late` 会增加，输出可能出现旧缓冲重复或边界不连续，听感是爆音、断裂或节奏抖动；主循环和 MIDI/按键事件的处理也可能积压，造成输入延迟。双核版本若 Core 1 单次任务超过约 8 ms，Core 0 会放弃等待并停用 Core 1，通常先发生一次音频故障，随后退回单核运行。

## 构建与验证

在 PowerShell 中运行：

```powershell
cd E:\Projects\fm1\RDX
.\build-rdx.ps1
```

RDX 构建默认生成强制启动第二核的固件：

```powershell
.\build-rdx.ps1 -Cpu2 1
```

烧录后每次开机都会启动 Core 1，不读取用户的 `2ND CORE` 开关。Core 1 若未响应或任务超时，本次运行仍会退回单核，并在下次开机重新尝试。只有明确传入 `-Cpu2 0` 才生成不含第二核代码的版本。脚本会另外保留 `build\felucca-rdx-dualcore.fwsc` 或 `build\felucca-rdx-singlecore.fwsc`，避免混淆两个版本。

脚本会同时生成：

- `build\felucca.fwsc`：FM-1 固件包。
- `build\felucca.bin`：应用镜像。
- `build\emu`：浏览器模拟器。
- `build\site\webapp\installer`：本地网页安装器。

`web/emu/rdx_test.mjs` 会演奏全部 35 个 factory patch，检查每个 patch 都有有效输出、松键边界连续，检查第九个同时按下的音符不会让活动声部超过 8，并检查松键后声部最终释放。它还验证 Pitch EG、RDX LFO、keyboard level scaling、mono portamento、SysEx checksum、bulk import 和七段完整 dump。

要重新从 CoreS3 参考工程导入 factory patch 和查表数据，可运行：

```powershell
python tools\gen_rdx_data.py E:\Projects\CoreS3\reface_cp_assistant\components\rdx_reface firmware\src\felucca_rdx_data.h
```

浏览器模拟器可用以下命令启动，然后访问 `http://localhost:8790/`：

```powershell
.\serve-rdx.ps1 emulator
```

本地安装器可用以下命令启动，然后访问 `http://localhost:8000/webapp/installer/`：

```powershell
.\serve-rdx.ps1 installer
```

## 2026-10-21: glide investigation and dual-core recovery

- DynaLead factory common bytes are MONO-FULL=1, PORTA=0 in both this project and the CoreS3 RDX_new shared factory data. This preserves envelopes across overlapping notes but intentionally does not glide. Set PORTA=48 (about 445 ms) to test audible glide without changing factory data.
- Added `tests/rdx_runtime_test.c`: FULL/LEG, overlapping and separated notes, up/down 7/12/24/36/48 semitones; 1..8 voices with spread/reverb, serial versus threaded output/state equality; injected worker failures after jobs 1 and 17.
- RDX bypasses the generic Felucca envelope tick: only its own envelope/done/256-sample forced tail can retire it. Generic REL=0 previously could deactivate it before rdx_done.
- Core1's assigned voice states are backed up before dispatch. After HAL holds a failed Core1, restore those states before Core0 rerenders; discard the failed job's private output. This prevents double advancement but cannot undo an audio deadline already missed.
- Odd voice counts assign the extra voice to Core1 (7: Core0=3, Core1=4); 8 remains 4+4, mono stays on Core0. Both render concurrently before joining. No claim of measured AC79 speedup; rendezvous remains every 32 samples, deadline remains 128/44100 seconds, recovery timeout remains 8 ms.
- Validation passed: runtime tests above; all nine RDX groups in dualcore_test (all split modes); all wasm RDX regressions; native build RAM and section checks.
- Output: build/felucca.fwsc, SHA256 9B7B03A1B9C0FD9AF9A2985FA0ED50510344929652A21D4A8D8105492BC10EC4. Matching dualcore copy and local installer refreshed. Real FM1 timing/listening still needs hardware validation.

## 2026-10-21: Core1-heavy voice split and AUDIO LATE attribution

- The RDX Core1 voice counts for 1..8 active voices are now `1, 2, 2, 3, 3, 4, 4, 5`; Core0 handles the remaining `0, 0, 1, 1, 2, 2, 3, 3` voices plus all control preparation, mixing and effects.
- The Core1 timeout snapshot now holds five voices. Host tests verify 1..8 voices produce identical audio and final RDX state in serial, parallel and injected-recovery runs.
- The AUDIO LATE overlay shows `SOURCE CORE0`, `SOURCE CORE1`, `SOURCE BOTH`, or `SOURCE SYNC`. Attribution compares each core's independent busy ticks with the exact 128-frame hardware deadline; `SYNC` means neither core alone exceeded it but rendezvous/IRQ timing missed the wall deadline.
- Native image is 451380 B; RAM `.data + .bss` is 54904 B of 73728 B. All 35 web RDX regressions and all RDX dual-core preset groups passed.
- Core1 mailbox failure and 8 ms timeout are explicitly attributed to Core1 even when the stalled job cannot publish a completed render-time sample.
- Output: `build/felucca.fwsc`, SHA256 `03A74144D43C9DA5008CD16DBF90ABA507721DF497856F4B4B0B84A44A495779`. The named dual-core copy and local installer contain the same package.

## 2026-10-21: same-half deadline profiler

- INFO is scrollable with the PRESETS encoder. It keeps the existing RDX/Core meters, then shows `WALL MAX`, `IRQ MAX` and `WAIT MAX`.
- The `L ...` rows are one coherent snapshot of the most recent AUDIO LATE half: wall time, IRQ entry delay, Core0 busy time, Core1 wait/work, nested TIMER5 work, event/mix preparation, RDX control preparation, Core0 FM rendering, cross-core dispatch/merge, post FX, output tail and uncategorised Core0 remainder.
- TIMER5 is already contained in whichever Core0 phase it interrupted and is displayed as a subset; do not add it to the phase total again. Core1 work runs in parallel; the useful critical-path comparison is `L WALL`, `L C1 WAIT`, `L C1 WORK` and the Core0 phase breakdown.
- DMA-late detection now samples the active half immediately when audio data is ready. Load-meter and diagnostic bookkeeping run afterwards and can no longer create a false AUDIO LATE report by crossing the following boundary.
- Native dual-core image is 452884 B; RAM `.data + .bss` is 55144 B of 73728 B. RDX 1..8 voice serial/parallel/recovery comparisons and all 35 web regressions passed.
- Output: `build/felucca.fwsc`, SHA256 `0CDA4E286AEE50C563BA6E4C383E10EBF38E8365E6542D907AD07495FFFFFD82`. The named dual-core copy and local installer contain the same package.

## 2026-10-21: seven-voice rebalance and per-half IRQ timing

- Hardware timing showed that 6 voices at Core0/Core1 = 2/4 spent about 905 us waiting for Core1. The high-polyphony splits are now 6 = 3/3, 7 = 3/4 and 8 = 4/4.
- The Core1 counts for 1..8 active voices are `1, 2, 2, 3, 3, 3, 4, 4`; Core0 handles `0, 0, 1, 1, 2, 3, 3, 4`.
- `IRQ MAX` and `L IRQ` now report only the excess time of one audio-half start-to-start interval above the 128-frame deadline. The previous absolute expected-tick accumulator could retain oscillator mismatch and a delayed interrupt indefinitely, producing growing values unrelated to the current late half.
- The 1..8 voice host test confirmed identical serial, parallel and recovery audio/state. The native image is 452884 B; RAM `.data + .bss` is 55144 B of 73728 B; all 35 web RDX factory regressions passed.
- Output: `build/felucca.fwsc`, SHA256 `9E7EEA3A7EAD5D1B41A6A0B95DB3C3C7F2CE3F882D4DD0813BF707FF99F8070C`. The named dual-core copy and local installer contain the same package.
