# FCEUX 2.8.0-Y330

> An unofficial enhanced fork maintained on top of **FCEUX 2.6.6**, focused on emulator compatibility, NES/VTxx hardware support, NSF playback, PPU/video accuracy, input handling, debugger usability, fast-forward performance, large-ROM support, and practical recording features.

## About

FCEUX 2.8.0-Y330 is a long-term custom branch derived from the FCEUX 2.6.6 source tree.

The goal is not to rewrite FCEUX from scratch. Instead, this branch keeps the traditional Win32 workflow and compatibility where possible while fixing practical issues found during real-world use.

Current development focuses on:

- NSF / NSFe player improvements
- PPU, Color 512 and video fixes
- DirectInput gamepad and hotkey improvements
- Tab Turbo / 3200% / 6400% fast-forward optimization
- Debugger features and scroll-performance improvements
- UTF-8 / CJK / special-character filename support
- 32MB–512MB large-ROM loading and memory-release optimization
- OneBus / VT03 / VT09 / VT32 / VT4FFx / VT369 support
- Mapper, expansion-audio and multicart compatibility
- AVI / NSF-player recording and display improvements

---

## Main Features

### 1. Enhanced NSF / NSFe Player

Current features include:

- NSF and NSFe playback
- Song / artist / copyright information
- NSFe track labels
- Track time and fade information
- Play / Pause / Stop controls
- Waveform display
- Virtual keyboard display
- Peak / RMS meters
- Expansion-audio status display
- Gamepad control in the NSF player
- Multiple expansion-audio chips in one NSF

Expansion-audio compatibility work includes:

- VRC6
- VRC7
- FDS
- MMC5
- Namco 106 / N163
- Sunsoft FME-7 / 5B

Several mixed-expansion NSF issues were also fixed, including cases where a later sound core overwrote another one during initialization or where the virtual keyboard did not match the actual audio state.

### 2. NSF Custom Playback Rates

The NSF header fields:

- `NTSCspeed`
- `PALspeed`

are used as the actual PLAY-call intervals.

The older frame-accumulation approach could produce:

- two PLAY calls in one video frame
- no PLAY call in another frame
- audible one-frame jumps or holds after long playback

Typical values include:

- `40FF` (16639 µs)
- `4165` (16741 µs)

The current implementation uses finer-grained PLAY scheduling so NSF PLAY calls are no longer forced onto whole video-frame boundaries.

---

### 3. PPU / Color 512 / Video Fixes

The NEWPPU and Color 512 paths include fixes for:

- per-dot `$2001` state handling
- per-dot grayscale
- per-dot color emphasis
- correct emphasis return to zero
- forced-blank palette handling
- preventing later `$2007` writes from changing already-recorded pixels
- rendering flags that were previously handled only in 8-pixel blocks
- normal integer 2x / 3x paths not reading `XDBuf` correctly
- color-emphasis brightness
- VT369 RGB555 / direct-color output
- VT369 HUD / overlay lifecycle and ROM-close cleanup

Video work also includes:

- improved NTSC 2x output
- PAL 3x output compatibility
- Digital Prime palette adjustments
- Color 512 output fixes
- integer 2x / 3x scaling fixes
- grayscale and emphasis corrections

---

### 4. Gamepad, Hotkeys and Input

The Win32 build includes DirectInput gamepad hotkey support for:

- normal buttons
- X / Y / Z axes
- Rx / Ry / Rz axes
- POV / Hat input

Commands such as the following can be bound:

- Pause
- Reset
- Save State
- Load State
- Save Slot
- Screenshot
- Frame Advance
- Turbo

One-shot and held commands are handled separately:

- Pause / Reset / Screenshot / Save / Load: fire once per press
- Frame Advance / Turbo: keep held behavior

Controller bindings retain the device GUID to improve device matching after restarting the emulator.

Input fixes include:

- clicks inside the emulator being detected as `JS 0 axis x+ / x-`
- failed DirectInput `GetDeviceState()` calls producing false zero-axis input
- desktop/taskbar/other-window clicks triggering hotkeys
- the button used in the hotkey dialog firing immediately after leaving the dialog
- excessive hotkey-table scanning during Turbo

The Turbo input path now includes:

- early return for empty hotkey mappings
- no joystick check for keyboard-only mappings
- keyboard-state generation caching
- O(1) hotkey fast path during stable Turbo
- reduced full input polling during high-speed execution

> Note: vendor-specific M1 / M2 / M3 / M4 rear buttons are only available when the controller driver exposes them as independent DirectInput buttons.

---

### 5. Tab Turbo and High-Speed Performance

Multiple optimization passes were made for Tab Turbo and manual high-speed execution.

Supported modes include:

- normal Turbo
- 3200%
- 6400%

