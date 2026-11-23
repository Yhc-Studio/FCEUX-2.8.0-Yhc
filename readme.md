# FCEUX 2.8.0-Y330

> 基于 **FCEUX 2.6.6** 长期维护的非官方增强版本，主要面向模拟器兼容性、NES/VTxx 硬件支持、NSF 播放、PPU/视频精度、输入系统、调试器、快进性能、大 ROM 支持以及录制功能的持续改进。

## 项目简介

FCEUX 2.8.0-Y330 是基于 FCEUX 2.6.6 源代码持续维护的自定义版本。

本项目并不是对 FCEUX 进行大规模重写，而是在尽量保持原有使用习惯、Win32 界面和兼容性的基础上，对实际使用过程中发现的问题进行修正和增强。

当前维护重点主要包括：

- NSF / NSFe 播放器增强
- PPU、Color 512 与视频显示修正
- DirectInput 手柄与 Hotkey 改进
- Tab Turbo / 3200% / 6400% 高速运行优化
- Debugger 功能与滚动性能优化
- UTF-8 / 中文 / 特殊字符文件名支持
- 32MB～512MB 大 ROM 加载与内存释放优化
- OneBus / VT03 / VT09 / VT32 / VT4FFx / VT369 支持
- Mapper、扩展音源与多合一卡兼容性更新
- AVI / NSF 界面录制与显示相关改进

---

## 主要功能

### 1. NSF / NSFe 播放器增强

当前版本支持：

- NSF 与 NSFe 播放
- 曲名、作者、版权信息显示
- NSFe Track Label
- 曲目时间与 Fade 信息
- 播放 / 暂停 / 停止控制
- 波形显示
- 虚拟键盘显示
- Peak / RMS 电平显示
- 扩展音源状态显示
- NSF 播放界面的手柄控制
- 多扩展音源混合播放

已经针对以下扩展音源进行适配或兼容性修正：

- VRC6
- VRC7
- FDS
- MMC5
- Namco 106 / N163
- Sunsoft FME-7 / 5B

同时修复了部分组合扩展音源 NSF 中，某个音源被后初始化音源覆盖、可视键盘与真实音频状态不一致等问题。

### 2. NSF 自定义播放速率

NSF Header 中的：

- `NTSCspeed`
- `PALspeed`

会作为实际 PLAY 调用间隔使用。

旧的“按视频帧累计误差”方案在 NSF 速率与视频刷新率略有差异时，可能出现：

- 某一帧连续执行两次 PLAY
- 某一帧不执行 PLAY
- 长时间播放后产生听感上的突然前进或停留

典型值例如：

- `40FF`（16639 µs）
- `4165`（16741 µs）

当前版本使用更细粒度的 PLAY 调度，避免把 PLAY 调用强制量化到整帧边界。

---

### 3. PPU / Color 512 / 视频修正

NEWPPU 与 Color 512 路径进行了多项修正，包括：

- `$2001` 状态按 dot 处理
- Grayscale 按 dot 生效
- Color Emphasis 按 dot 生效
- Emphasis 状态正确恢复到 0
- 强制空白期间的调色板读取状态修正
- 避免后续 `$2007` 写入影响已记录像素
- 修正部分渲染标志按 8 像素块处理的问题
- 修正普通整数 2x / 3x 放大路径未正确读取 `XDBuf`
- 调整 Color Emphasis 亮度，避免强调色过暗
- VT369 RGB555 / Direct Color 显示路径
- VT369 HUD / Overlay 生命周期与 ROM Close 清理

视频相关还包括：

- NTSC 2x 显示支持改进
- PAL 3x 相关输出兼容
- Digital Prime 调色板调整
- Color 512 输出路径修正
- 2x / 3x 整数缩放修正
- 部分强调色与灰度显示问题修复

---

### 4. 手柄、Hotkey 与输入系统

当前版本增加并完善了 DirectInput 手柄快捷键支持，可绑定：

- 普通按钮
- X / Y / Z 轴
- Rx / Ry / Rz 轴
- POV / Hat

例如可绑定：

