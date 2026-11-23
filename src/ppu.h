void FCEUPPU_Init(void);
void FCEUPPU_Reset(void);
void FCEUPPU_Power(void);
int FCEUPPU_Loop(int skip);

void FCEUPPU_LineUpdate();
void FCEUPPU_SetVideoSystem(int w);

extern void (*PPU_hook)(uint32 A);
extern void (*GameHBIRQHook)(void), (*GameHBIRQHook2)(void);

int newppu_get_scanline();
int newppu_get_dot();
void newppu_hacky_emergency_reset();

/* For cart.c and banksw.h, mostly */
extern uint8 NTARAM[0x800], * vnapage[4];
extern uint8 PPUNTARAM;
extern uint8 PPUCHRRAM;

void FCEUPPU_SaveState(void);
void FCEUPPU_LoadState(int version);
uint32 FCEUPPU_PeekAddress();
uint8* FCEUPPU_GetCHR(uint32 vadr, uint32 refreshaddr);
int FCEUPPU_GetAttr(int ntnum, int xt, int yt);
void ppu_getScroll(int& xpos, int& ypos);


#ifdef _MSC_VER
#define FASTCALL __fastcall
#else
#define FASTCALL
#endif

void PPU_ResetHooks();
extern uint8(FASTCALL* FFCEUX_PPURead)(uint32 A);
extern void (*FFCEUX_PPUWrite)(uint32 A, uint8 V);
/* Optional CPU PPUDATA ($2007) bus overrides. Unlike FFCEUX_PPURead/Write,
 * these callbacks are not used by renderer fetches. They let SoC mappers
 * model CPU-only video address extensions without corrupting the normal PPU
 * rendering path. */
extern uint8(FASTCALL* FFCEUX_PPUReadCPU)(uint32 A);
extern void (*FFCEUX_PPUWriteCPU)(uint32 A, uint8 V);

/* Optional renderer-side extended pattern bus.  VT03/OneBus uses attribute/OAM
 * bits as per-tile address extension and may expose four pattern planes.
 * A is the normal NES pattern address of plane 0 for the requested row;
 * attr is the nametable attribute (BG) or OAM attribute (sprite). */
extern uint8(FASTCALL* FFCEUX_PPUReadPatternExt)(uint32 A, uint8 attr, uint8 isSprite, uint8 plane);
/* VT32 has a packed-color renderer that is selected only when its extended
 * mode register is exactly $80.  Keep it separate from the ordinary VT03+
 * four-plane path so VT02/VT03/VT09 timing and addressing stay untouched.
 * GetPackedMode returns 0 normally, 1 for packed 8bpp BG, 2 for dual-layer
 * packed 4bpp BG.  ReadPackedBG returns the final 8-bit palette pen for one
 * pixel of the current tile; ReadPackedSprite returns one packed sprite byte
 * (two 4bpp pixels, low nibble first). */
extern uint8(FASTCALL* FFCEUX_PPUGetPackedMode)(void);
extern uint8(FASTCALL* FFCEUX_PPUReadPackedBG)(uint32 refreshAddr, uint8 pixel);
extern uint8(FASTCALL* FFCEUX_PPUReadPackedSprite)(uint8 tile, uint8 row, uint8 byteIndex);
/* Returns the live extended-video mode register ($2010 on VTxx).  A null
 * callback means the standard 2C02 renderer should be used. */
extern uint8(FASTCALL* FFCEUX_PPUGetExtMode)(void);
/* Map a VT renderer pen to a normal 6-bit 2C02 color when COLCOMP is off.
 * This avoids consuming FCEUX output-palette slots $80-$FF, which are also
 * used by emulator OSD/HUD drawing. */
extern uint8(FASTCALL* FFCEUX_PPUMapExtPalette)(uint8 pen);
/* Optional notification for emulator overlays. Returns non-zero while the
 * cartridge owns FCEUX output-palette slots $80-$FF (for example VT03
 * COLCOMP). OSD code can then use the fixed low GUI palette instead of
 * colliding with game colors. */
extern uint8(FASTCALL* FFCEUX_PPUUseFixedOSD)(void);
/* Optional request from overlay code to restore the current game-owned
 * extended palette after temporary OSD palette mirroring. */
extern void(FASTCALL* FFCEUX_PPURefreshExtPalette)(void);

/* VT369 Stage 2 enhanced video hooks.  These are deliberately separate from
 * the VT03/VT32 hooks above: $201E selects the VT3xx packed renderer, while
 * $2008 extends OAM addressing to 9 bits.  A null callback preserves the
 * ordinary 2C02/VT03 paths byte-for-byte.
 * GetVT369Mode bit 0 = standard-resolution enhanced renderer active;
 * bit 1 = packed 8bpp background; bit 2 = expanded OAM table format;
 * bit 3 = high-resolution compatibility renderer; bit 4 = VT369 special
 * 64-byte name-table / 16-bit tile-number mode. */
extern uint8(FASTCALL* FFCEUX_PPUGetVT369Mode)(void);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369Reg)(uint8 reg);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369BG)(uint32 address, uint8 color, uint8 pixel);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369BG16)(uint16 tile, uint8 row, uint8 pixel);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369Sprite)(uint16 tile, uint8 row, uint8 pixel, uint8 bpp, uint8 alt16, uint8 rightTable);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369HiResSprite)(uint16 tile, uint8 width, uint8 height, uint8 row, uint8 pixel, uint8 bpp);
/* low byte=$201D sprite geometry/count, high byte=$201C packed-format control. */
extern uint16(FASTCALL* FFCEUX_PPUGetVT369SpriteConfig)(void);
extern uint8(FASTCALL* FFCEUX_PPUReadVT369OAM)(uint16 index);
extern uint8(FASTCALL* FFCEUX_PPUGetVT369OAMHigh)(void);
extern void(FASTCALL* FFCEUX_PPUWriteVT369OAM)(uint8 low, uint8 value);

extern uint8 FASTCALL FFCEUX_PPURead_Default(uint32 A);
void FFCEUX_PPUWrite_Default(uint32 A, uint8 V);
/* VT3x palette-DMA quirk: when a compensated transfer starts at $3F01,
 * hardware writes the first byte to $3F00. Returns nonzero when applied. */
int FCEUX_PPUApplyVTCGRAMDMABug(void);
/* VT369 special VRAM DMA latches its destination from the two $2006 writes. */
uint16 FCEUX_PPUGetVT369DMAVRAMTarget(void);
void FCEUX_PPUSetVT369DMAVRAMTarget(uint16 target);

extern int g_rasterpos;
extern uint8 PPU[4];

// When non-zero, the renderer respects the PPU $2001 mask register.
// When zero, the video output forces background/sprite visibility for debugging/recording.
extern int FCEUPPU_MaskEnabled;
extern bool DMC_7bit;
extern bool paldeemphswap;

#ifndef _FCEU_PPU_H
#define _FCEU_PPU_H

enum PPUPHASE {
	PPUPHASE_VBL, PPUPHASE_BG, PPUPHASE_OBJ
};

extern PPUPHASE ppuphase;

#endif

extern unsigned char* cdloggervdata;
extern unsigned int cdloggerVideoDataSize;
extern volatile int rendercount, vromreadcount, undefinedvromcount;