Major improvements include:

- normal-NES CPU fast path
- avoiding unnecessary divide/modulo work when CPU multiplier is 1
- immediate return when opcode encryption is disabled
- dedicated `pputile_novt.inc` path for normal NES rendering
- non-VT games skipping VT369 scanline / RGB555 / overlay buffers
- normal frames skipping VT369 per-pixel overlay queries
- reduced Hotkey / Keyboard / DirectInput overhead during Turbo
- muteTurbo DirectSound steady-state bypass
- reduced heavy-tool refresh rate during high-speed execution
- separate 3200% / 6400% video refresh policies
- Smooth Turbo behavior to avoid “frozen-looking” frame skipping

With filters and extra hardware-acceleration overhead disabled, ordinary games can reach roughly the 4000-FPS range on suitable hardware. Actual performance depends on CPU, video settings, filters, scaling mode and the game itself.

---

### 6. Debugger Improvements

The Debugger received both feature and performance work.

#### Bank Size

Disassembly bank display supports:

- 32KB
- 16KB
- 8KB

The selected bank size is used by:

- Bank:Address display
- Symbolic Debug pages
- hover Bank display
- `K==#xx` breakpoint conditions
- address conversion logic

#### Debugger Scroll Performance

The disassembly view was optimized so scrolling no longer stalls game emulation:

- high-frequency `WM_VSCROLL / WM_MOUSEWHEEL` events are coalesced
- live scrolling preview is limited to about 33Hz
- full `HighlightSyntax()` is skipped while scrolling
- Symbolic name/comment expansion is skipped during preview
- the Code/Data Logger side column is not rebuilt during preview
- `SB_ENDSCROLL` does not trigger another redundant full refresh
- after roughly 120ms of scroll inactivity, one full symbolic/highlighted view is committed
- paused / single-step mode keeps immediate full updates

This allows the mouse wheel or scrollbar thumb to move through disassembly without causing a large drop in game FPS.

#### Other Debugger Fixes

- `$FFFF` disassembly overflow fix
- VT opcode Debugger Guard
- VT/OneBus opcode-decryption debug support
- Chinese Debugger UI compatibility retained

---

### 7. UTF-8 and Filename Support

The Win32 file-I/O path has been updated for better UTF-8 / Unicode compatibility.

ROMs, saves and project files can use filenames containing:

- Simplified Chinese
- Traditional Chinese
- Japanese
- Unicode special characters
- historical CJK filenames that previously caused `Error Opening`

UTF-8-aware wrappers are used across common ROM, archive, cheat, AVI, TAS Editor and save-file paths.

---

### 8. 32MB–512MB Large-ROM Support and Optimization

Modern multicarts and test ROMs up to 512MB are supported with dedicated large-file optimizations.

Main changes include:

- Large-ROM fast path beginning at 32MB
- plain `.nes` files can bypass unnecessary 7-Zip full-format signature scanning
- uninitialized large buffers avoid clearing 32–512MB before reading
- large Windows buffers use `VirtualAlloc`
- ROM Close uses `VirtualFree(MEM_RELEASE)`
- 8MB / 16MB chunked streaming reads
- CRC / hash processing during file reads
- reduced duplicate full-ROM memory-bandwidth passes
- large modern NES 2.0 mappers may skip unnecessary legacy MD5 streaming
- Code/Data Logger large-ROM buffers are released on ROM Close
- heap / working-set trimming after ROM close reduces residual Task Manager usage

These optimizations target:

- 32MB
- 64MB
- 128MB
- 256MB
- 512MB

ROM loading time, peak memory usage and memory return after closing a game.

---

### 9. OneBus / VTxx Support

The current branch contains ongoing OneBus / VTxx compatibility work.

#### VT03 / VT02

Implemented or corrected areas include:

- VT03 renderer
- VT02 / VT03 menu and HUD colors
- Timer IRQ
- DMA / CPU IRQ
- PCM foundations
- compatibility fixes for titles such as RetroGame and Samuri

#### VT09

- basic display support
- HUD / color fixes shared with VT03 / VT32 paths

#### VT32

Current work includes:

- CPU / PPU extensions
- PCM / DMA / IRQ
- packed graphics
- HUD corrections
- dynamic graphics behavior
- opcode encryption / Debugger Guard
- fixes for menu-movement artifacts and display issues

#### VT4FFx

- OneBus extension compatibility paths
- shared CPU / PPU / opcode infrastructure with VT03 / VT32 / VT369

#### VT369

Multiple stages of experimental VT369 support are included:

- Stage 1: CPU + Protection + RGB555 / PPU foundations
- Stage 2: 8bpp background / sprite renderer
- enhanced Sprite / OAM
- 64-byte NameTable
- 16-bit tile numbers
- special VRAM DMA
- high-resolution-related paths
- Cube Tech / NubSup boot and graphics experiments
- HUD / Overlay / Direct RGB555
- ROM-close / frame-ownership cleanup
- OAM512 / 128-sprite support
- standard packed-address alignment

VT369 remains an actively developed hardware target. Some unusual ROMs may still have graphics, timing or PCM differences.

---

### 10. OneBus / VTxx Performance

To prevent VT03 / VT32 / VT4FFx / VT369 support from slowing down normal NES games, several fast paths were added.

#### Normal NES Fast Path

- normal CPU execution avoids VT opcode / multiplier overhead
- normal PPU rendering avoids VT tile / packed-renderer branches
- normal video frames skip VT369 direct-RGB555 / overlay processing
- VT369 side buffers are touched only when required

#### OneBus Active Fast Path

- Dynamic CPU Hook
- the OneBus CPU-cycle hook is installed only while PCM / ALU / VT369 Sound HLE work is active
- Power-of-Two CHR fast addressing
- common power-of-two CHR sizes use `address & mask` instead of `% size`
- non-power-of-two ROMs automatically retain modulo fallback behavior

---

### 11. Mapper and Compatibility Updates

This branch contains a large number of mapper additions and fixes.

Mappers that have received work include, but are not limited to:

`242`, `270`, `296`, `343`, `351`, `376`, `408`, `412`, `425`, `438`, `445`, `447`, `449`, `500`, `511`, `556`, `598`, `599`

Additional ongoing work covers:

- MMC1
- MMC3
- MMC5
- N163
- VRC series
- OneBus
- VTxx
- special multicarts
- split-ROM layouts
- large PRG / CHR mappings
- mirroring
- IRQ behavior
- expansion audio
- special register behavior

---

### 12. AVI Recording

Recording-related improvements include:

- NSF-player UI recording
- NTSC 2x / PAL 3x output
- custom output-size support
- A/V synchronization fixes
- isolation of AVI dumping from Turbo / Frameskip so recording remains deterministic

Available options may depend on the current Win32 build.

---

## Building

The project still follows the FCEUX 2.6.6 source layout.

For Windows, Visual Studio 2022 or another Visual Studio environment compatible with the original FCEUX 2.6.6 Win32 project is recommended.

Basic steps:

1. Obtain the full source tree.
2. Open the Win32 / Visual Studio solution.
3. Make sure the Windows SDK and DirectX / DirectInput dependencies are available.
4. `Release | x64` is recommended.
5. Build the FCEUX executable.

Recommended Release settings include:

- `/O2`
- `/Oi`
- `/Ob2`
- `/GL`
- `/LTCG`
- Favor Speed
- Whole Program Optimization

---

## Usage Notes

After upgrading, review:

- Input
- Hotkeys
- Sound
- Video
- NSF Playback
- Background Input
- Debugger Bank Size
- Turbo / Frame Skip
- Filter / Hardware Acceleration

If old configuration files reference controllers that are no longer present, rebind them in the Input or Hotkeys dialog.

For maximum Tab Turbo throughput, consider reducing:

- video filters
- expensive scaling
- additional hardware-acceleration overhead
- continuously updating Debugger / PPU Viewer / Trace Logger windows

---

## Known Limitations

- Vendor-specific M1 / M2 / M3 / M4 rear buttons are only usable when exposed as independent DirectInput buttons.
- Some uncommon mappers and multicarts may still require additional compatibility work.
- Mixed-expansion NSF files can use unusual register behavior and may require file-specific handling.
- VT369 remains experimental; some ROMs may still show graphics, HUD, PCM or timing differences.
- 512MB-class ROM loading still depends heavily on disk speed, Windows file cache and antivirus scanning.
- This is an unofficial FCEUX fork and does not represent official FCEUX behavior.

---

## Bug Reports

When reporting an issue, include:

- exact ROM / NSF name
- Mapper / Submapper
- FCEUX build/version
- whether New PPU is enabled
- whether the title uses VT03 / VT09 / VT32 / VT369
- reproduction steps
- Debugger / Trace Logger information
- whether filters / hardware acceleration are enabled
- savestate, FM2, NSF, WAV or screenshots when useful

---

## Credits

Thanks to the original FCEUX project and all of its contributors.

This branch continues to rely heavily on FCEUX's CPU / PPU emulation, APU, Debugger, Trace Logger, TAS, Movie, NSF and Mapper infrastructure.

Thanks also to the NES / OneBus / VTxx documentation, emulator implementations and test ROMs that help with hardware behavior research.

---

## License

This project is derived from FCEUX and follows the licensing terms of the original FCEUX source code and individual source files.

Refer to the source headers and the upstream FCEUX project for complete licensing information.