- Pause
- Reset
- Save State
- Load State
- Save Slot
- Screenshot
- Frame Advance
- Turbo

一次性快捷键与持续型快捷键分别处理：

- Pause / Reset / Screenshot / Save / Load：按下一次只触发一次
- Frame Advance / Turbo：保留按住行为

手柄绑定会保存设备 GUID，提高重新启动后的设备匹配稳定性。

同时修复：

- 鼠标点击模拟器内部时误识别为 `JS 0 axis x+ / x-`
- DirectInput `GetDeviceState()` 失败后全零状态被误认为有效输入
- 点击桌面、任务栏或其他程序时误触 Save Slot 等快捷键
- 从 Hotkey 设置窗口退出后，刚刚用于绑定的按钮立即触发功能
- Turbo 高速运行时 Hotkey 全表反复扫描造成的性能开销

当前 Turbo 路径加入了：

- 空 Hotkey 映射快速返回
- 纯键盘映射不进入 joystick 检查
- Keyboard State Generation 缓存
- Turbo 稳定状态 O(1) Hotkey fast path
- Turbo Input Poll 降频

> 注意：M1 / M2 / M3 / M4 等厂商自定义背键是否可用，取决于驱动是否将它们作为独立 DirectInput Button 暴露给 Windows。

---

### 5. Tab Turbo 与高速运行优化

当前版本对 Tab Turbo 和高倍率运行进行了多轮性能优化。

支持：

- 普通 Turbo
- 3200%
- 6400%

主要优化包括：

- 普通 NES CPU fast path
- `multiplier=1` 时避免每条 CPU 指令执行无意义除法 / 取模
- 无 opcode encryption 时直接返回普通 opcode
- 普通 NES PPU 使用独立 `pputile_novt.inc` 快路径
- 非 VT 游戏跳过 VT369 专用 scanline / RGB555 / Overlay 缓冲处理
- 非 VT 帧跳过 VT369 每像素 Overlay 查询
- Turbo 下 Hotkey / Keyboard / DirectInput 减负
- muteTurbo DirectSound steady-state bypass
- 高速运行时重型工具窗口降频
- 3200% / 6400% 独立视频刷新策略
- Smooth Turbo：保持较高 FPS 的同时避免“冻结式跳帧”

在关闭滤镜和硬件加速等额外视频开销的情况下，普通游戏 Tab Turbo 可达到约 4000 FPS 级别；实际性能取决于 CPU、视频设置、滤镜、窗口缩放和游戏本身。

---

### 6. Debugger 增强

Debugger 进行了多项功能和性能改进。

#### Bank Size

反汇编 Bank 显示支持：

- 32KB
- 16KB
- 8KB

Bank Size 会影响：

- Bank:Address 显示
- Symbolic Debug page
- 鼠标悬停 Bank
- `K==#xx` 断点条件
- 地址换算逻辑

#### Debugger Scroll Performance

针对运行游戏时滚动反汇编窗口导致掉 FPS 的问题进行了优化：

- `WM_VSCROLL / WM_MOUSEWHEEL` 高频事件合并
- 运行中滚动预览约 33Hz
- 滚动期间跳过完整语法高亮
- 跳过 Symbolic name/comment 展开
- 跳过 Code/Data Logger 左侧列重建
- `SB_ENDSCROLL` 不重复完整刷新
- 停止滚动约 120ms 后再完成完整高亮和正式提交
- 暂停 / 单步状态仍保持即时完整刷新

因此在拖动滚动条或使用鼠标滚轮浏览指令时，模拟器主游戏 FPS 不再出现明显下降。

#### 其他调试修正

- `$FFFF` 反汇编 Overflow 修正
- VT opcode Debugger Guard
- VT/OneBus opcode 解密调试支持
- 中文 Debugger UI 保持兼容

---

### 7. UTF-8 与文件名支持

Win32 文件访问路径进行了 UTF-8 / Unicode 兼容性修正。

支持包含以下内容的 ROM / 存档 / 工程文件名：

- 简体中文
- 繁体中文
- 日文
- Unicode 特殊字符
- 部分原来会导致 `Error Opening` 的历史 CJK 文件名

相关文件访问路径逐步统一使用 UTF-8 aware wrapper，覆盖 ROM、Archive、Cheat、AVI、TAS Editor、Save 等常用路径。

---

### 8. 32MB～512MB 大 ROM 支持与性能优化

针对现代大容量多合一卡和测试 ROM，增加了 32MB、64MB、128MB、256MB、512MB 文件加载优化。

主要改进：

- 32MB 起进入 Large-ROM fast path
- plain `.nes` 大文件可跳过不必要的 7-Zip 全格式签名扫描
- 大 ROM 缓冲使用未初始化分配，避免先清零整个 32～512MB 缓冲
- Windows 大块缓冲使用 `VirtualAlloc`
- ROM Close 使用 `VirtualFree(MEM_RELEASE)`
- 8MB / 16MB 分块 streaming read
- 读取过程中同步计算 CRC / Hash
- 减少“完整读取后再扫描整份 ROM”的重复内存带宽消耗
- 对大型 NES 2.0 现代 Mapper 可跳过不必要的 legacy MD5 流扫描
- Code/Data Logger 大 ROM 缓冲在 ROM Close 时释放
- ROM 关闭后执行 Heap / Working Set 收缩，减少任务管理器残留占用

这一系列修改主要用于改善：

- 32MB
- 64MB
- 128MB
- 256MB
- 512MB

ROM 的打开速度、峰值内存占用和关闭后的内存归还。

---

### 9. OneBus / VTxx 支持

当前维护线已经加入并持续完善 OneBus / VTxx 相关支持。

#### VT03 / VT02

已完成或修正：

- VT03 renderer
- VT02 / VT03 菜单与 HUD 颜色
- Timer IRQ
- DMA / CPU IRQ
- PCM 相关基础支持
- RetroGame / Samuri 等兼容性问题修正

#### VT09

- 基本显示支持
- 与 VT03 / VT32 共用的 HUD / 颜色逻辑修正

#### VT32

已加入：

- CPU / PPU 扩展
- PCM / DMA / IRQ
- packed graphics
- HUD 修正
- 动态图形行为
- opcode encryption / Debugger Guard
- 部分菜单移动时杂线和图像问题修正

#### VT4FFx

- OneBus 扩展相关兼容路径
- 与 VT03 / VT32 / VT369 共用的 CPU / PPU / opcode 基础设施

#### VT369

已完成多个阶段的移植和实验性支持，包括：

- Stage 1：CPU + Protection + RGB555 / PPU 基础增强
- Stage 2：8bpp Background / Sprite Renderer
- Enhanced Sprite / OAM
- 64-byte NameTable
- 16-bit Tile Number
- 特殊 VRAM DMA
- 高分辨率相关路径
- Cube Tech / NubSup 启动与图形兼容实验
- HUD / Overlay / Direct RGB555
- ROM Close / frame ownership 清理
- OAM512 / 128 Sprite
- Standard Packed address alignment

VT369 仍属于持续完善中的扩展硬件支持，部分特殊 ROM 可能仍存在图形、时序或 PCM 兼容性问题。

---

### 10. OneBus / VTxx 性能优化

为了避免加入 VT03 / VT32 / VT4FFx / VT369 后影响普通 NES 游戏，当前版本加入了多层 Fast Path。

#### 普通 NES Fast Path

- 普通 CPU 不承担 VT opcode / multiplier 额外成本
- 普通 PPU 不进入 VT tile / packed renderer 判断
- 普通视频帧不处理 VT369 Direct RGB555 / Overlay
- VT369 专用 framebuffer / scanline side buffer 只在需要时处理

#### OneBus Active Fast Path

- Dynamic CPU Hook
- 只有 PCM / ALU / VT369 Sound HLE 活跃时才安装 OneBus CPU cycle hook
- Power-of-Two CHR fast address
- 常见 2 的幂 CHR 容量使用 `address & mask` 代替 `% size`
- 非 2 的幂 ROM 自动回退原取模逻辑

---

### 11. Mapper 与兼容性更新

本版本包含大量 Mapper 兼容性修正和新增支持。

长期维护过的 Mapper 包括但不限于：

`242`, `270`, `296`, `343`, `351`, `376`, `408`, `412`, `425`, `438`, `445`, `447`, `449`, `500`, `511`, `556`, `598`, `599`

此外还对以下方向进行了持续适配：

- MMC1
- MMC3
- MMC5
- N163
- VRC 系列
- OneBus
- VTxx
- 特殊多合一卡
- Split-ROM
- PRG / CHR 大容量映射
- Mirroring
- IRQ
- 扩展音频
- 特殊寄存器响应

---

### 12. AVI 录制

录制相关改进包括：

- NSF 播放界面录制
- NTSC 2x / PAL 3x 输出
- 自定义输出尺寸支持
- 录制过程中的画面 / 声音同步修正
- 高速模式与 AVI Dump 隔离，避免 Turbo / Frameskip 破坏录制确定性

具体可用功能取决于当前 Win32 构建配置。

---

## 编译

本项目仍基于 FCEUX 2.6.6 的源代码结构。

Windows 版本建议使用 Visual Studio 2022 或与 FCEUX 2.6.6 Win32 工程兼容的 Visual Studio 环境。

基本流程：

1. 获取完整源代码。
2. 打开 Win32 / Visual Studio 工程。
3. 确认 Windows SDK、DirectX / DirectInput 等依赖可用。
4. 推荐选择 `Release | x64`。
5. 编译生成 FCEUX 可执行文件。

Release 构建建议保持：

- `/O2`
- `/Oi`
- `/Ob2`
- `/GL`
- `/LTCG`
- Favor Speed
- Whole Program Optimization

---

## 使用建议

升级版本后建议重新检查：

- Input
- Hotkeys
- Sound
- Video
- NSF Playback
- Background Input
- Debugger Bank Size
- Turbo / Frame Skip
- Filter / Hardware Acceleration

如果旧配置文件中保存了已经不存在的手柄设备，建议重新进入 Hotkeys 或 Input 设置进行绑定。

为了获得更高的 Tab Turbo 峰值性能，可适当减少：

- 视频滤镜
- 复杂缩放
- 硬件加速额外开销
- Debugger / PPU Viewer / Trace Logger 等实时工具窗口

---

## 已知限制

- M1 / M2 / M3 / M4 等厂商自定义背键是否可用，取决于驱动是否将其作为独立 DirectInput Button 暴露。
- 部分极少见 Mapper / 多合一 ROM 仍可能存在兼容性问题。
- NSF 扩展音源组合较多，不同文件可能使用非常规寄存器写法。
- VT369 仍属于持续完善中的实验性支持，部分 ROM 可能存在图形、HUD、PCM 或时序差异。
- 512MB 级 ROM 的实际加载性能仍受磁盘速度、Windows 文件缓存和杀毒软件扫描影响。
- 本项目属于非官方 FCEUX 分支，不代表 FCEUX 官方行为。

---

## 问题反馈

提交问题时建议附带：

- ROM / NSF 的准确名称
- Mapper / Submapper
- 使用的 FCEUX 构建版本
- 是否启用 New PPU
- 是否属于 VT03 / VT09 / VT32 / VT369
- 重现步骤
- Debugger / Trace Logger 信息
- 是否开启 Filter / Hardware Acceleration
- 必要时附带存档、FM2、NSF、WAV 或截图

---

## 致谢

感谢 FCEUX 原项目及其历代开发者。

本项目中的大量功能仍建立在 FCEUX 原有 CPU / PPU 模拟、APU、Debugger、Trace Logger、TAS、Movie、NSF 与 Mapper 基础设施之上。

同时感谢相关 NES / OneBus / VTxx 文档、模拟器实现和测试 ROM 对硬件行为研究提供的参考。

---

## License

本项目基于 FCEUX 源代码派生，并遵循原 FCEUX 项目及各源文件所声明的许可条款。

请参考源代码文件头和 FCEUX 上游项目获取完整许可证信息。
