/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2007-2010 CaH4e3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * VR02/VT03 Console and OneBus System
 *
 * Street Dance (Dance pad) (Unl)
 * 101-in-1 Arcade Action II
 * DreamGEAR 75-in-1, etc.
 *
 */

#include "mapinc.h"
#include "../ines.h"
#include "../palette.h"
#include "../driver.h"
#include <math.h>

static uint8 submapper;
static void (*Sync)(void);
static uint8 *CHRRAM;
static uint32 CHRRAMSIZE;
/* VROM may point to either CHR-ROM or the CHR-RAM buffer allocated by the
 * iNES loader.  OneBus needs to know whether the cartridge actually contains
 * CHR-ROM, because boards without CHR-ROM use PRG-ROM as their CHR source.
 * CartInfo::CHRRomSize is the logical CHR-ROM byte count and is not confused
 * by a writable VROM/CHR-RAM allocation. */
static uint8 OneBusHasCHRROM;

/* Stage 4.2 PPU bus state. CSync records the board-specific final CHR mask
 * and OR value so CPU $2007 address-extension transfers can use exactly the
 * same outer mapping as the ordinary 1 KiB VPage path. */
static uint32 OneBusCHRAnd = 0x7FFF;
static uint32 OneBusCHROr = 0;
/* Some OneBus variants temporarily map a private/internal RAM chip instead
 * of chip 0. CPU $2007 must then use the already-established VPage mapping. */
static uint8 OneBusUseMappedCHR = 0;
static uint8 (FASTCALL *OneBusOldPPUReadCPU)(uint32 A) = NULL;
static void (*OneBusOldPPUWriteCPU)(uint32 A, uint8 V) = NULL;
/* VT03 has 256 bytes of palette RAM: pens $00-$7F use the low half and,
 * in COLCOMP mode, $80-$FF supply the upper six bits of the 12-bit color. */
static uint8 OneBusCGRAM[0x200];
static uint8 OneBusOutputPaletteDirty = 0;
/* Hardware-generation capabilities derived from NES 2.0 extended console type
 * with submapper fallback for older/malformed headers. */
static uint8 OneBusHas4BPP = 1;
static uint8 OneBusHasEnhancedPalette = 1;
static uint8 OneBusHas16BitVideoBus = 0;
enum { ONEBUS_PAL_VT03 = 0, ONEBUS_PAL_RGB444 = 1, ONEBUS_PAL_RGB555 = 2 };
enum { ONEBUS_AUDIO_VT03 = 0, ONEBUS_AUDIO_VT32 = 1, ONEBUS_AUDIO_VT369 = 2 };
static uint8 OneBusPaletteFormat = ONEBUS_PAL_VT03;
static uint8 OneBusAudioGeneration = ONEBUS_AUDIO_VT03;
static uint8 OneBusIsMapper296 = 0;
/* VT369 Stage 0 capabilities are deliberately kept separate from the VT32
 * audio generation.  VT369 reuses the arithmetic unit but later models have
 * their own sound CPU/APU, so treating it as VT32 audio would be incorrect. */
static uint8 OneBusIsVT369 = 0;
static uint8 OneBusHasALU = 0;
static uint8 VT369ExtraRAM[0x800];
static uint8 VT369SoundRAM[0x800];
static uint8 VT369Relative[2];
/* VT369 Stage 1: newer PPU register shadow ($2020-$207F), direct palette
 * window ($5000-$53FF / PPU $3C00-$3FFF), and $4169 protection latch. */
static uint8 VT369PPURegs[0x60];
static uint8 VT369PaletteRAM[0x400];
/* Stage 2 keeps VT369's 512-byte OAM private to OneBus.  This avoids changing
 * the global 2C02 SPRAM size and therefore cannot perturb ordinary NES boards. */
static uint8 VT369OAM[0x200];
static uint8 VT369OAMHigh;
static uint8 VT369Protection4169;
/* Stage 2.2.1 bootstrap compatibility for VT369 sets (notably NubSup Mini
 * Game Fan).  Real VT369 hardware contains a second sound CPU.  Until that
 * processor is emulated, a tiny HLE keeps the documented shared-RAM
 * handshake alive so the main CPU does not deadlock before reaching the
 * menu.  GPIO/system registers below are ordinary VT369 latch/readback
 * storage; model-specific external devices remain future work. */
static uint8 VT369SoundControl4162;
static uint8 VT369SoundHLEActive;
static uint32 VT369SoundHLECycles;       // fallback/unknown-program handshake timer
static uint32 VT369SoundPhase6;          // sound timer phase in 1/6 base CPU cycles
static uint32 VT369SoundIRQAcc;          // fractional $18F6 timer-IRQ counter
static uint16 VT369SoundPeriodN;         // $0293 stream timer period
static int16 VT369SoundSample;           // current HLE DAC sample, signed 16-bit
static uint8 VT369SoundStartPrev;
static uint8 VT369Sound3Left[3];
static uint32 VT369Sound3Frame[3][2];
static uint8 VT369PeripheralCycleRemainder;
static uint8 VT369GPIO414x[0x10];
static uint8 VT369GPIO415x[0x10];
static uint8 VT369GPIO41Ex[0x10];
/* Stage 2 forward declarations used by packed fetch helpers. */
static uint8 Mapper296CHRDecrypt(uint8 v);
static void VT369WritePaletteByte(uint32 off, uint8 V);
#define ONEBUS_IVR_CHIP 0x11
#define MAPPER296_CHR_CHIP 0x12

/* General Purpose Registers */
static uint8 cpu410x[64], ppu201x[16], apu40xx[64], reg4242, dipswitch;
static const uint8 *cpuMangle, *ppuMangle, *mmc3Mangle;

/* IRQ Registers */
static uint8 IRQCount, IRQa, IRQReload;
static uint8 IRQPending;
#define IRQLatch cpu410x[0x1]
#define ONEBUS_TIMER_IRQ FCEU_IQEXT
#define ONEBUS_PCM_IRQ   FCEU_IQEXT2

/* MMC3 Registers */
#define mmc3cmd  cpu410x[0x5]	/* pcv- ----, p - program swap, c - video swap, v - internal VRAM enable */
#define mirror   cpu410x[0x6]	/* ---- ---m, m = 0 - H, m = 1 - V */

/* VTxx DWS / PCM engine.
 *
 * $4010-$4013 are the VT speech/DMA registers, not the RP2A03 DMC registers.
 * The old implementation forwarded these writes to the stock DMC and then
 * used the DMC DAC as a 7-bit PCM sink.  That loses the VT 8-bit DAC path,
 * ignores DWS, and leaves the sample clock fixed.  Keep the VT engine here,
 * but feed its output through FCEUX's expansion-sound mixer callbacks.
 */
static uint8 pcm_enable = 0, pcm_irq = 0;
static uint16 pcm_addr = 0, pcm_size = 0;
/* Retained state tags from the earlier PCM prototype for save-state layout
 * compatibility. pcm_latch is no longer a countdown; pcm_clock is the
 * rounded CPU-cycle period corresponding to the selected SD rate. */
static int16 pcm_latch = 0, pcm_clock = 0xE1;
static uint16 pcm_start_addr = 0, pcm_start_size = 0;
static uint64 pcm_phase = 0; /* CPU cycles * selected sample rate */
static uint8 pcm_end_pending = 0;
static uint8 pcm_dws_shift = 0, pcm_dws_bits_left = 0;
static uint8 pcm_dws_initial = 0, pcm_dws_amplitude = 0;
static uint8 pcm_output = 0;
static uint32 OneBusAudioHiPosition = 0;
static uint32 OneBusAudioLQPosition = 0;

static const uint16 OneBusDWSSampleRates[16] = {
	4200, 4700, 5300, 5500, 6200, 7000, 7900, 8400,
	9000, 11000, 13000, 14000, 17000, 21000, 25000, 33000
};

/* VT31/VT32 two-channel PCM/ADPCM engine.
 *
 * R3 follows the user-supplied NRS NintendoEmulator APU_VT32 model and the
 * actual encrypted DGUN2573 access sequence.  The two channels capture one
 * shared physical-ROM address latch when their bit in $4033 rises.  $4032 is
 * the common sample period in CPU cycles (DGUN software normally programs
 * $6F=111); it is NOT a channel-1 volume register.  Both channels use the
 * fixed NRS gain of 128, so Channel::output == sample*8.
 *
 * This matters for real software: treating $4032 as a channel selector made
 * subsequent $4012/$4035/$4036 writes program the wrong channel, which sent
 * playback into unrelated PRG data.  Combined with signed samples being added
 * directly to FCEUX's packed HQ WaveHi word, that produced the severe full-
 * scale clipping seen in the user's DGUN2573 WAV capture. */
static uint32 vt32_pcm_programmed[2], vt32_pcm_address[2];
static int32 vt32_pcm_count[2];
static uint32 vt32_pcm_period[2] = { 0x6F, 0x6F };
static int16 vt32_pcm_sample[2];
static uint8 vt32_pcm_volume[2] = { 128, 128 };
static uint8 vt32_pcm_playing[2], vt32_pcm_adpcm[2];
static int32 vt32_pcm_predictor[2], vt32_pcm_step_index[2], vt32_pcm_step_size[2];
static uint8 vt32_pcm_second_nibble[2], vt32_pcm_selected;
static uint32 vt32_alu_operand14;
static uint16 vt32_alu_operand56, vt32_alu_operand67;
static uint8 vt32_alu_busy;

/* OneBus Active Fast Path R1.
 * MapIRQHook used to stay installed for the entire lifetime of every OneBus
 * title even when no cycle-driven peripheral was active.  Keep the function
 * declaration here so PCM/ALU/Sound start/stop paths can dynamically arm it. */
static void UNLOneBusCpuHook(int a);
static void OneBusUpdateCpuHook(void);

/* Cached CHR wrap information.  Most commercial VT images expose power-of-two
 * CHR/unified-ROM sizes, so a hot per-pixel modulo can become one AND.
 * Non-power-of-two images retain the exact modulo fallback. */
static uint32 OneBusCHRWrapMask;
static uint32 OneBusCHRSplitWrapMask;
static uint8 OneBusCHRWrapPow2;
static uint8 OneBusCHRSplitWrapPow2;
static const int8 VT32ADPCMIndexTable[16] = {
	-1,-1,-1,-1,1,2,4,6,-1,-1,-1,-1,1,2,4,6
};
static const int16 VT32ADPCMStepTable[59] = {
	7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
	50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
	253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
	1060,1166,1282,1411,1552,1707,1878
};

/* VT369 Sound-CPU HLE. The real chip runs a second 6502, but its common
 * commercial sound programs are deterministic ADPCM mixers. This mirrors the
 * NRS/Furbtendulator/PocketVT HLE model while keeping it behind the VT369
 * capability flag so ordinary OneBus audio is untouched. */
static const int8 VT369Step[16][16] = {
	{0,14,28,42,56,70,84,97,-111,-97,-84,-70,-56,-42,-28,-14},
	{0,13,26,39,52,65,78,91,-104,-91,-78,-65,-52,-39,-26,-13},
	{0,11,21,32,43,54,64,75,-86,-75,-64,-54,-43,-32,-21,-11},
	{0,9,18,27,35,44,53,62,-71,-62,-53,-44,-35,-27,-18,-9},
	{0,7,13,20,27,34,40,47,-54,-47,-40,-34,-27,-20,-13,-7},
	{0,6,11,17,22,28,33,39,-44,-39,-33,-28,-22,-17,-11,-6},
	{0,5,9,14,18,23,27,32,-36,-32,-27,-23,-18,-14,-9,-5},
	{0,4,8,11,15,19,23,26,-30,-26,-23,-19,-15,-11,-8,-4},
	{0,3,6,9,12,15,17,20,-23,-20,-17,-15,-12,-9,-6,-3},
	{0,2,5,7,10,12,14,17,-19,-17,-14,-12,-10,-7,-5,-2},
	{0,2,4,6,8,10,12,14,-16,-14,-12,-10,-8,-6,-4,-2},
	{0,2,3,5,6,8,10,11,-13,-11,-10,-8,-6,-5,-3,-2},
	{0,1,2,4,5,6,7,9,-10,-9,-7,-6,-5,-4,-2,-1},
	{0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1},
	{0,1,2,3,3,4,5,6,-7,-6,-5,-4,-3,-3,-2,-1},
	{0,1,1,2,3,4,4,5,-6,-5,-4,-4,-3,-2,-1,-1}
};
static const uint8 VT1682IStep[4] = { 0,0,3,5 };
static const uint8 VT1682ITab[26] = { 0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,20,20,20,20 };
static const int8 VT1682Step[4][21] = {
	{0,1,1,1,1,1,2,2,2,3,3,4,5,5,6,7,8,10,11,13,15},
	{1,3,3,3,4,4,6,6,7,9,10,12,15,16,19,22,25,30,34,40,46},
	{3,5,5,6,7,8,10,11,13,16,18,21,25,28,32,38,43,51,58,68,78},
	{4,7,7,8,10,11,14,15,18,22,25,29,35,39,45,53,60,71,81,95,109}
};

typedef struct { uint16 masks, period, loop; uint8 loopinc, stream; } VT369SoundProg;
static uint16 VT369SoundResetVector(void) {
	return (uint16)VT369SoundRAM[0x7F8] | ((uint16)VT369SoundRAM[0x7F9] << 8);
}
static int VT369SoundProgFor(uint16 rv, VT369SoundProg *p) {
	p->loop = 0; p->loopinc = 1; p->stream = 0;
	switch (rv) {
	case 0x0203: case 0x02A0: case 0x4E24: p->masks=0x04C; p->period=0x03B; p->loop=0x0FC; return 1;
	case 0x02E0: p->masks=0x04C; p->period=0x03B; p->loop=0x073; p->loopinc=4; return 1;
	case 0x0250: p->masks=0x04C; p->period=0x03B; p->loop=0x0FC; p->stream=1; return 1;
	case 0x1C4C: case 0x40AE: p->masks=0x0A4; p->period=0x06B; return 1;
	default: return 0;
	}
}
static uint32 VT369Read24(uint32 off) {
	return (uint32)VT369SoundRAM[off & 0x7FF] |
		((uint32)VT369SoundRAM[(off+1) & 0x7FF] << 8) |
		((uint32)VT369SoundRAM[(off+2) & 0x7FF] << 16);
}
static void VT369Write24(uint32 off, uint32 v) {
	VT369SoundRAM[off & 0x7FF]=(uint8)v;
	VT369SoundRAM[(off+1)&0x7FF]=(uint8)(v>>8);
	VT369SoundRAM[(off+2)&0x7FF]=(uint8)(v>>16);
}
static uint8 VT369PRGRead(uint32 a) {
	if (!PRGptr[0] || !PRGsize[0]) return 0xFF;
	return PRGptr[0][a % PRGsize[0]];
}

static void VT369SoundCommand0293(void) {
	const uint32 ch = VT369SoundRAM[0x7A3] % 3;
	switch (VT369SoundRAM[0x7A2]) {
	case 0x01:
		VT369SoundRAM[0x4B0+ch]=0xFF;
		VT369SoundRAM[0x4B7+ch*2]=0;
		VT369SoundRAM[0x180+ch*2]=VT369SoundRAM[0x181+ch*2]=0;
		VT369Sound3Left[ch]=0;
		VT369SoundRAM[0x7A2]=0; break;
	case 0x02: {
		const int16 p=(int16)((VT369SoundRAM[0x7A0] | ((uint16)VT369SoundRAM[0x7A1]<<8))*3);
		VT369SoundPeriodN=(uint16)(-p);
		VT369SoundRAM[0x7A2]=0; break;
	}
	case 0x03: VT369SoundRAM[0x4B0+ch]=0; VT369SoundRAM[0x7A2]=0; break;
	}
}
static int32 VT369SoundTick0293(void) {
	int32 mix=0;
	for (uint32 ch=0; ch<3; ++ch) {
		if (!VT369SoundRAM[0x4B0+ch]) continue;
		uint32 rd=VT369SoundRAM[0x4B7+ch*2]&0x78;
		uint32 left=VT369Sound3Left[ch];
		uint32 lo=VT369Sound3Frame[ch][0], hi=VT369Sound3Frame[ch][1];
		int pred=(int8)VT369SoundRAM[0x180+ch*2];
		uint32 idx=VT369SoundRAM[0x181+ch*2]; if (idx>20) idx=20;
		if (!left) {
			const uint32 base=ch*0x80 + rd;
			lo=(uint32)VT369SoundRAM[base] | ((uint32)VT369SoundRAM[base+1]<<8) |
				((uint32)VT369SoundRAM[base+2]<<16) | ((uint32)VT369SoundRAM[base+3]<<24);
			hi=(uint32)VT369SoundRAM[base+4] | ((uint32)VT369SoundRAM[base+5]<<8) |
				((uint32)VT369SoundRAM[base+6]<<16) | ((uint32)VT369SoundRAM[base+7]<<24);
			if (hi&0x80000000U) lo=hi=0;
			left=21; rd=(rd+8)&0x7F;
		}
		const uint32 code=lo&7;
		int d=VT1682Step[code&3][idx]; if (code&4) d=-d;
		pred+=d; if (pred<-128) pred=-128; if (pred>127) pred=127;
		idx=VT1682ITab[idx+VT1682IStep[code&3]];
		mix += pred<<7;
		lo=(lo>>3)|(hi<<29); hi>>=3; left--;
		VT369SoundRAM[0x4B7+ch*2]=(uint8)rd;
		VT369SoundRAM[0x180+ch*2]=(uint8)pred; VT369SoundRAM[0x181+ch*2]=(uint8)idx;
		VT369Sound3Frame[ch][0]=lo; VT369Sound3Frame[ch][1]=hi; VT369Sound3Left[ch]=(uint8)left;
	}
	// PocketVT uses gain 3 for the 3-bit streamed program.
	mix=(mix*3)>>9; if (mix>127) mix=127; if (mix<-128) mix=-128;
	return mix;
}

static int VT369SoundDecode48(uint32 ch, const VT369SoundProg *p, int32 *mix) {
	uint8 *d=&VT369SoundRAM[ch*8];
	uint32 pos=d[5], lead=d[0], frame=d[1];
	int last=(int8)d[3], out=(int8)d[4];
	const int vol=d[2]&0x7F;
	uint32 wa=VT369Read24(0x30+ch*4);
	uint32 swa=VT369SoundRAM[0x521+ch*2];
	const uint32 start=VT369Read24(0x60+ch*4);
	if (pos==48) {
		if (p->stream) {
			const uint32 rb=0x100+ch*0x100; lead=VT369SoundRAM[rb+(swa++&0xFF)]; frame=VT369SoundRAM[rb+(swa++&0xFF)];
		} else { lead=VT369PRGRead(wa++); frame=VT369PRGRead(wa++); }
		if (lead==0xFF) {
			const int loop=p->loop ? (VT369SoundRAM[p->loop+ch*p->loopinc]!=0) : 0;
			if (!loop) return 0;
			wa=start; last=out=0; pos=48; VT369Write24(0x30+ch*4,wa); d[3]=d[4]=0; d[5]=48; return 1;
		}
	} else if (!(pos&1)) {
		if (p->stream) { const uint32 rb=0x100+ch*0x100; frame=VT369SoundRAM[rb+(swa++&0xFF)]; }
		else frame=VT369PRGRead(wa++);
	}
	uint32 q=pos; if (q>=48) q=(q==48)?0:q%48;
	const uint32 nib=(q&1)?((frame>>4)&15):(frame&15);
	const int index=(int)lead-((q>=24&&(lead&0x40))?1:0)+((q>=24&&(lead&0x80))?2:0);
	const int step=VT369Step[index&15][nib];
	int o;
	switch ((lead>>4)&3) { case 0:o=step;break; case 1:o=step+out;break; case 2:o=step+out*2-last;break; default:o=step+out-(last>>1);break; }
	last=out; out=(int8)o; pos=q+1; *mix += out*vol;
	d[0]=(uint8)lead; d[1]=(uint8)frame; d[3]=(uint8)last; d[4]=(uint8)out; d[5]=(uint8)pos;
	if (p->stream) VT369SoundRAM[0x521+ch*2]=(uint8)swa; else VT369Write24(0x30+ch*4,wa);
	return 1;
}

static int32 VT369SoundTick48(const VT369SoundProg *p) {
	uint8 active=VT369SoundRAM[p->masks+1];
	const uint8 stop=VT369SoundRAM[p->masks+2];
	active &= (uint8)~stop;
	int32 mix=0;
	for (uint32 ch=0; ch<4; ++ch) {
		const uint8 bit=(uint8)(1U<<ch);
		if (!(active&bit)) continue;
		if (!VT369SoundDecode48(ch,p,&mix)) active &= (uint8)~bit;
	}
	VT369SoundRAM[p->masks+1]=active;
	// The sound program's timer IRQ increments $18F6 once per ~25.78 sample ticks.
	VT369SoundIRQAcc += 159;
	VT369SoundRAM[0x0F6]=(uint8)(VT369SoundRAM[0x0F6]+(VT369SoundIRQAcc>>12));
	VT369SoundIRQAcc &= 0xFFF;
	mix=(mix*6)>>9; if (mix>127) mix=127; if (mix<-128) mix=-128;
	return mix;
}

static uint16 VT369SoundCurrentPeriod(void) {
	const uint16 rv=VT369SoundResetVector();
	if (rv==0x0293) return VT369SoundPeriodN ? VT369SoundPeriodN : 768;
	VT369SoundProg p;
	if (VT369SoundProgFor(rv,&p)) {
		const int16 per=(int16)(VT369SoundRAM[p.period] | ((uint16)VT369SoundRAM[p.period+4]<<8));
		const uint16 n=(uint16)(-per); return n?n:768;
	}
	return 0;
}
static int VT369SoundProgramKnown(void) {
	const uint16 rv=VT369SoundResetVector(); VT369SoundProg p; return rv==0x0293 || VT369SoundProgFor(rv,&p);
}
static void VT369SoundDoTick(void) {
	const uint16 rv=VT369SoundResetVector();
	int32 out=0;
	if (rv==0x0293) out=VT369SoundTick0293();
	else { VT369SoundProg p; if (VT369SoundProgFor(rv,&p)) out=VT369SoundTick48(&p); }
	VT369SoundSample=(int16)(out<<8);
}
static void VT369SoundHLEWrite(uint32 off, uint8 value) {
	if (!VT369SoundHLEActive) return;
	const uint16 rv=VT369SoundResetVector();
	if (rv==0x0293) { if (off==0x7A2) VT369SoundCommand0293(); return; }
	VT369SoundProg p;
	if (!VT369SoundProgFor(rv,&p)) {
		// Conservative bootstrap fallback for an as-yet unknown sound program.
		if (off==0x04C) VT369SoundRAM[0x04D]=value;
		return;
	}
	if (off==p.masks) {
		const uint8 edge=(uint8)(value & ~VT369SoundStartPrev);
		VT369SoundStartPrev=value;
		for (uint32 ch=0; ch<4; ++ch) if (edge & (1U<<ch))
			VT369Write24(0x30+ch*4, VT369Read24(0x60+ch*4));
		VT369SoundRAM[p.masks+1] |= edge;
	} else if (off==p.masks+2) {
		VT369SoundRAM[p.masks+1] &= (uint8)~value;
	}
}

static void UNLOneBusVT32PCMSetPeriod(uint8 value) {
	/* NRS ignores zero and otherwise uses the byte directly as CPU cycles per
	 * sample.  Keep both copies for state-file compatibility with R2. */
	if (!value) return;
	vt32_pcm_period[0] = vt32_pcm_period[1] = (uint32)value;
}
static uint8 UNLOneBusVT32PCMROMRead(uint32 a) {
	if (!PRGptr[0] || !PRGsize[0]) return 0xFF;
	return PRGptr[0][a % PRGsize[0]];
}
static void UNLOneBusVT32PCMStopChannel(int ch, int clearBit) {
	vt32_pcm_playing[ch] = 0; vt32_pcm_count[ch] = 0; vt32_pcm_sample[ch] = 0;
	vt32_pcm_second_nibble[ch] = 0;
	if (clearBit) apu40xx[0x33] &= (uint8)~(ch ? 0x08 : 0x10);
	OneBusUpdateCpuHook();
}
static void UNLOneBusVT32PCMStartChannel(int ch, uint8 control) {
	/* Both channels capture the same address latch at their respective start. */
	vt32_pcm_address[ch] = vt32_pcm_programmed[0] & ~0x3FU;
	vt32_pcm_count[ch] = 0; vt32_pcm_sample[ch] = 0; vt32_pcm_playing[ch] = 1;
	vt32_pcm_adpcm[ch] = (control & 0x40) ? 1 : 0;
	vt32_pcm_predictor[ch] = vt32_pcm_step_index[ch] = vt32_pcm_step_size[ch] = 0;
	vt32_pcm_second_nibble[ch] = 0;
	OneBusUpdateCpuHook();
}
static void UNLOneBusVT32PCMLinearTick(int ch) {
	uint8 v = UNLOneBusVT32PCMROMRead(vt32_pcm_address[ch]++);
	if (v == 0xFF) { UNLOneBusVT32PCMStopChannel(ch, 1); return; }
	vt32_pcm_sample[ch] = (int16)(((int)v - 0x80) << 4);
}
static void UNLOneBusVT32PCMADPCMTick(int ch) {
	uint8 n; int d, idx;
	if (!vt32_pcm_second_nibble[ch] &&
	    UNLOneBusVT32PCMROMRead(vt32_pcm_address[ch]) == 0x00 &&
	    UNLOneBusVT32PCMROMRead(vt32_pcm_address[ch]+1) == 0x00 &&
	    UNLOneBusVT32PCMROMRead(vt32_pcm_address[ch]+2) == 0xFF) {
		UNLOneBusVT32PCMStopChannel(ch, 1); return;
	}
	n = UNLOneBusVT32PCMROMRead(vt32_pcm_address[ch]);
	if (vt32_pcm_second_nibble[ch]) { n &= 15; vt32_pcm_address[ch]++; } else n >>= 4;
	vt32_pcm_second_nibble[ch] ^= 1;
	idx = vt32_pcm_step_index[ch]; if (idx < 0) idx = 0; if (idx > 58) idx = 58;
	vt32_pcm_step_size[ch] = VT32ADPCMStepTable[idx];
	d = vt32_pcm_step_size[ch] >> 3;
	if (n & 4) d += vt32_pcm_step_size[ch];
	if (n & 2) d += vt32_pcm_step_size[ch] >> 1;
	if (n & 1) d += vt32_pcm_step_size[ch] >> 2;
	if (n & 8) d = -d;
	vt32_pcm_predictor[ch] += d;
	if (vt32_pcm_predictor[ch] > 2047) vt32_pcm_predictor[ch] = 2047;
	if (vt32_pcm_predictor[ch] < -2048) vt32_pcm_predictor[ch] = -2048;
	vt32_pcm_sample[ch] = (int16)vt32_pcm_predictor[ch];
	vt32_pcm_step_index[ch] += VT32ADPCMIndexTable[n & 15];
	if (vt32_pcm_step_index[ch] < 0) vt32_pcm_step_index[ch] = 0;
	if (vt32_pcm_step_index[ch] > 58) vt32_pcm_step_index[ch] = 58;
}
static void UNLOneBusVT32PCMTickChannel(int ch) {
	if (!vt32_pcm_playing[ch]) return;
	if (vt32_pcm_adpcm[ch]) UNLOneBusVT32PCMADPCMTick(ch); else UNLOneBusVT32PCMLinearTick(ch);
	if (vt32_pcm_playing[ch]) vt32_pcm_count[ch] += (int32)vt32_pcm_period[ch];
}
static void UNLOneBusResetVT32PCM(void) {
	int ch; vt32_pcm_selected = 0;
	vt32_alu_operand14 = 0; vt32_alu_operand56 = vt32_alu_operand67 = 0; vt32_alu_busy = 0;
	for (ch=0; ch<2; ch++) {
		vt32_pcm_programmed[ch]=vt32_pcm_address[ch]=0; vt32_pcm_count[ch]=0;
		vt32_pcm_period[ch]=0x6F;
		vt32_pcm_sample[ch]=0; vt32_pcm_volume[ch]=128; vt32_pcm_playing[ch]=0;
		vt32_pcm_adpcm[ch]=0; vt32_pcm_predictor[ch]=vt32_pcm_step_index[ch]=vt32_pcm_step_size[ch]=0;
		vt32_pcm_second_nibble[ch]=0;
	}
}
static int32 UNLOneBusVT32PCMMix(void) {
	/* NRS Channel::output(): (sample * 128) >> 4.  Sum in 64 bits and
	 * saturate once so dual-channel peaks cannot wrap before the mixer. */
	int64 mix = 0;
	if (vt32_pcm_playing[0]) mix += ((int32)vt32_pcm_sample[0] * 128) >> 4;
	if (vt32_pcm_playing[1]) mix += ((int32)vt32_pcm_sample[1] * 128) >> 4;
	if (mix > 32767) mix = 32767;
	if (mix < -32768) mix = -32768;
	return (int32)mix;
}
static void UNLOneBusAudioFlushBackdated(uint32 cyclesBack);
static void UNLOneBusVT32PCMRunCycles(uint32 cycles) {
	while (cycles && (vt32_pcm_playing[0] || vt32_pcm_playing[1])) {
		uint32 next = cycles + 1; int ch;
		for (ch=0; ch<2; ch++) if (vt32_pcm_playing[ch]) {
			uint32 c = vt32_pcm_count[ch] <= 0 ? 0U : (uint32)vt32_pcm_count[ch];
			if (c < next) next = c;
		}
		if (next > cycles) {
			for (ch=0; ch<2; ch++) if (vt32_pcm_playing[ch]) vt32_pcm_count[ch] -= (int32)cycles;
			break;
		}
		for (ch=0; ch<2; ch++) if (vt32_pcm_playing[ch]) vt32_pcm_count[ch] -= (int32)next;
		cycles -= next; UNLOneBusAudioFlushBackdated(cycles);
		for (ch=0; ch<2; ch++) if (vt32_pcm_playing[ch] && vt32_pcm_count[ch] <= 0) UNLOneBusVT32PCMTickChannel(ch);
	}
}

static uint32 UNLOneBusAudioCPUClock(void) {
	return (uint32)((PAL ? PAL_CPU : NTSC_CPU) + 0.5);
}

static uint32 UNLOneBusAudioSampleRate(void) {
	return OneBusDWSSampleRates[apu40xx[0x10] & 0x0F];
}

static uint8 UNLOneBusDWSOutput(uint8 amplitude) {
	/* DWS amplitude is seven bits; expand it monotonically to the eight-bit
	 * XOP DA range without changing either endpoint. */
	return (uint8)((amplitude << 1) | (amplitude >> 6));
}

static uint8 UNLOneBusAudioOutputEnabled(void) {
	const uint8 control = apu40xx[0x30];
	if (control & 0x10) /* DP=1: PCM is routed only to XOP2. */
		return (control & 0x08) != 0;
	/* DP=0: DWS can use XOP1 (DA1=0) and/or XOP2 (DA2=1). FCEUX
	 * currently exposes a mono output, so the two physical pins are folded
	 * into one signal rather than counted twice. */
	return ((control & 0x04) == 0) || ((control & 0x08) != 0);
}

static int32 UNLOneBusAudioAmplitude(int highQuality) {
	uint32 volume;
	uint32 fullScale;
	if (!FSettings.SndRate)
		return 0;
	volume = (FSettings.PCMVolume < 0) ? 0 :
		(FSettings.PCMVolume > 256 ? 256 : (uint32)FSettings.PCMVolume);
	if (!volume)
		return 0;
	fullScale = highQuality ? 9409 : 588;

	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT369) {
		if (!VT369SoundHLEActive) return 0;
		const int32 mix = VT369SoundSample;
		if (highQuality) {
			const int32 linear = 16384 + (mix >> 1);
			return (int32)(((int64)linear * volume) / 256);
		}
		return (int32)(((int64)mix * 588LL * (int64)volume) / (32768LL * 256LL));
	}

	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32) {
		const int32 mix = UNLOneBusVT32PCMMix();
		if (highQuality) {
			/* WaveHi is NOT a signed linear PCM buffer: bits 16..31 carry
			 * the packed RP2A03 nonlinear mixer inputs.  R2 wrote negative VT32
			 * samples directly here; the uint32 conversion in sound.cpp then
			 * interpreted the sign bits as maximum APU levels, causing the
			 * measured full-scale blast.  Follow FCEUX's linear expansion-audio
			 * convention (mapper 594): halve the signed two-channel range and
			 * bias it into the low 16-bit field, yielding 0..32767 with no carry
			 * into the RP2A03 fields.  NeoFilterSound removes the constant DC. */
			const int32 linear = 16384 + (mix >> 1);
			return (int32)(((int64)linear * volume) / 256);
		}
		/* Low-quality Wave[] is already a signed linear accumulator. */
		return (int32)(((int64)mix * 588LL * (int64)volume) /
			(32768LL * 256LL));
	}

	if (!UNLOneBusAudioOutputEnabled())
		return 0;
	/* VT03 XOP2 is an unsigned eight-bit DA output. */
	return (int32)(((uint64)pcm_output * fullScale * volume + (255 * 128)) /
		(255 * 256));
}

static void UNLOneBusAudioFlush(void) {
	uint32 end;
	uint32 v;
	int32 amplitude;
	if (!FSettings.SndRate)
		return;
	if (FSettings.soundq >= 1) {
		end = SOUNDTS;
		if (end > 40000) end = 40000;
		if (OneBusAudioHiPosition > end)
			OneBusAudioHiPosition = end;
		amplitude = UNLOneBusAudioAmplitude(1);
		for (v = OneBusAudioHiPosition; v < end; v++)
			WaveHi[v] += amplitude;
		OneBusAudioHiPosition = end;
	} else {
		if (!soundtsinc)
			return;
		end = (uint32)(((uint64)SOUNDTS << 16) / soundtsinc);
		if (end > (2048 + 512) * 16)
			end = (2048 + 512) * 16;
		if (OneBusAudioLQPosition > end)
			OneBusAudioLQPosition = end;
		amplitude = UNLOneBusAudioAmplitude(0);
		for (v = OneBusAudioLQPosition; v < end; v++)
			Wave[v >> 4] += amplitude;
		OneBusAudioLQPosition = end;
	}
}

static void UNLOneBusAudioFlushBackdated(uint32 cyclesBack) {
	uint32 fudge = cyclesBack;
	if (fudge) {
		const uint32 available = soundtsoffs + soundtimestamp;
		if (fudge > available)
			fudge = available;
		soundtsoffs -= fudge;
	}
	UNLOneBusAudioFlush();
	if (fudge)
		soundtsoffs += fudge;
}

static void UNLOneBusAudioFill(int Count) {
	(void)Count;
	UNLOneBusAudioFlush();
	/* The low-quality Wave[] buffer is rebased after each frame, while its
	 * fractional sample phase is carried by Count (= end & 0x0F). */
	OneBusAudioLQPosition = (uint32)Count & 0x0F;
}

static void UNLOneBusAudioHiFill(void) {
	UNLOneBusAudioFlush();
}

static void UNLOneBusAudioHiSync(int32 ts) {
	OneBusAudioHiPosition = (ts < 0) ? 0 : (uint32)ts;
}

static void UNLOneBusAudioRateChanged(void) {
	OneBusAudioHiPosition = soundtsoffs;
	OneBusAudioLQPosition = soundtsinc ?
		(uint32)(((uint64)soundtsoffs << 16) / soundtsinc) & 0x0F : 0;
}

static void UNLOneBusAudioKill(void) {
	OneBusAudioHiPosition = 0;
	OneBusAudioLQPosition = 0;
}

static void UNLOneBusInstallAudio(void) {
	GameExpSound.Fill = UNLOneBusAudioFill;
	GameExpSound.NeoFill = NULL;
	GameExpSound.HiFill = UNLOneBusAudioHiFill;
	GameExpSound.HiSync = UNLOneBusAudioHiSync;
	GameExpSound.RChange = UNLOneBusAudioRateChanged;
	GameExpSound.Kill = UNLOneBusAudioKill;
	UNLOneBusAudioRateChanged();
}

static void UNLOneBusPCMIRQClear(void) {
	pcm_irq = 0;
	X6502_IRQEnd(ONEBUS_PCM_IRQ);
}

static void UNLOneBusPCMIRQAssert(void) {
	if (apu40xx[0x10] & 0x80) {
		pcm_irq = 0x80;
		X6502_IRQBegin(ONEBUS_PCM_IRQ);
	}
}

static void UNLOneBusPCMUpdateRate(void) {
	const uint32 rate = UNLOneBusAudioSampleRate();
	const uint32 cpuClock = UNLOneBusAudioCPUClock();
	pcm_clock = (int16)((cpuClock + (rate >> 1)) / rate);
	pcm_latch = 0;
}

static void UNLOneBusPCMRestart(void) {
	pcm_addr = pcm_start_addr;
	pcm_size = pcm_start_size;
	pcm_end_pending = 0;
	pcm_dws_shift = 0;
	pcm_dws_bits_left = 0;
	pcm_dws_amplitude = pcm_dws_initial;
	if (!(apu40xx[0x30] & 0x10))
		pcm_output = UNLOneBusDWSOutput(pcm_dws_amplitude);
}

static void UNLOneBusPCMStart(void) {
	UNLOneBusAudioFlush();
	UNLOneBusPCMIRQClear();
	/* Keep the 15-bit counter separate from $4030's live A15/A14
	 * complement pins, matching the original OneBus bus-read path. */
	pcm_start_addr = (uint16)((uint16)apu40xx[0x12] << 6);
	pcm_start_size = (uint16)(((uint16)apu40xx[0x13] << 4) + 1);
	pcm_phase = 0;
	UNLOneBusPCMUpdateRate();
	UNLOneBusPCMRestart();
	pcm_enable = 1;
	OneBusUpdateCpuHook();
}

static void UNLOneBusPCMStop(void) {
	UNLOneBusAudioFlush();
	pcm_enable = 0;
	pcm_phase = 0;
	pcm_end_pending = 0;
	pcm_dws_bits_left = 0;
	UNLOneBusPCMIRQClear();
	OneBusUpdateCpuHook();
}

static void UNLOneBusPCMComplete(void) {
	UNLOneBusPCMIRQAssert();
	if (apu40xx[0x10] & 0x40) {
		/* DREP repeats both DWS and PCM DMA. The caller continues the tick
		 * after restart, so the next pass begins without an extra silent clock. */
		UNLOneBusPCMRestart();
	} else {
		pcm_enable = 0;
		pcm_phase = 0;
		pcm_end_pending = 0;
	}
	OneBusUpdateCpuHook();
}

static uint16 UNLOneBusPCMReadAddress(void) {
	/* $4030 drives the high address pins live; the DMA counter supplies
	 * A14:A0 and wraps at 0x7FFF, as in the original OneBus reference path. */
	return (uint16)(pcm_addr | (((apu40xx[0x30] ^ 0x03) & 0x03) << 14));
}

static void UNLOneBusPCMAddressIncrement(void) {
	pcm_addr = (uint16)((pcm_addr + 1) & 0x7FFF);
}

static void UNLOneBusPCMTick(void) {
	if (pcm_end_pending) {
		UNLOneBusPCMComplete();
		if (!pcm_enable || !(apu40xx[0x10] & 0x40))
			return;
	}
	if (apu40xx[0x30] & 0x10) {
		/* PCM: one unsigned 8-bit sample per selected SD clock. */
		if (!pcm_size) {
			pcm_end_pending = 1;
			return;
		}
		{
			const uint16 addr = UNLOneBusPCMReadAddress();
			pcm_output = ARead[addr](addr);
		}
		UNLOneBusPCMAddressIncrement();
		pcm_size--;
		if (!pcm_size)
			pcm_end_pending = 1;
	} else {
		/* DWS: serial one-bit slope data, least-significant bit first. The
		 * VT03 datasheet specifies the 7-bit initial amplitude and SD clock;
		 * the documented slope-decoder interface is modelled as a saturating
		 * one-step delta integrator (1=up, 0=down). Keeping the decoder isolated
		 * makes its polarity/step easy to refine if a reference trace differs. */
		if (!pcm_dws_bits_left) {
			if (!pcm_size) {
				pcm_end_pending = 1;
				return;
			}
			const uint16 addr = UNLOneBusPCMReadAddress();
			pcm_dws_shift = ARead[addr](addr);
			UNLOneBusPCMAddressIncrement();
			pcm_size--;
			pcm_dws_bits_left = 8;
		}
		if (pcm_dws_shift & 1) {
			if (pcm_dws_amplitude < 0x7F) pcm_dws_amplitude++;
		} else {
			if (pcm_dws_amplitude) pcm_dws_amplitude--;
		}
		pcm_dws_shift >>= 1;
		pcm_dws_bits_left--;
		pcm_output = UNLOneBusDWSOutput(pcm_dws_amplitude);
		if (!pcm_size && !pcm_dws_bits_left)
			pcm_end_pending = 1;
	}
}

/* Keep the OneBus audio engine in the same reset state as its APU registers. */
static void UNLOneBusResetPCM(void) {
	pcm_enable = 0;
	pcm_irq = 0;
	pcm_addr = 0;
	pcm_size = 0;
	pcm_latch = 0;
	pcm_clock = 0;
	pcm_start_addr = 0;
	pcm_start_size = 0;
	pcm_phase = 0;
	pcm_end_pending = 0;
	pcm_dws_shift = 0;
	pcm_dws_bits_left = 0;
	pcm_dws_initial = 0;
	pcm_dws_amplitude = 0;
	pcm_output = 0;
	UNLOneBusResetVT32PCM();
	UNLOneBusAudioRateChanged();
	OneBusUpdateCpuHook();
}

static writefunc defapuwrite[64];
static readfunc defapuread[64];
static writefunc VT369OldWrite2008 = NULL;
static readfunc VT369OldRead2008 = NULL;

static SFORMAT StateRegs[] =
{
	{ cpu410x, 64, "REGC" },
	{ ppu201x, 16, "REGS" },
	{ OneBusCGRAM, 0x100, "VTCG" },
	{ OneBusCGRAM + 0x100, 0x100, "V3CG" },
	{ apu40xx, 64, "REGA" },
	{ &IRQReload, 1, "IRQR" },
	{ &IRQCount, 1, "IRQC" },
	{ &IRQa, 1, "IRQA" },
	{ &IRQPending, 1, "IRQP" },
	{ &pcm_enable, 1, "PCME" },
	{ &pcm_irq, 1, "PCMI" },
	{ &pcm_addr, 2 | FCEUSTATE_RLSB, "PCMA" },
	{ &pcm_size, 2 | FCEUSTATE_RLSB, "PCMS" },
	{ &pcm_latch, 2 | FCEUSTATE_RLSB, "PCML" },
	{ &pcm_clock, 2 | FCEUSTATE_RLSB, "PCMC" },
	{ &pcm_start_addr, 2 | FCEUSTATE_RLSB, "PCSA" },
	{ &pcm_start_size, 2 | FCEUSTATE_RLSB, "PCSL" },
	{ &pcm_phase, 8 | FCEUSTATE_RLSB, "PCPH" },
	{ &pcm_end_pending, 1, "PCEN" },
	{ &pcm_dws_shift, 1, "DWSS" },
	{ &pcm_dws_bits_left, 1, "DWSB" },
	{ &pcm_dws_initial, 1, "DWSI" },
	{ &pcm_dws_amplitude, 1, "DWSA" },
	{ &pcm_output, 1, "PCMO" },
	{ vt32_pcm_programmed + 0, 4 | FCEUSTATE_RLSB, "V2P0" },
	{ vt32_pcm_programmed + 1, 4 | FCEUSTATE_RLSB, "V2P1" },
	{ vt32_pcm_address + 0, 4 | FCEUSTATE_RLSB, "V2A0" },
	{ vt32_pcm_address + 1, 4 | FCEUSTATE_RLSB, "V2A1" },
	{ vt32_pcm_count + 0, 4 | FCEUSTATE_RLSB, "V2C0" },
	{ vt32_pcm_count + 1, 4 | FCEUSTATE_RLSB, "V2C1" },
	{ vt32_pcm_period + 0, 4 | FCEUSTATE_RLSB, "V2R0" },
	{ vt32_pcm_period + 1, 4 | FCEUSTATE_RLSB, "V2R1" },
	{ vt32_pcm_sample + 0, 2 | FCEUSTATE_RLSB, "V2S0" },
	{ vt32_pcm_sample + 1, 2 | FCEUSTATE_RLSB, "V2S1" },
	{ vt32_pcm_volume, 2, "V2VL" },
	{ vt32_pcm_playing, 2, "V2PL" },
	{ vt32_pcm_adpcm, 2, "V2AD" },
	{ vt32_pcm_predictor + 0, 4 | FCEUSTATE_RLSB, "V2D0" },
	{ vt32_pcm_predictor + 1, 4 | FCEUSTATE_RLSB, "V2D1" },
	{ vt32_pcm_step_index + 0, 4 | FCEUSTATE_RLSB, "V2I0" },
	{ vt32_pcm_step_index + 1, 4 | FCEUSTATE_RLSB, "V2I1" },
	{ vt32_pcm_step_size + 0, 4 | FCEUSTATE_RLSB, "V2Z0" },
	{ vt32_pcm_step_size + 1, 4 | FCEUSTATE_RLSB, "V2Z1" },
	{ vt32_pcm_second_nibble, 2, "V2NB" },
	{ &vt32_pcm_selected, 1, "V2SL" },
	{ &vt32_alu_operand14, 4 | FCEUSTATE_RLSB, "V2M4" },
	{ &vt32_alu_operand56, 2 | FCEUSTATE_RLSB, "V2M6" },
	{ &vt32_alu_operand67, 2 | FCEUSTATE_RLSB, "V2M7" },
	{ &vt32_alu_busy, 1, "V2MB" },
	{ &reg4242, 1, "4242" },
	{ &dipswitch, 1, "DIPS" },
	{ VT369ExtraRAM, 0x800, "V3ER" },
	{ VT369SoundRAM, 0x800, "V3SR" },
	{ VT369Relative, 2, "V3RB" },
	{ VT369PPURegs, 0x60, "V3PX" },
	{ VT369PaletteRAM, 0x400, "V3PL" },
	{ VT369OAM, 0x200, "V3OA" },
	{ &VT369OAMHigh, 1, "V3OH" },
	{ &VT369Protection4169, 1, "V369" },
	{ &VT369SoundControl4162, 1, "V3SC" },
	{ &VT369SoundHLEActive, 1, "V3SA" },
	{ &VT369SoundHLECycles, 4 | FCEUSTATE_RLSB, "V3CY" },
	{ &VT369SoundPhase6, 4 | FCEUSTATE_RLSB, "V3P6" },
	{ &VT369SoundIRQAcc, 4 | FCEUSTATE_RLSB, "V3IA" },
	{ &VT369SoundPeriodN, 2 | FCEUSTATE_RLSB, "V3PN" },
	{ &VT369SoundSample, 2 | FCEUSTATE_RLSB, "V3SO" },
	{ &VT369SoundStartPrev, 1, "V3ST" },
	{ VT369Sound3Left, 3, "V3L3" },
	{ VT369Sound3Frame, 24 | FCEUSTATE_RLSB, "V3F3" },
	{ &VT369PeripheralCycleRemainder, 1, "V3CR" },
	{ VT369GPIO414x, 0x10, "V414" },
	{ VT369GPIO415x, 0x10, "V415" },
	{ VT369GPIO41Ex, 0x10, "V41E" },
	{ 0 }
};

static uint8 *WRAM;
static uint8 *securityROM_data = NULL;

/* VT369 $4160/$4161 form a 12-bit relative ROM base in 8 KiB units.
 * The base is additive and applies to both CPU PRG and PPU CHR ROM reads. */
static uint32 VT369Relative8K(void) {
	if (!OneBusIsVT369) return 0;
	return (uint32)VT369Relative[0] | (((uint32)VT369Relative[1] & 0x0FU) << 8);
}

static uint32 OneBusPRGBankFromRegister(uint8 bank) {
	uint8 bankmode = cpu410x[0xb] & 7;
	uint8 mask = (bankmode == 0x7) ? 0xff : (0x3f >> bankmode);
	uint32 block = ((cpu410x[0x0] & 0xf0) << 4) + (cpu410x[0xa] & (~mask));
	return block | (bank & mask);
}

static void PSync(int AND, int OR) {
	uint8 bankmode = cpu410x[0xb] & 7;
	uint8 mask = (bankmode == 0x7) ? (0xff) : (0x3f >> bankmode);
	uint32 block = ((cpu410x[0x0] & 0xf0) << 4) + (cpu410x[0xa] & (~mask));
	uint32 pswap = (mmc3cmd & 0x40) << 8;

	uint8 bank0 = cpu410x[0x7];
	uint8 bank1 = cpu410x[0x8];
	uint8 bank2 = (cpu410x[0xb] & 0x40) ? (cpu410x[0x9]) : (~1);
	uint8 bank3 = ~0;
	
	const uint32 rel = VT369Relative8K();
	setprg8(0x8000 ^ pswap,(((block | (bank0 & mask)) &AND) | OR) + rel);
	setprg8(0xa000,        (((block | (bank1 & mask)) &AND) | OR) + rel);
	setprg8(0xc000 ^ pswap,(((block | (bank2 & mask)) &AND) | OR) + rel);
	setprg8(0xe000,        (((block | (bank3 & mask)) &AND) | OR) + rel);
}

static void CSync(int AND, int OR) {
	static const uint8 midx[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
	OneBusCHRAnd = (uint32)AND;
	OneBusCHROr = (uint32)OR;
	OneBusUseMappedCHR = 0;

	/* $4105 bit 5 (IVRCH) maps the SoC's internal 2 KiB NTRAM onto the
	 * pattern bus as writable CHR-RAM. Mirror the 2 KiB RAM four times. */
	if (mmc3cmd & 0x20) {
		int page;
		OneBusUseMappedCHR = 1;
		for (page = 0; page < 8; page++)
			setchr1r(ONEBUS_IVR_CHIP, page << 10, page & 1);
		setmirror((mirror ^ 1) & 1);
		return;
	}

	uint8 mask = 0xff >> midx[ppu201x[0xa] & 7];
	uint32 block = ((cpu410x[0x0] & 0x0f) << 11) + ((ppu201x[0x8] & 0x70) << 4) + (ppu201x[0xa] & (~mask));
	uint32 cswap = (mmc3cmd & 0x80) << 5;

	uint8 bank0 = ppu201x[0x6] & (~1);
	uint8 bank1 = ppu201x[0x6] | 1;
	uint8 bank2 = ppu201x[0x7] & (~1);
	uint8 bank3 = ppu201x[0x7] | 1;
	uint8 bank4 = ppu201x[0x2];
	uint8 bank5 = ppu201x[0x3];
	uint8 bank6 = ppu201x[0x4];
	uint8 bank7 = ppu201x[0x5];

	const uint32 rel1K = VT369Relative8K() << 3;
	setchr1(0x0000 ^ cswap,(((block | (bank0 & mask)) &AND) | OR) + rel1K);
	setchr1(0x0400 ^ cswap,(((block | (bank1 & mask)) &AND) | OR) + rel1K);
	setchr1(0x0800 ^ cswap,(((block | (bank2 & mask)) &AND) | OR) + rel1K);
	setchr1(0x0c00 ^ cswap,(((block | (bank3 & mask)) &AND) | OR) + rel1K);
	setchr1(0x1000 ^ cswap,(((block | (bank4 & mask)) &AND) | OR) + rel1K);
	setchr1(0x1400 ^ cswap,(((block | (bank5 & mask)) &AND) | OR) + rel1K);
	setchr1(0x1800 ^ cswap,(((block | (bank6 & mask)) &AND) | OR) + rel1K);
	setchr1(0x1c00 ^ cswap,(((block | (bank7 & mask)) &AND) | OR) + rel1K);

	setmirror((mirror ^ 1) & 1);
}

/* Translate a CPU PPUDATA pattern-table address through the live VT bank
 * registers. If either BKEXTEN or SPEXTEN is enabled, CPU accesses use
 * $2018[2:0] as EVA[2:0] (VRWB). */
/* VT03 renderer helpers -------------------------------------------------- */
static uint8 FASTCALL UNLOneBusGetExtMode(void) {
	uint8 mode = ppu201x[0x0];
	/* VT02 has address extension, but no 4bpp, PIX16, COLCOMP, SPOPEN or
	 * 16-bit video bus.  Mask unsupported bits before the generic renderer
	 * sees them; games sometimes leave those register bits non-zero. */
	if (!OneBusHas4BPP) mode &= 0x18;
	else if (!OneBusHas16BitVideoBus) mode &= 0x9F;
	if (!OneBusHasEnhancedPalette) mode &= 0x7F;
	return mode;
}

static uint8 FASTCALL UNLOneBusGetPackedMode(void);

static uint8 FASTCALL UNLOneBusUseFixedOSD(void) {
	/* Return a mode, not merely a boolean:
	 *   0 = ordinary FCEUX palette;
	 *   1 = VT03/VT09/normal VT32 COLCOMP, where $80-$FF are game-owned
	 *       and the fixed low mirror slots remain safe;
	 *   2 = VT32 $2010=$80 packed mode, where all 256 indices are game-owned.
	 *       drawing.cpp then allocates OSD colors only from indices unused by
	 *       the current frame, preserving both game and HUD colors.
	 *   3 = VT369 enhanced 512-colour BG/OBJ mode. drawing.cpp reserves unused
	 *       9-bit direct-palette entries and also writes the XDBuf high bits. */
	if (OneBusIsVT369 && ppu201x[0x0E]) return 3;
	if (UNLOneBusGetPackedMode()) return 2;
	return (OneBusHasEnhancedPalette && (UNLOneBusGetExtMode() & 0x80)) ? 1 : 0;
}

static uint8 FASTCALL UNLOneBusMapExtPalette(uint8 pen) {
	pen &= 0x7F;
	if (!OneBusHasEnhancedPalette)
		return FFCEUX_PPURead_Default(0x3F00 | (pen & 0x1F)) & 0x3F;
	return OneBusCGRAM[pen] & 0x3F;
}

/* VT369 Stage 2 ---------------------------------------------------------
 * $201E!=0 switches the VT3xx PPU to its packed enhanced renderer.  Keep the
 * high-resolution bit visible to the core but do not render that mode yet.
 * flags: bit0 enhanced, bit1 packed 8bpp BG, bit2 split-table OAM, bit3 hires,
 * bit4 special 64-byte name-table / 16-bit tile-number mode. */
static uint8 FASTCALL UNLOneBusGetVT369Mode(void) {
	if (!OneBusIsVT369 || ppu201x[0x0E] == 0) return 0;
	uint8 flags = 0x01;
	if ((ppu201x[0x0C] & 0x03) == 0x02) flags |= 0x02;
	if (ppu201x[0x0E] & 0x04) flags |= 0x04;
	if (ppu201x[0x0C] & 0x04) flags |= 0x08;
	/* NRS/MAME VT369 commercial-game mode: $201C/$201D/$201E=12/09/0F,
	 * background tile-base bit 4 set, sprite base low byte exactly $08. */
	if (ppu201x[0x0C] == 0x12 && ppu201x[0x0D] == 0x09 && ppu201x[0x0E] == 0x0F &&
		(VT369PPURegs[0] & 0x10) && VT369PPURegs[2] == 0x08)
		flags |= 0x10;
	return flags;
}
static uint16 FASTCALL UNLOneBusGetVT369SpriteConfig(void) {
	return OneBusIsVT369 ? (uint16)ppu201x[0x0D] | ((uint16)ppu201x[0x0C] << 8) : 0;
}

static uint8 FASTCALL UNLOneBusReadVT369Reg(uint8 reg) {
	return OneBusIsVT369 ? ppu201x[reg & 0x0F] : 0;
}

static uint16 VT369TileBaseFull(int sprite) {
	const int off = sprite ? 2 : 0;
	return (uint16)VT369PPURegs[off] | ((uint16)VT369PPURegs[off + 1] << 8);
}

static uint16 VT369TileBase(int sprite) {
	return VT369TileBaseFull(sprite) & 0x0FFF;
}

static void OneBusUpdateCHRFastWrap(void) {
	const uint32 size = CHRsize[0];
	const uint32 split = size >> 1;
	OneBusCHRWrapPow2 = (size && !(size & (size - 1))) ? 1 : 0;
	OneBusCHRWrapMask = OneBusCHRWrapPow2 ? (size - 1) : 0;
	OneBusCHRSplitWrapPow2 = (split && !(split & (split - 1))) ? 1 : 0;
	OneBusCHRSplitWrapMask = OneBusCHRSplitWrapPow2 ? (split - 1) : 0;
}

static inline uint32 OneBusWrapCHRAddress(uint32 address) {
	return OneBusCHRWrapPow2 ? (address & OneBusCHRWrapMask)
	                        : (address % CHRsize[0]);
}

static inline uint32 OneBusWrapCHRSplitAddress(uint32 address, uint32 splitSize) {
	return OneBusCHRSplitWrapPow2 ? (address & OneBusCHRSplitWrapMask)
	                             : (address % splitSize);
}

/* VT369 packed graphics read the unified cartridge address space, with the
 * $4160/$4161 relative base added in 8 KiB units.  OneBus maps PRG as CHR when
 * a cart has no dedicated CHR ROM, so CHRptr[0] is the appropriate raw image. */
static uint8 VT369ReadOneSpaceRelative(uint32 offset) {
	if (!CHRptr[0] || !CHRsize[0]) return 0;
	const uint32 physical = OneBusWrapCHRAddress((VT369Relative8K() << 13) + offset);
	return Mapper296CHRDecrypt(CHRptr[0][physical]);
}

/* Convert the ordinary NES-style pattern address generated by FCEUX into
 * VT369 enhanced packed 4/8bpp offsets.  The name-table byte supplies tile
 * bits 0..7 and, unless $201C.3 is set, the cell attribute supplies bits 8..9. */
static uint32 VT369LegacyPackedBase(uint32 address, int sprite, uint16 tile) {
	/* NRS/Furb VT369:
	 *   BG  live-bank granularity uses $201C.3;
	 *   OBJ live-bank granularity uses $201C.7.
	 * $201C[1:0] and [5:4] then add the BG/OBJ packed-format shift. */
	const uint8 r1c = ppu201x[0x0C];
	uint8 bank;
	if (sprite) {
		const int right = (address & 0x1000U) ? 1 : 0;
		bank = right ? (ppu201x[0x6] & ~1) : ppu201x[0x2];
		(void)tile;
	}
	else {
		bank = (address & 0x1000U) ? (ppu201x[0x6] & ~1) : ppu201x[0x2];
	}
	const uint8 shift0 = sprite ? ((r1c & 0x80) ? 10 : 13)
	                           : ((r1c & 0x08) ? 10 : 13);
	const uint8 shift1 = sprite ? ((r1c >> 4) & 3) : (r1c & 3);
	return (uint32)bank << (shift0 + shift1);
}

static uint32 VT369BGBase(uint32 address) {
	(void)address;
	/* Stage 2.4.11: match the VT3xx reference renderer.  Once $201E enables
	 * the packed VT3xx renderer, $2020/$2021 is the packed BG base directly;
	 * it is a full 16-bit value in 8 KiB units.  Do not reinterpret $201E.0
	 * as a live-bank selector. */
	return (uint32)VT369TileBaseFull(0) << 13;
}

static uint32 VT369SpriteBase(uint16 tile, uint8 rightTable) {
	(void)tile;
	(void)rightTable;
	/* Standard-resolution packed OBJ likewise always uses $2022/$2023 as the
	 * full 16-bit 8 KiB base.  The ordinary NES pattern-table selector does
	 * not participate in VT3xx packed OBJ addressing. */
	return (uint32)VT369TileBaseFull(1) << 13;
}

static uint16 VT369FixSpriteTile(uint16 tile) {
	if (ppu201x[0x0C] & 0x80) tile &= 0x00FF;
	/* NRS/Furb "Cross River" quirk: with this exact enhanced configuration
	 * tile bits 8..10 are physically shifted down to 6..8. */
	if (ppu201x[0x0E] == 0x0F && ppu201x[0x0D] == 0x0B &&
		(UNLOneBusGetExtMode() & 0x10))
		tile = (uint16)((tile & 0x003F) | ((tile >> 2) & ~0x003FU));
	return tile;
}

static uint8 FASTCALL UNLOneBusPPUReadVT369BG(uint32 address, uint8 color, uint8 pixel) {
	const uint8 mode = UNLOneBusGetVT369Mode();
	if (!(mode & 0x01)) return 0;
	pixel &= 7;

	/* VT369 high-resolution compatibility path.  Current NRS/MAME rendering
	 * presents these 16x16 resources through an 8x8 compatibility fetch: the
	 * tile consumes 0x100 bytes, rows are 0x20 bytes apart and the 8 pixels
	 * select the even sub-pixels through the documented VA34 permutation. */
	if (mode & 0x08) {
		const uint16 tile = (uint16)((address & 0x0FF0U) >> 4);
		const uint8 row = (uint8)(address & 7);
		const uint8 plane = (address & 8) ? 8 : 0;
		const uint8 va34 = (uint8)((pixel & 3) << 1);
		const uint8 pixplane = (pixel & 4) ? 8 : 0;
		const uint32 base = (uint32)VT369TileBaseFull(0) << 13;
		const uint32 off = base + (uint32)tile * 0x100U + (uint32)row * 0x20U +
			va34 + plane + pixplane + (uint32)(color & 3) * 0x10000U;
		return VT369ReadOneSpaceRelative(off);
	}

	/* FCEUX supplies the ordinary NES pattern address: tile byte in A4..A11
	 * and fine Y in A0..A2.  In VT3xx enhanced mode the cell attribute is
	 * latched as BG4/BG3 and always selects the packed resource block. */
	uint16 tile = (uint16)((address >> 4) & 0x00FF);
	/* VT3xx packed BG always uses the latched attribute quadrant as the two
	 * resource-select bits.  In 8bpp this is exactly color * $4000; folding
	 * it into tile bits 8..9 is algebraically equivalent. */
	tile |= (uint16)(color & 3) << 8;
	const uint8 row = (uint8)(address & 7);
	const uint32 base = VT369BGBase(address);

	if (mode & 0x02) {
		/* Packed 8bpp: 64 bytes/tile, one byte/pixel. */
		const uint32 off = base + (uint32)tile * 64U + (uint32)row * 8U + pixel;
		return VT369ReadOneSpaceRelative(off);
	}

	/* Packed 4bpp: 32 bytes/tile, four sequential bytes per scanline,
	 * low nibble = left pixel (GBA-style packed tile order). */
	const uint32 off = base + (uint32)tile * 32U + (uint32)row * 4U + (pixel >> 1);
	const uint8 packed = VT369ReadOneSpaceRelative(off);
	return (pixel & 1) ? (packed >> 4) : (packed & 0x0F);
}

/* Special VT369 name-table mode supplies a full 16-bit tile number. It is
 * always the packed 8bpp path: 64 bytes/tile, one byte per pixel. */
static uint8 FASTCALL UNLOneBusPPUReadVT369BG16(uint16 tile, uint8 row, uint8 pixel) {
	if (!(UNLOneBusGetVT369Mode() & 0x10)) return 0;
	/* The name entry is 16 bits wide, but the current NRS/MAME packed-address
	 * model uses a 12-bit wide tile index in this mode (address & $FFF0).
	 * Unlike the Stage2 compatibility helper, the new-mode tile base itself is
	 * the full $2020/$2021 16-bit value. */
	const uint16 tileBase = (uint16)VT369PPURegs[0] | ((uint16)VT369PPURegs[1] << 8);
	const uint32 base = (uint32)tileBase << 13;
	const uint32 off = base + (uint32)(tile & 0x0FFF) * 64U + ((uint32)row & 7U) * 8U + ((uint32)pixel & 7U);
	return VT369ReadOneSpaceRelative(off);
}

static uint8 FASTCALL UNLOneBusPPUReadVT369Sprite(uint16 tile, uint8 row, uint8 pixel, uint8 bpp, uint8 alt16, uint8 rightTable) {
	if (!(UNLOneBusGetVT369Mode() & 0x01)) return 0;
	tile = VT369FixSpriteTile(tile);

	/* Standard-resolution VT3xx packed OBJ addressing:
	 *   8bpp       : tile*64  + line*8 + pixel
	 *   4bpp normal: tile*128 + line*8 + pixel/2
	 *   4bpp alt16 : tile*32  + line*4 + pixel/2 */
	const uint32 base = VT369SpriteBase(tile, rightTable);
	uint32 off;
	if (bpp == 8) {
		off = base + (uint32)tile * 64U + (uint32)row * 8U + pixel;
		return VT369ReadOneSpaceRelative(off);
	}
	const uint32 tileStride = alt16 ? 32U : 128U;
	const uint32 rowStride = alt16 ? 4U : 8U;
	off = base + (uint32)tile * tileStride + (uint32)row * rowStride + (pixel >> 1);
	const uint8 packed = VT369ReadOneSpaceRelative(off);
	return (pixel & 1) ? (packed >> 4) : (packed & 0x0F);
}
/* High-resolution VT369 sprite resources use a different OAM geometry and
 * different packed strides.  Keep this read helper separate from the standard
 * 128-sprite path so Cube Tech high-res menus cannot perturb ordinary VT369. */
static uint8 FASTCALL UNLOneBusPPUReadVT369HiResSprite(uint16 tile, uint8 width, uint8 height, uint8 row, uint8 pixel, uint8 bpp) {
	if (!(UNLOneBusGetVT369Mode() & 0x08) || !width || !height) return 0;
	const uint32 base = (uint32)VT369TileBaseFull(1) << 13;
	uint32 off;
	if (bpp == 8) {
		off = base + (uint32)tile * (4U * width * height) + (uint32)row * (4U * width) + (uint32)pixel * 2U;
		return VT369ReadOneSpaceRelative(off);
	}
	off = base + (uint32)tile * (2U * width * height) + (uint32)row * (2U * width) + pixel;
	return VT369ReadOneSpaceRelative(off) & 0x0F;
}

static uint8 FASTCALL UNLOneBusPPUReadVT369OAM(uint16 index) {
	return VT369OAM[index & 0x1FF];
}
static uint8 FASTCALL UNLOneBusPPUGetVT369OAMHigh(void) { return VT369OAMHigh & 1; }
static void FASTCALL UNLOneBusPPUWriteVT369OAM(uint8 low, uint8 value) {
	/* VT3xx enhanced mode exposes the full 512-byte sprite RAM.  $2008.0
	 * selects the current high page and a low-byte $FF->$00 wrap toggles it.
	 * This is independent of $201D.0; forcing the high page to zero here
	 * corrupts the second 256-byte half of packed/expanded OAM. */
	VT369OAM[(uint16)low | ((uint16)(VT369OAMHigh & 1) << 8)] = value;
	if (ppu201x[0x0E] && low == 0xFF)
		VT369OAMHigh ^= 1;
}
static DECLFR(VT369Read2008) { (void)A; return VT369OAMHigh & 1; }
static DECLFW(VT369Write2008) {
	(void)A;
	FCEUPPU_LineUpdate();
	VT369OAMHigh = V & 1;
}

static uint8 OneBusRenderEVA(uint8 attr, uint8 isSprite) {
	if (isSprite) {
		if (UNLOneBusGetExtMode() & 0x08) /* SPEXTEN */
			return (uint8)((attr >> 2) & 0x07);
	}
	else if (UNLOneBusGetExtMode() & 0x10) { /* BKEXTEN */
		/* EVA12S ($2011.0): 0 = BKPAGE ($2018.3), 1 = HV ($4106.0). */
		const uint8 eva2 = (ppu201x[0x1] & 0x01)
			? (cpu410x[0x6] & 0x01)
			: ((ppu201x[0x8] >> 3) & 0x01);
		return (uint8)((attr & 0x03) | (eva2 << 2));
	}
	return 0;
}

static uint8 OneBusPageInner(uint8 page) {
	switch (page & 7) {
	case 0: return ppu201x[0x6] & ~1;
	case 1: return ppu201x[0x6] |  1;
	case 2: return ppu201x[0x7] & ~1;
	case 3: return ppu201x[0x7] |  1;
	case 4: return ppu201x[0x2];
	case 5: return ppu201x[0x3];
	case 6: return ppu201x[0x4];
	default:return ppu201x[0x5];
	}
}

/* Resolve one renderer plane to the physical OneBus CHR/PRG byte.
 *
 * 4bpp has two physical layouts:
 *   V16BEN=0 (VT03): plane-sequential 00..07, 08..0F, 10..17, 18..1F.
 *   V16BEN=1 (VT09/VT16+): 16-bit fetch pairs.  Little-endian byte pairs
 *   contain planes 0/2, while the second 16-byte half contains planes 1/3:
 *       plane0=row*2+0, plane2=row*2+1,
 *       plane1=$10+row*2+0, plane3=$10+row*2+1.
 * This is the documented V16BEN layout (SNES-like with planes 1/2 swapped),
 * not the old Stage4.2 placeholder that merely doubled every byte address. */
static uint32 OneBusPack4BPPAddress(uint32 base, uint8 plane, uint8 v16) {
	plane &= 3;
	if (v16) {
		/* Base is the ordinary NES plane-0 row address in the pre-4bpp
		 * address space. Move row bits 0..2 to 1..3, insert plane D1
		 * at physical A0 and plane D0 at physical A4, and widen all
		 * tile/bank bits above the 16-byte NES tile boundary. */
		return ((base & 0x00000007U) << 1)
		     | ((uint32)(plane & 2) >> 1)
		     | ((uint32)(plane & 1) << 4)
		     | ((base & ~0x0000000FU) << 1);
	}

	/* VT03 8-bit bus: row stays in A0..A2, the two plane bits occupy
	 * A3..A4, and tile/bank bits are widened by one address bit. */
	return (base & 0x00000007U)
	     | ((uint32)plane << 3)
	     | ((base & ~0x0000000FU) << 1);
}

static uint32 OneBusTranslateRenderCHRAddress(uint32 A, uint8 attr, uint8 isSprite, uint8 plane) {
	static const uint8 midx[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
	const uint32 logicalBase = A & 0x1FFF;
	const uint8 page = (uint8)(((logicalBase >> 10) & 7) ^ ((mmc3cmd & 0x80) ? 4 : 0));
	const uint8 inner = OneBusPageInner(page);
	const uint8 mask = 0xFF >> midx[ppu201x[0xA] & 7];
	const uint8 extMode = UNLOneBusGetExtMode();
	const int extension = isSprite ? ((extMode & 0x08) != 0)
	                              : ((extMode & 0x10) != 0);
	uint32 bank;

	if (extension) {
		const uint8 eva = OneBusRenderEVA(attr, isSprite);
		bank = (((uint32)((inner & mask) | (ppu201x[0xA] & ~mask))) << 3)
		     | eva
		     | ((uint32)(cpu410x[0x0] & 0x0F) << 11);
	}
	else {
		bank = ((uint32)(cpu410x[0x0] & 0x0F) << 11)
		     | ((uint32)(ppu201x[0x8] & 0x70) << 4)
		     | (ppu201x[0xA] & ~mask)
		     | (inner & mask);
	}

	bank = (bank & OneBusCHRAnd) | OneBusCHROr;
	if (OneBusIsVT369) bank += VT369Relative8K() << 3;
	uint32 result = (bank << 10) | (logicalBase & 0x3FF);
	const int is4bpp = isSprite ? ((extMode & 0x04) != 0)
	                           : ((extMode & 0x02) != 0);
	if (is4bpp)
		result = OneBusPack4BPPAddress(result, plane, (extMode & 0x40) ? 1 : 0);
	else if (plane & 1)
		result += 8;
	return result;
}

static uint8 Mapper296CHRDecrypt(uint8 v) {
	if (!OneBusIsMapper296 || !(cpu410x[0x1E] & 0xC0))
		return v;
	/* VT3x CHR scrambling swaps D0<->D4, D1<->D2, D3<->D7, D5<->D6. */
	return (uint8)(((v & 0x01) << 4) | ((v & 0x10) >> 4) |
	               ((v & 0x02) << 1) | ((v & 0x04) >> 1) |
	               ((v & 0x08) << 4) | ((v & 0x80) >> 4) |
	               ((v & 0x20) << 1) | ((v & 0x40) >> 1));
}

static uint8 FASTCALL UNLOneBusPPUReadPatternExt(uint32 A, uint8 attr, uint8 isSprite, uint8 plane) {
	if (OneBusUseMappedCHR) {
		/* IVRCH is a 2 KiB internal RAM window. The normal two pattern planes
		 * remain useful here; upper VT03 planes mirror the same internal window. */
		uint32 logical = (A + ((plane & 1) ? 8 : 0)) & 0x1FFF;
		return VPage[logical >> 10][logical];
	}
	if (CHRptr[0] && CHRsize[0]) {
		const uint32 physical = OneBusWrapCHRAddress(OneBusTranslateRenderCHRAddress(A, attr, isSprite, plane));
		return Mapper296CHRDecrypt(CHRptr[0][physical]);
	}
	return 0;
}

/* VT32 $2010=$80 packed-color renderer ----------------------------------
 *
 * NRS NintendoEmulator does not send this mode through the VT03 plane
 * renderer.  Instead it indexes compacted 4bpp CHR halves directly and has
 * two distinct BG formats selected by $201C.0.  Reconstruct the equivalent
 * byte from FCEUX's original interleaved PRG/CHR image on demand; this avoids
 * allocating a second half-ROM copy just for the packed path. */
static uint8 FASTCALL UNLOneBusGetPackedMode(void) {
	if (OneBusPaletteFormat != ONEBUS_PAL_RGB444 || ppu201x[0x0] != 0x80)
		return 0;
	return (ppu201x[0x0C] & 0x01) ? 2 : 1;
}

static uint8 OneBusPackedReadNT(uint32 A) {
	A &= 0x2FFF;
	return vnapage[(A >> 10) & 3][A & 0x3FF];
}

/* Return the offset, in NRS's compact chrLow/chrHigh view, of one logical
 * 1 KiB bank pointer.  Packed mode is COLCOMP with no BKEXTEN, so h_OneBus
 * halves CHR bank units before applying the mapper-specific outer mask. */
static uint32 OneBusPackedSplitBase(uint8 logicalPage) {
	static const uint8 midx[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
	const uint8 mask = (uint8)(0xFF >> midx[ppu201x[0xA] & 7]);
	const uint8 page = (uint8)((logicalPage & 7) ^ ((mmc3cmd & 0x80) ? 4 : 0));
	const uint8 inner = OneBusPageInner(page);
	const uint32 va18 = (uint32)((ppu201x[0x8] >> 4) & 7);
	uint32 bank = (uint32)(inner & mask)
		| (uint32)(ppu201x[0xA] & (uint8)~mask)
		| (va18 << 8)
		| ((uint32)(cpu410x[0x0] & 0x0F) << 11);
	bank = (bank & (OneBusCHRAnd >> 1)) | (OneBusCHROr >> 1);
	return bank << 10;
}

/* NRS builds chrLow/chrHigh by compacting each raw 32-byte 4bpp tile to a
 * 16-byte half.  Convert a compact offset back to the original ROM byte. */
static uint8 OneBusPackedReadCompact(uint8 highHalf, uint8 logicalPage, uint32 offset) {
	if (!CHRptr[0] || CHRsize[0] < 2) return 0;
	const uint32 splitSize = CHRsize[0] >> 1;
	if (!splitSize) return 0;
	const uint32 split = OneBusWrapCHRSplitAddress(OneBusPackedSplitBase(logicalPage) + offset, splitSize);
	uint32 raw = ((split & ~0x0FU) << 1) | (split & 0x0FU);
	if (highHalf) raw += 0x10;
	raw = OneBusWrapCHRAddress(raw);
	/* NintendoEmulator's packed renderer dereferences CHRPointer directly;
	 * Mapper296's normal PPU read-handler descrambler is therefore not applied
	 * on this special path. */
	return CHRptr[0][raw];
}

static uint8 FASTCALL UNLOneBusPPUReadPackedBG(uint32 v, uint8 pixel) {
	const uint8 packedMode = UNLOneBusGetPackedMode();
	uint32 nt, pat;
	uint16 tl;
	if (!packedMode) return 0;
	pixel &= 7;

	if (packedMode == 1) {
		/* One 8bpp background.  The 16-bit nametable word supplies the tile
		 * number; fine Y and nametable bits contribute the remaining address. */
		nt = (v & 0x01E) | ((v >> 1) & 0x7E0) | 0x2000;
		tl = (uint16)OneBusPackedReadNT(nt);
		tl |= (uint16)OneBusPackedReadNT(nt + 1) << 8;
		pat = ((uint32)tl << 7)
			| ((v << 1) & 0x40)
			| ((v >> 9) & 0x30)
			| ((v << 3) & 0x08)
			| pixel;
		return OneBusPackedReadCompact((v & 0x1000) ? 1 : 0, 0, pat);
	}

	/* $201C.0=1: two packed 4bpp backgrounds.  Layer at $2400 is the base;
	 * a non-zero nibble from the $2000/CHR+2 layer overlays it. */
	uint8 result = 0;
	nt = (v & 0x01E) | ((v >> 1) & 0x7E0) | 0x2400;
	tl = (uint16)OneBusPackedReadNT(nt);
	tl |= (uint16)OneBusPackedReadNT(nt + 1) << 8;
	pat = (((uint32)tl << 6) & 0x3FFC0)
		| (v & 0x20) | ((v >> 10) & 0x10) | ((v >> 9) & 0x08)
		| ((pixel >> 1) & 7);
	{
		const uint8 b = OneBusPackedReadCompact((v & 0x2000) ? 1 : 0, 0, pat);
		result = (pixel & 1) ? (b >> 4) : (b & 0x0F);
		result |= (uint8)((tl >> 8) & 0xF0);
	}

	nt = (v & 0x01E) | ((v >> 1) & 0x7E0) | 0x2000;
	tl = (uint16)OneBusPackedReadNT(nt);
	tl |= (uint16)OneBusPackedReadNT(nt + 1) << 8;
	pat = (((uint32)tl << 6) & 0x3FFC0)
		| (v & 0x20) | ((v >> 10) & 0x10) | ((v >> 9) & 0x08)
		| ((pixel >> 1) & 7);
	{
		const uint8 b = OneBusPackedReadCompact((v & 0x2000) ? 1 : 0, 2, pat);
		const uint8 nib = (pixel & 1) ? (b >> 4) : (b & 0x0F);
		if (nib) result = (uint8)(nib | ((tl >> 8) & 0xF0));
	}
	return result;
}

static uint8 FASTCALL UNLOneBusPPUReadPackedSprite(uint8 tile, uint8 row, uint8 byteIndex) {
	if (!UNLOneBusGetPackedMode()) return 0;
	row &= 31; byteIndex &= 15;
	const uint32 pat = ((uint32)tile << 8) | (((uint32)row << 3) & 0xF0) | byteIndex;
	return OneBusPackedReadCompact((row & 1) ? 1 : 0, 4, pat);
}

static uint8 OneBusClampByte(double v) {
	if (v <= 0.0) return 0;
	if (v >= 1.0) return 255;
	return (uint8)(v * 255.0 + 0.5);
}

/* NewRisingSun's VT03 12-bit HSV/YUV conversion, also used by MAME. */
static void OneBusVT03Color(uint16 palval, uint8 *r, uint8 *g, uint8 *b) {
	int nPhase = palval & 0x0F;
	int nLuma = (palval >> 4) & 0x0F;
	int nChroma = (palval >> 8) & 0x0F;
	double phaseOffset = -11.0;
	if (nLuma < ((nChroma + 1) >> 1) || nLuma > 15 - (nChroma >> 1)) {
		static const uint8 altPhases[16] = { 13,7,8,9,10,11,12,1,2,3,4,5,6,0,14,15 };
		static const int8 altPhaseOffset[16] = { -5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,0,-5,-5,-5 };
		phaseOffset += altPhaseOffset[nPhase];
		nPhase = altPhases[nPhase];
		nChroma = 16 - nChroma;
		nLuma = (nLuma - 8) & 0x0F;
	}
	const double fLuma = (nLuma - 4) / 9.625;
	const double fChroma = nChroma / 18.975;
	const double fPhase = (((nPhase - 2) * 30.0) + phaseOffset) * 3.14159265358979323846 / 180.0;
	double Y = fLuma, C = fChroma;
	if (nPhase == 0 || nPhase > 12) C = 0.0;
	if (nPhase == 0) Y += fChroma;
	if (nPhase == 13) Y -= fChroma;
	if (nPhase >= 14) Y = 0.0;
	const double V = sin(fPhase) * C * 1.05;
	const double U = cos(fPhase) * C * 1.05;
	*r = OneBusClampByte(Y + 1.1400 * V);
	*g = OneBusClampByte(Y - 0.5807 * V - 0.3940 * U);
	*b = OneBusClampByte(Y + 2.0290 * U);
}

/* VT31/VT32 enhanced palette: BBBB GGGG RRRR. */
static void OneBusVT32Color(uint16 palval, uint8 *r, uint8 *g, uint8 *b) {
	*r = (uint8)((palval & 0x000F) * 17);
	*g = (uint8)(((palval >> 4) & 0x000F) * 17);
	*b = (uint8)(((palval >> 8) & 0x000F) * 17);
}

/* VT368/VT369 enhanced palette: xRRRRRGGGGGBBBBB (BGR555 in memory).
 * Expand 5-bit channels with bit replication so 31 maps exactly to 255. */
static void OneBusVT369Color(uint16 palval, uint8 *r, uint8 *g, uint8 *b) {
	const uint8 b5 = (uint8)(palval & 0x1F);
	const uint8 g5 = (uint8)((palval >> 5) & 0x1F);
	const uint8 r5 = (uint8)((palval >> 10) & 0x1F);
	*r = (uint8)((r5 << 3) | (r5 >> 2));
	*g = (uint8)((g5 << 3) | (g5 >> 2));
	*b = (uint8)((b5 << 3) | (b5 >> 2));
}

/* VT369 $3C00-$3FFF contains 512 little-endian BGR555 colours: 256 BG then
 * 256 OBJ.  Keep this palette completely separate from FCEUX's ordinary
 * 256-entry output palette.  Stage 2.4.1 marks VT369 direct-colour pixels in
 * XDBuf, so group zero no longer needs to steal ordinary palette slots 0..63. */
static void OneBusUpdateVT369DirectPaletteEntry(uint16 index) {
	index &= 0x1FF;
	const uint16 off = (uint16)(index << 1);
	const uint16 palval = (uint16)VT369PaletteRAM[off] | ((uint16)VT369PaletteRAM[off + 1] << 8);
	uint8 r, g, b;
	OneBusVT369Color(palval, &r, &g, &b);
	FCEU_DirectPalette512[index].r = r;
	FCEU_DirectPalette512[index].g = g;
	FCEU_DirectPalette512[index].b = b;
}

/* FCEUD_SetPalette is also the frontend's "palette translation dirty" signal.
 * Touch an ordinary entry with its current RGB instead of copying VT369 direct
 * colour 0 into it; this preserves NES/HUD colours while still rebuilding the
 * 512-entry high-colour translation table. */
static void OneBusTouchOutputPalette(void) {
	uint8 r, g, b;
	FCEUD_GetPalette(0, &r, &g, &b);
	FCEUD_SetPalette(0, r, g, b);
}

static void OneBusUpdateVT369DirectPalette(void) {
	if (!OneBusIsVT369 || !ppu201x[0x0E]) return;
	/* Stage 2.4.3: entering VT369 direct RGB555 must restore the ordinary
	 * FCEUX output palette first.  Older COLCOMP writes may have repurposed
	 * $80-$FF, which are exactly the colours used by FPS/input/movie HUD. */
	if (!FCEU_DirectPalette512Enabled) {
		FCEU_ResetPalette();
		OneBusOutputPaletteDirty = 0;
	}
	FCEU_DirectPalette512Enabled = true;
	for (uint16 i = 0; i < 512; ++i) OneBusUpdateVT369DirectPaletteEntry(i);
}

static void OneBusUpdatePackedPalettePen(uint8 pen) {
	if (!UNLOneBusGetPackedMode()) return;
	const uint16 base = (pen & 0x80) ? 0x100 : 0x000;
	const uint16 idx = pen & 0x7F;
	const uint16 palval = (OneBusCGRAM[base + idx] & 0x3F)
		| ((uint16)(OneBusCGRAM[base + 0x80 + idx] & 0x3F) << 6);
	uint8 r, g, b;
	OneBusVT32Color(palval, &r, &g, &b);
	FCEUD_SetPalette(pen, r, g, b);
	OneBusOutputPaletteDirty = 1;
}

static void OneBusUpdatePalettePen(uint8 pen) {
	/* VT369 enhanced RGB555 has its own per-pixel direct-colour domain. Never
	 * overwrite FCEUX's ordinary $80-$FF output palette while that renderer is
	 * active; those entries belong to the emulator HUD/OSD. */
	if (OneBusIsVT369 && ppu201x[0x0E]) return;
	/* Only COLCOMP needs private RGB output slots. In compatible color mode
	 * the renderer resolves CGRAM to an ordinary 6-bit NES color and leaves
	 * FCEUX's $80-$FF output palette untouched, preserving HUD/OSD colors. */
	if (!OneBusHasEnhancedPalette || !(UNLOneBusGetExtMode() & 0x80)) return;
	uint8 r, g, b;
	pen &= 0x7F;
	uint16 palval;
	if (OneBusPaletteFormat == ONEBUS_PAL_RGB555) {
		palval = (uint16)OneBusCGRAM[pen] | ((uint16)(OneBusCGRAM[pen | 0x80] & 0x7F) << 8);
		OneBusVT369Color(palval, &r, &g, &b);
	}
	else {
		palval = (OneBusCGRAM[pen] & 0x3F) | ((uint16)(OneBusCGRAM[pen | 0x80] & 0x3F) << 6);
		if (OneBusPaletteFormat == ONEBUS_PAL_RGB444)
			OneBusVT32Color(palval, &r, &g, &b);
		else
			OneBusVT03Color(palval, &r, &g, &b);
	}
	FCEUD_SetPalette((uint8)(0x80 | pen), r, g, b);
	OneBusOutputPaletteDirty = 1;
}

static void OneBusRestoreOutputPalette(void) {
	/* VT369 Stage 2 can replace the high-color backend's 512-entry source. */
	FCEU_DirectPalette512Enabled = false;
	if (!OneBusOutputPaletteDirty) return;
	FCEU_ResetPalette();
	OneBusOutputPaletteDirty = 0;
}

static void OneBusUpdateAllPalette(void) {
	if (!OneBusHasEnhancedPalette || !(UNLOneBusGetExtMode() & 0x80)) return;
	if (UNLOneBusGetPackedMode()) {
		for (int i = 0; i < 0x100; ++i) OneBusUpdatePackedPalettePen((uint8)i);
		return;
	}
	for (int i = 0; i < 0x80; ++i) OneBusUpdatePalettePen((uint8)i);
}

static void FASTCALL UNLOneBusRefreshExtPalette(void) {
	/* OSD composition may temporarily borrow unused game palette entries.
	 * Rebuild the complete hardware-owned palette before the next frame scan. */
	if (OneBusIsVT369 && ppu201x[0x0E]) {
		OneBusUpdateVT369DirectPalette();
		return;
	}
	OneBusUpdateAllPalette();
}

static void OneBusPaletteWrite(uint32 A, uint8 V) {
	uint8 off = (uint8)(A & 0xFF);
	const uint8 vt32 = (OneBusPaletteFormat == ONEBUS_PAL_RGB444) ? 1 : 0;
	const uint8 vt369 = (OneBusPaletteFormat == ONEBUS_PAL_RGB555) ? 1 : 0;
	const uint8 rawV = V;
	const uint8 vt32NoUniversalMirror = (vt32 && (ppu201x[0x0] & 0x80)) ? 1 : 0;

	/* NRS VT32 exposes a second 256-byte palette page at PPU $3E00-$3EFF.
	 * It is primarily used by the packed-color VT32 path. Keep it separate
	 * from the ordinary $3Fxx page even before the full packed renderer is
	 * enabled so CPU/DMA palette uploads land in the correct hardware RAM. */
	if (vt32 && (A & 0x3F00) == 0x3E00) {
		OneBusCGRAM[0x100 | off] = V;
		if (UNLOneBusGetPackedMode())
			OneBusUpdatePackedPalettePen((uint8)(0x80 | (off & 0x7F)));
		return;
	}

	V = vt369 ? rawV : (uint8)(rawV & 0x3F);
	if (!OneBusHasEnhancedPalette) {
		FFCEUX_PPUWrite_Default(0x3F00 | (off & 0x1F), rawV & 0x3F);
		return;
	}

	OneBusCGRAM[off] = V;
	/* The normal universal-color mirrors are disabled by VT32's bit7
	 * enhanced/packed palette mode. Older VT02/03/09 behavior is unchanged. */
	if (!vt32NoUniversalMirror && off < 0x20 && !(off & 0x03))
		OneBusCGRAM[off ^ 0x10] = V;

	if (off < 0x20)
		FFCEUX_PPUWrite_Default(0x3F00 | off, rawV & 0x3F);

	if (UNLOneBusGetPackedMode())
		OneBusUpdatePackedPalettePen(off & 0x7F);
	else
		OneBusUpdatePalettePen(off & 0x7F);
	if (off & 0x80) OneBusUpdatePalettePen(off & 0x7F);
	if (!vt32NoUniversalMirror && off < 0x20 && !(off & 3))
		OneBusUpdatePalettePen((off ^ 0x10) & 0x7F);
}

static uint32 OneBusTranslateCPUCHRAddress(uint32 A) {
	static const uint8 midx[8] = { 0, 1, 2, 0, 3, 4, 5, 0 };
	const uint8 mask = 0xFF >> midx[ppu201x[0xA] & 7];
	const uint8 page = (uint8)(((A >> 10) & 7) ^ ((mmc3cmd & 0x80) ? 4 : 0));
	uint8 inner;

	switch (page) {
	case 0: inner = ppu201x[0x6] & ~1; break;
	case 1: inner = ppu201x[0x6] |  1; break;
	case 2: inner = ppu201x[0x7] & ~1; break;
	case 3: inner = ppu201x[0x7] |  1; break;
	case 4: inner = ppu201x[0x2]; break;
	case 5: inner = ppu201x[0x3]; break;
	case 6: inner = ppu201x[0x4]; break;
	default: inner = ppu201x[0x5]; break;
	}

	uint32 bank;
	const uint8 extMode = UNLOneBusGetExtMode();
	if (extMode & 0x18) {
		bank = (((uint32)((inner & mask) | (ppu201x[0xA] & ~mask))) << 3)
		     | (ppu201x[0x8] & 0x07)
		     | ((uint32)(cpu410x[0x0] & 0x0F) << 11);
	}
	else {
		bank = ((uint32)(cpu410x[0x0] & 0x0F) << 11)
		     | ((uint32)(ppu201x[0x8] & 0x70) << 4)
		     | (ppu201x[0xA] & ~mask)
		     | (inner & mask);
	}

	bank = (bank & OneBusCHRAnd) | OneBusCHROr;
	if (OneBusIsVT369) bank += VT369Relative8K() << 3;
	uint32 result = (bank << 10) | (A & 0x3FF);

	/* 4bpp widens the video data layout. Bit 6 selects the later alternate
	 * 16-bit ordering; VT03 uses the 16-byte-group interleave below. */
	if (extMode & 0x06) {
		if (extMode & 0x40)
			result <<= 1;
		else
			result = (result & 0x0F) | ((result & ~0x0F) << 1);
	}

	return result;
}

static uint8 FASTCALL UNLOneBusPPUReadCPU(uint32 A) {
	A &= 0x3FFF;
	if (OneBusIsVT369 && ppu201x[0x0E] && A >= 0x3C00)
		return VT369PaletteRAM[A & 0x3FF];
	if (OneBusPaletteFormat == ONEBUS_PAL_RGB444 && (A & 0x3F00) == 0x3E00)
		return OneBusCGRAM[0x100 | (A & 0xFF)];
	if (A >= 0x3F00)
		return OneBusHasEnhancedPalette ? OneBusCGRAM[A & 0xFF] : FFCEUX_PPURead_Default(A);
	if (A < 0x2000 && !OneBusUseMappedCHR && CHRptr[0] && CHRsize[0]) {
		uint32 physical = OneBusWrapCHRAddress(OneBusTranslateCPUCHRAddress(A));
		if (PPU_hook) PPU_hook(A);
		return Mapper296CHRDecrypt(CHRptr[0][physical]);
	}
	if (OneBusOldPPUReadCPU && OneBusOldPPUReadCPU != UNLOneBusPPUReadCPU)
		return OneBusOldPPUReadCPU(A);
	return FFCEUX_PPURead(A);
}

static void UNLOneBusPPUWriteCPU(uint32 A, uint8 V) {
	A &= 0x3FFF;
	if (OneBusIsVT369 && ppu201x[0x0E] && A >= 0x3C00) {
		VT369WritePaletteByte(A & 0x3FF, V);
		return;
	}
	if ((OneBusPaletteFormat == ONEBUS_PAL_RGB444 && (A & 0x3F00) == 0x3E00) ||
		A >= 0x3F00) {
		OneBusPaletteWrite(A, V);
		return;
	}
	if (A < 0x2000 && !OneBusUseMappedCHR && CHRram[0] && CHRptr[0] && CHRsize[0]) {
		uint32 physical = OneBusWrapCHRAddress(OneBusTranslateCPUCHRAddress(A));
		if (PPU_hook) PPU_hook(A);
		CHRptr[0][physical] = V;
		return;
	}
	if (OneBusOldPPUWriteCPU && OneBusOldPPUWriteCPU != UNLOneBusPPUWriteCPU)
		OneBusOldPPUWriteCPU(A, V);
	else if (FFCEUX_PPUWrite)
		FFCEUX_PPUWrite(A, V);
	else
		FFCEUX_PPUWrite_Default(A, V);
}

static void UNLOneBusInstallPPUBus(void) {
	if (FFCEUX_PPUReadCPU != UNLOneBusPPUReadCPU)
		OneBusOldPPUReadCPU = FFCEUX_PPUReadCPU;
	if (FFCEUX_PPUWriteCPU != UNLOneBusPPUWriteCPU)
		OneBusOldPPUWriteCPU = FFCEUX_PPUWriteCPU;
	FFCEUX_PPUReadCPU = UNLOneBusPPUReadCPU;
	FFCEUX_PPUWriteCPU = UNLOneBusPPUWriteCPU;
	FFCEUX_PPUReadPatternExt = UNLOneBusPPUReadPatternExt;
	FFCEUX_PPUGetPackedMode = UNLOneBusGetPackedMode;
	FFCEUX_PPUReadPackedBG = UNLOneBusPPUReadPackedBG;
	FFCEUX_PPUReadPackedSprite = UNLOneBusPPUReadPackedSprite;
	FFCEUX_PPUGetExtMode = UNLOneBusGetExtMode;
	FFCEUX_PPUMapExtPalette = UNLOneBusMapExtPalette;
	FFCEUX_PPUUseFixedOSD = UNLOneBusUseFixedOSD;
	FFCEUX_PPURefreshExtPalette = UNLOneBusRefreshExtPalette;
	FFCEUX_PPUGetVT369Mode = UNLOneBusGetVT369Mode;
	FFCEUX_PPUReadVT369Reg = UNLOneBusReadVT369Reg;
	FFCEUX_PPUReadVT369BG = UNLOneBusPPUReadVT369BG;
	FFCEUX_PPUReadVT369BG16 = UNLOneBusPPUReadVT369BG16;
	FFCEUX_PPUReadVT369Sprite = UNLOneBusPPUReadVT369Sprite;
	FFCEUX_PPUReadVT369HiResSprite = UNLOneBusPPUReadVT369HiResSprite;
	FFCEUX_PPUGetVT369SpriteConfig = UNLOneBusGetVT369SpriteConfig;
	FFCEUX_PPUReadVT369OAM = UNLOneBusPPUReadVT369OAM;
	FFCEUX_PPUGetVT369OAMHigh = UNLOneBusPPUGetVT369OAMHigh;
	FFCEUX_PPUWriteVT369OAM = UNLOneBusPPUWriteVT369OAM;
}

static void SyncEncryption (void) {
	encryptOpcodes = 0;
	if (submapper == 10 && cpu410x[0x1C] &0x02) encryptOpcodes =10;
	if (submapper == 11) {
		encryptOpcodes = 11;
		encryptOpcodesConfig = cpu410x[0x1C];
	}
	if (submapper == 12 && cpu410x[0x1C] &0x40) encryptOpcodes =12;
	if (submapper == 14 && cpu410x[0x1C] &0x40) encryptOpcodes =14;
	/* Taikee/Jungletac power on encrypted; $4169.0=1 disables it.
	 * Use new internal mode IDs because 15/16 are already VT32 XOR modes. */
	if (submapper == 13 && !(VT369Protection4169 & 0x01)) encryptOpcodes =17;
	if (submapper == 15 && !(VT369Protection4169 & 0x01)) encryptOpcodes =18;
}

static void VT369Sync6000(void) {
	if (!OneBusIsVT369) return;
	if (cpu410x[0x1C] & 0x40) {
		const uint32 bank = OneBusPRGBankFromRegister(cpu410x[0x12]) + VT369Relative8K();
		setprg8(0x6000, bank);
	} else {
		setprg8r(0x10, 0x6000, 0);
	}
}

static void Sync256(void) {
	PSync(0x0FFF, 0x000);
	CSync(0x7FFF, 0x000);
	VT369Sync6000();
	SyncEncryption();
}

static const uint8 cpuMangles[16][4] = {
	{ 0, 1, 2, 3 }, 	/* Submapper 0: Normal                                  */
	{ 0, 1, 2, 3 }, 	/* Submapper 1: Waixing VT03                            */
	{ 1, 0, 2, 3 }, 	/* Submapper 2: Trump Grand                             */
	{ 0, 1, 2, 3 }, 	/* Submapper 3: Zechess                                 */
	{ 0, 1, 2, 3 }, 	/* Submapper 4: Qishenglong                             */
	{ 0, 1, 2, 3 }, 	/* Submapper 5: Waixing VT02                            */
	{ 0, 1, 2, 3 }, 	/* Submapper 6: unused so far                           */
	{ 0, 1, 2, 3 }, 	/* Submapper 7: unused so far                           */
	{ 0, 1, 2, 3 }, 	/* Submapper 8: unused so far                           */
	{ 0, 1, 2, 3 }, 	/* Submapper 9: unused so far                           */
	{ 0, 1, 2, 3 }, 	/* Submapper A: CPU opcode encryption                   */
	{ 0, 1, 2, 3 }, 	/* Submapper B: CPU opcode encryption                   */
	{ 0, 1, 2, 3 }, 	/* Submapper C: Cheertone (CPU opcode encryption only)  */
	{ 0, 1, 2, 3 }, 	/* Submapper D: Taikee (CPU opcode encryption only)  */
	{ 0, 1, 2, 3 }, 	/* Submapper E: Karaoto (CPU opcode encryption only)    */
	{ 0, 1, 2, 3 }  	/* Submapper F: Jungletac (CPU opcode encryption only)  */
};
/* Stage 4.3 R1: VT timer compatibility model.
 *
 * Furbtendulator/NintendulatorNRS model this as a continuously clocked
 * scanline counter. $4102 does not immediately copy $4101 into the live
 * counter; it clears/reloads the counter so the NEXT scanline clock loads the
 * preload value. This produces the hardware-observed reload-then-count (N+1)
 * phase used by VT03 raster splits. Games also write a new $4101 value from
 * inside the IRQ handler without writing $4102; the new preload must therefore
 * be picked up by the automatic reload after the just-fired zero count.
 *
 * TSYNEN ($410B.7) selects AD12 vs HSYNC in silicon. FCEUX's classic renderer
 * exposes one stable HBlank callback per rendered line, which is also the
 * long-standing NRS/FCEUMM approximation used by these carts. Stage4.3 R1
 * deliberately keeps that compatibility clock here instead of the previous
 * fetch-level PPU_hook approximation, which re-phased real VT03 raster IRQs
 * and corrupted the lower part of the screen. */
static void UNLOneBusTimerIRQEnd(void) {
	IRQPending = 0;
	X6502_IRQEnd(ONEBUS_TIMER_IRQ);
}

static void UNLOneBusTimerRestart(void) {
	/* Furb h_OneBus: $4102 -> counter = 0.  The next scanline tick reloads
	 * IRQLatch; it does not consume a count on the same tick. */
	IRQCount = 0;
	IRQReload = 1;
}

static void UNLOneBusTimerClock(void) {
	const uint8 count = IRQCount;

	if (!count || IRQReload) {
		IRQCount = IRQLatch;
		IRQReload = 0;
	}
	else {
		IRQCount--;
	}

	/* Trigger only on a nonzero -> zero transition.  In this Furb/NRS
	 * compatibility model a zero preload reloads zero and therefore does not
	 * immediately assert an IRQ every line.  No 256-wrap behavior is claimed. */
	if (count && !IRQCount && IRQa) {
		IRQPending = 1;
		X6502_IRQBegin(ONEBUS_TIMER_IRQ);
	}
}

static DECLFR(UNLOneBusReadVT32ALU) {
	switch (A & 0x3F) {
	case 0x30: case 0x38: return (uint8)(vt32_alu_operand14 >> 0);
	case 0x31: case 0x39: return (uint8)(vt32_alu_operand14 >> 8);
	case 0x32: case 0x3A: return (uint8)(vt32_alu_operand14 >> 16);
	case 0x33: case 0x3B: return (uint8)(vt32_alu_operand14 >> 24);
	case 0x34: case 0x3C: return (uint8)(vt32_alu_operand56 >> 0);
	case 0x35: case 0x3D: return (uint8)(vt32_alu_operand56 >> 8);
	case 0x36: return vt32_alu_busy;
	default: return 0xFF;
	}
}

static DECLFW(UNLOneBusWriteCPU410X) {
/*	FCEU_printf("CPU %04x:%04x\n",A,V); */
	A &=0x3F;
	if (OneBusHasALU && A >= 0x30 && A <= 0x37) {
		cpu410x[A] = V;
		switch (A) {
		case 0x30: vt32_alu_operand14 = (vt32_alu_operand14 & 0xFFFFFF00U) | ((uint32)V << 0); break;
		case 0x31: vt32_alu_operand14 = (vt32_alu_operand14 & 0xFFFF00FFU) | ((uint32)V << 8); break;
		case 0x32: vt32_alu_operand14 = (vt32_alu_operand14 & 0xFF00FFFFU) | ((uint32)V << 16); break;
		case 0x33: vt32_alu_operand14 = (vt32_alu_operand14 & 0x00FFFFFFU) | ((uint32)V << 24); break;
		case 0x34: vt32_alu_operand56 = (uint16)((vt32_alu_operand56 & 0xFF00U) | V); break;
		case 0x35:
			vt32_alu_operand56 = (uint16)((vt32_alu_operand56 & 0x00FFU) | ((uint16)V << 8));
			vt32_alu_operand14 = (uint32)(vt32_alu_operand14 & 0xFFFFU) * vt32_alu_operand56;
			vt32_alu_busy = 16;
			OneBusUpdateCpuHook();
			break;
		case 0x36: vt32_alu_operand67 = (uint16)((vt32_alu_operand67 & 0xFF00U) | V); break;
		case 0x37:
			vt32_alu_operand67 = (uint16)((vt32_alu_operand67 & 0x00FFU) | ((uint16)V << 8));
			if (vt32_alu_operand67) {
				const uint32 dividend = vt32_alu_operand14;
				vt32_alu_operand56 = (uint16)(dividend % vt32_alu_operand67);
				vt32_alu_operand14 = dividend / vt32_alu_operand67;
				vt32_alu_busy = 32;
				OneBusUpdateCpuHook();
			}
			break;
		}
		return;
	}
	switch (A) {
	case 0x1: IRQLatch = V; break;
	case 0x2: UNLOneBusTimerRestart(); break;
	case 0x3: IRQa = 0; UNLOneBusTimerIRQEnd(); break;
	case 0x4: IRQa = 1; break;
	default:
		if (A >=0x7 && A <=0xA) A =0x7 +cpuMangle[A -0x7];
		/* Packed/extended VT rendering derives physical CHR addresses from the
		 * live $410x state. Flush before changing it, for the same reason as
		 * $201x above. This is particularly visible when menus switch banks
		 * while moving the selection cursor. */
		if (cpu410x[A] != V) FCEUPPU_LineUpdate();
		cpu410x[A] = V;
		if (OneBusIsVT369 && A == 0x1C)
			X6502_SetOneBusClockMultiplier((V & 0x80) ? 3 : 1);
		Sync();
	}
}

static DECLFW(UNLOneBusWriteCPU4242) {
	reg4242 =V;
	Sync();
}

static const uint8 ppuMangles[16][6] = {
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 0: Normal                                  */
	{ 1, 0, 5, 4, 3, 2 }, 	/* Submapper 1: Waixing VT03                            */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 2: Trump Grand                             */
	{ 5, 4, 3, 2, 0, 1 }, 	/* Submapper 3: Zechess                                 */
	{ 2, 5, 0, 4, 3, 1 }, 	/* Submapper 4: Qishenglong                             */
	{ 1, 0, 5, 4, 3, 2 }, 	/* Submapper 5: Waixing VT02                            */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 6: unused so far                           */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 7: unused so far                           */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 8: unused so far                           */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper 9: unused so far                           */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper A: CPU opcode encryption                   */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper B: CPU opcode encryption                   */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper C: Cheertone (CPU opcode encryption only)  */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper D: Taikee (CPU opcode encryption only)  */
	{ 0, 1, 2, 3, 4, 5 }, 	/* Submapper E: Karaoto (CPU opcode encryption only)    */
	{ 0, 1, 2, 3, 4, 5 }  	/* Submapper F: Jungletac (CPU opcode encryption only)  */
};
static DECLFW(UNLOneBusWritePPU201X) {
/*	FCEU_printf("PPU %04x:%04x\n",A,V); */
	A &=0x0F;
	if (A >=2 && A <=7) A =2 +ppuMangle[A -2];
	const uint8 old = ppu201x[A];
	/* The classic FCEUX PPU renders a scanline lazily.  VT render callbacks
	 * consult ppu201x live, so changing the register before LineUpdate would
	 * retroactively render the already elapsed part of the line with the new
	 * bank/mode.  NRS's cycle PPU naturally commits the old pixels first. */
	if (old != V) FCEUPPU_LineUpdate();
	ppu201x[A] = V;
	if (OneBusIsVT369 && A == 0x0E && old != V) {
		if (V) OneBusUpdateVT369DirectPalette();
		else OneBusRestoreOutputPalette();
	}
	if (A == 0 && old != V) {
		const uint8 newComp = UNLOneBusGetExtMode() & 0x80;
		if (!newComp)
			OneBusRestoreOutputPalette();
		else
			OneBusUpdateAllPalette();
	}
	Sync();
}

static const uint8 mmc3Mangles[16][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 0: Normal                                 */
	{ 5, 4, 3, 2, 1, 0, 6, 7 }, 	/* Submapper 1: Waixing VT03                           */
	{ 0, 1, 2, 3, 4, 5, 7, 6 }, 	/* Submapper 2: Trump Grand                            */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 3: Zechess                                */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 4: Qishenglong                            */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 5: Waixing VT02                           */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 6: unused so far                          */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 7: unused so far                          */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 8: unused so far                          */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper 9: unused so far                          */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper A: CPU opcode encryption                  */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper B: CPU opcode encryption                  */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper C: Cheertone (CPU opcode encryption only) */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper D: Taikee (CPU opcode encryption only) */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, 	/* Submapper E: Karaoto (CPU opcode encryption only)   */
	{ 0, 1, 2, 3, 4, 5, 6, 7 }  	/* Submapper F: Jungletac (CPU opcode encryption only) */
};

static DECLFW(UNLOneBusWriteMMC3) {
/*	FCEU_printf("MMC %04x:%04x\n",A,V); */
	FCEUPPU_LineUpdate();
	if (~cpu410x[0x0B] &0x08) /* FWEN bit must be 0 */
	switch (A & 0xe001) {
	case 0x8000: 
		V =V &0xF8 | mmc3Mangle[V &0x07];
		/* MMC3-compatible $8000 forwards to $4105 except IVRCH (bit 5). */
		mmc3cmd = (mmc3cmd & 0x18) | (V & 0xc7);
		Sync();
		break;
	case 0x8001:
	{
		switch (mmc3cmd & 7) {
		case 0: ppu201x[0x6] = V; Sync(); break;
		case 1: ppu201x[0x7] = V; Sync(); break;
		case 2: ppu201x[0x2] = V; Sync(); break;
		case 3: ppu201x[0x3] = V; Sync(); break;
		case 4: ppu201x[0x4] = V; Sync(); break;
		case 5: ppu201x[0x5] = V; Sync(); break;
		case 6: cpu410x[0x7] = V; Sync(); break;
		case 7: cpu410x[0x8] = V; Sync(); break;
		}
		break;
	}
	case 0xa000: mirror = V; Sync(); break;
	case 0xc000: IRQLatch = V; break;
	case 0xc001: UNLOneBusTimerRestart(); break;
	case 0xe000: IRQa = 0; UNLOneBusTimerIRQEnd(); break;
	case 0xe001: IRQa = 1; break;
	}
}

static void UNLOneBusIRQHook(void) {
	/* Compatibility scanline clock. See the Stage4.3 R1 timer note above. */
	UNLOneBusTimerClock();
}


static int UNLOneBusVT32WriteAudio(uint32 A, uint8 reg, uint8 V) {
	int ch;
	if (OneBusAudioGeneration != ONEBUS_AUDIO_VT32)
		return 0;

	/* VT32 R3 mirrors NRS APU_VT32 rather than the earlier provisional R2
	 * interpretation.  In particular, $4032 is the common sample period and
	 * $4012/$4035/$4036 form one shared physical-address latch. */
	switch (reg) {
	case 0x10: /* Ordinary RP2A03 DMC control while VT32 PCM is idle. */
		if (!vt32_pcm_playing[0] && !vt32_pcm_playing[1]) {
			apu40xx[reg] = V;
			defapuwrite[reg](A, V);
		}
		return 1;
	case 0x11: /* Ordinary RP2A03 DMC DAC. */
	case 0x13: /* Ordinary RP2A03 DMC sample length. */
	case 0x15: /* Ordinary APU/DMC enables. */
		apu40xx[reg] = V;
		defapuwrite[reg](A, V);
		return 1;
	case 0x12: /* Shared VT32 address latch A13:A6 + ordinary DMC address. */
		apu40xx[reg] = V;
		vt32_pcm_programmed[0] =
			(vt32_pcm_programmed[0] & ~(0xFFU << 6)) | ((uint32)V << 6);
		vt32_pcm_programmed[1] = vt32_pcm_programmed[0];
		defapuwrite[reg](A, V);
		return 1;
	case 0x30:
		/* Keep the OneBus shadow register but do not apply VT03 DWS routing to
		 * VT32's separate two-channel sample engine. */
		apu40xx[reg] = V;
		return 1;
	case 0x31:
		/* NRS falls through to the base OneBus raw-DAC register here. DGUN2573
		 * writes $80/$00, but this is not a VT32 channel selector/volume. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		pcm_output = V;
		return 1;
	case 0x32: /* Shared sample period, in CPU cycles. NRS commonly uses $6F. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		UNLOneBusVT32PCMSetPeriod(V);
		return 1;
	case 0x33: { /* ADPCM/PCM mode and per-channel start bits. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		for (ch = 0; ch < 2; ++ch) {
			const uint8 bit = ch ? 0x08 : 0x10;
			if ((V & bit) && !vt32_pcm_playing[ch])
				UNLOneBusVT32PCMStartChannel(ch, V & 0xC0);
			/* NRS: clearing a channel bit stops PCM (bit7), but ADPCM keeps
			 * running until $4037-$4039 or both mode bits are clear. */
			if (!(V & bit) && (V & 0x80) && vt32_pcm_playing[ch])
				UNLOneBusVT32PCMStopChannel(ch, 0);
		}
		if (!(V & 0xC0)) {
			UNLOneBusVT32PCMStopChannel(0, 0);
			UNLOneBusVT32PCMStopChannel(1, 0);
		}
		return 1;
	}
	case 0x35: /* Shared address latch A20:A14. */
		apu40xx[reg] = V;
		vt32_pcm_programmed[0] =
			(vt32_pcm_programmed[0] & ~(0x7FU << 14)) |
			(((uint32)V & 0x7F) << 14);
		vt32_pcm_programmed[1] = vt32_pcm_programmed[0];
		return 1;
	case 0x36: /* Shared address latch A28:A21. */
		apu40xx[reg] = V;
		vt32_pcm_programmed[0] =
			(vt32_pcm_programmed[0] & ~(0xFFU << 21)) | ((uint32)V << 21);
		vt32_pcm_programmed[1] = vt32_pcm_programmed[0];
		return 1;
	case 0x37: /* NRS: stop channel 0. */
	case 0x38: /* NRS: stop channel 1. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		UNLOneBusVT32PCMStopChannel(reg == 0x38 ? 1 : 0, 1);
		return 1;
	case 0x39: /* Explicit channel-stop bits, particularly for ADPCM. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		if (V & 0x10) UNLOneBusVT32PCMStopChannel(0, 1);
		if (V & 0x08) UNLOneBusVT32PCMStopChannel(1, 1);
		return 1;
	default:
		return 0;
	}
}

static DECLFW(UNLOneBusWriteAPU40XX) {
	const uint8 reg = A & 0x3F;
	const uint8 oldValue = apu40xx[reg];
	if (UNLOneBusVT32WriteAudio(A, reg, V))
		return;
	switch (reg) {
	case 0x10: /* VT DWS/PCM control; do not forward to RP2A03 DMC. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		if ((oldValue ^ V) & 0x0F) {
			pcm_phase = 0;
			UNLOneBusPCMUpdateRate();
		}
		return;
	case 0x11: /* DWS initial amplitude (seven bits). */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V & 0x7F;
		/* IA is latched by DWS start/repeat, not a live DAC write. */
		pcm_dws_initial = V & 0x7F;
		return;
	case 0x12: /* DWS/PCM start address bits A13-A6. */
		apu40xx[reg] = V;
		pcm_start_addr = (uint16)((uint16)V << 6) & 0x7FFF;
		if (pcm_enable)
			pcm_addr = pcm_start_addr;
		return;
	case 0x13: /* DWS/PCM length in 16-byte units, plus one. */
		apu40xx[reg] = V;
		pcm_start_size = (uint16)(((uint16)V << 4) + 1);
		if (pcm_enable) {
			pcm_size = pcm_start_size;
			pcm_end_pending = 0;
		}
		return;
	case 0x14: /* VT DMA trigger. VT369 $411C.7 selects the new $4024 source-low engine. */
		apu40xx[reg] = V;
		{
			const uint8 cfg = apu40xx[0x34];
			const uint8 code = (cfg >> 1) & 7;
			/* VT369 new-style DMA: source=$4014:$4024. Length encoding is
			 * 2/4/8/16/32/64/128 bytes, with code 0 meaning 256 bytes. */
			if (OneBusIsVT369 && (cpu410x[0x1C] & 0x80)) {
				const uint32 t369 = ((uint32)V << 8) | apu40xx[0x24];
				uint32 length369 = code ? (1U << code) : 256U;
				X6502_AddCycles(1 + (timestamp & 1));
				/* Generic VT369 DMA still reaches VRAM through $2007 and OAM through
				 * $2004.  Some later LCD/GPIO variants use the separately captured
				 * VT369DMAVRAMTarget for direct VRAM writes; keep that latch modeled
				 * in the PPU, but do not enable the variant-specific bypass globally. */
				for (uint32 x369 = 0; x369 < length369; ++x369)
					X6502_DMW((cfg & 1) ? 0x2007 : 0x2004, X6502_DMR((t369 + x369) & 0xFFFF));
				return;
			}

			uint32 t = ((uint32)V << 8) | (cfg & 0xF0);
			uint32 length = 0;
			uint32 x;

			/* DGUN2573/Mapper296 software identifies VT3x through $4119=$00
			 * and compensates for the legacy CGRAM DMA off-by-one by starting
			 * at $3F01. Reproduce that compensated case without changing the
			 * already-accepted VT02/VT03/VT09 DMA behavior. */
			if (OneBusIsMapper296 && (cfg & 0x01))
				FCEUX_PPUApplyVTCGRAMDMABug();

			switch (code) {
			case 0: length = 256; break;
			case 4: length = 16; break;
			case 5: length = 32; break;
			case 6: length = 64; break;
			case 7: length = 128; break;
			default: return; /* 001-011 are not defined by VT02+/VT03. */
			}

			/* 64/128-byte modes stop at their segment boundary rather than
			 * crossing into the next piece.  16-byte sources are naturally
			 * aligned because $4034 only supplies address bits 7-4. */
			if (length == 64 || length == 128) {
				const uint32 remain = length - (t & (length - 1));
				if (remain < length) length = remain;
			}

			/* VT369 enhanced OAM uses the ordinary $2004 write path.  Its board
			 * callback performs the 9-bit page toggle when the low $2003 address
			 * wraps, so DMA must not apply a second page transition here. */

			/* FCEUX models VT video/sprite DMA with the same CPU-bus halt/alignment
			 * convention as its OAM DMA.  The VT datasheets document transfer
			 * lengths/targets but not the exact halt phase, so this is deliberately
			 * a CPU-cycle approximation rather than a claim of transistor-level
			 * timing. Each byte still costs one read plus one write bus cycle. */
			X6502_AddCycles(1 + (timestamp & 1));
			for (x = 0; x < length; x++)
				X6502_DMW((cfg & 1) ? 0x2007 : 0x2004, X6502_DMR(t + x));

		}
		return;
	case 0x15: /* Bit 4 controls DWS/PCM; bits 0-3 remain XOP1/APU enables. */
		apu40xx[reg] = V;
		if (V & 0x10)
			UNLOneBusPCMStart();
		else
			UNLOneBusPCMStop();
		defapuwrite[reg](A, V & 0xEF);
		return;
	case 0x24: /* VT369 new-DMA source low byte. */
		apu40xx[reg] = V;
		return;
	case 0x30: /* DP, DA routing and DMA address A15/A14 complements. */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		if ((oldValue ^ V) & 0x10) {
			pcm_dws_shift = 0;
			pcm_dws_bits_left = 0;
			if (!(V & 0x10) && pcm_enable) {
				pcm_dws_amplitude = pcm_dws_initial;
				pcm_output = UNLOneBusDWSOutput(pcm_dws_amplitude);
			}
		}
		return;
	case 0x31: /* CPU-written 8-bit PCM DAC (only routed in PCM mode). */
		UNLOneBusAudioFlush();
		apu40xx[reg] = V;
		if (apu40xx[0x30] & 0x10)
			pcm_output = V;
		return;
	default:
		apu40xx[reg] = V;
		break;
	}
	defapuwrite[reg](A, V);
}

static DECLFR(UNLOneBusReadAPU40XX) {
	const uint8 reg = A & 0x3F;
	uint8 result;

	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32 && reg == 0x14)
		return (vt32_pcm_playing[0] ? 0x01 : 0) |
		       (vt32_pcm_playing[1] ? 0x02 : 0);

	result = defapuread[reg](A);
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32) {
		if (reg == 0x17 && cpu410x[0x0B] == 0x14)
			result = result & ~0x04 | (dipswitch & 1 ? 0x04 : 0x00);
		return result;
	}

	switch (reg) {
	case 0x15:
		result &= 0x4F;
		result = (result & 0xEF) | (pcm_enable ? 0x10 : 0) | pcm_irq;
		break;
	case 0x17:
		if (cpu410x[0x0B] == 0x14)
			result = result & ~0x04 | (dipswitch & 1 ? 0x04 : 0x00);
		break;
	}
	return result;
}

static DECLFR(readDIP_FamilyPocket) {
	return dipswitch &1? 8: 0;
}

/* VT369 CPU-visible resources.  Stage 1 adds the newer PPU register shadow,
 * direct palette window and RGB555/protection support. Stage 2 adds packed
 * 4/8bpp rendering and enhanced OAM; GPIO, exact high-speed DMA timing and
 * Stage 2.3 adds a high-level model of the common secondary sound-CPU programs;
 * full cycle-exact Sound CPU LLE remains a later stage. */
static DECLFR(VT369ReadExtraRAM) {
	return VT369ExtraRAM[A & 0x7FF];
}
static DECLFW(VT369WriteExtraRAM) {
	VT369ExtraRAM[A & 0x7FF] = V;
}
static DECLFR(VT369ReadInternalROM) {
	if (MiscROM && MiscROM_size >= 0x1000) return MiscROM[A & 0x0FFF];
	return 0xFF;
}
static DECLFW(VT369IgnoreInternalROMWrite) {
	(void)A; (void)V;
}
static DECLFR(VT369ReadNametableDirect) {
	const uint32 p = (A - 0x3000) & 0x0FFF;
	return vnapage[(p >> 10) & 3][p & 0x3FF];
}
static DECLFW(VT369WriteNametableDirect) {
	const uint32 p = (A - 0x3000) & 0x0FFF;
	FCEUPPU_LineUpdate();
	vnapage[(p >> 10) & 3][p & 0x3FF] = V;
}
static DECLFR(VT369ReadSoundRAM) {
	return VT369SoundRAM[A & 0x7FF];
}
static DECLFW(VT369WriteSoundRAM) {
	const uint32 off = A & 0x7FF;
	if (VT369SoundHLEActive) UNLOneBusAudioFlush();
	VT369SoundRAM[off] = V;
	VT369SoundHLEWrite(off, V);
}

static DECLFR(VT369Read411F) { (void)A; return cpu410x[0x1F]; }
static DECLFR(VT369ReadGPIO414x) { return VT369GPIO414x[A & 0x0F]; }
static DECLFW(VT369WriteGPIO414x) { VT369GPIO414x[A & 0x0F] = V; }
static DECLFR(VT369ReadGPIO415x) { return VT369GPIO415x[A & 0x0F]; }
static DECLFW(VT369WriteGPIO415x) { VT369GPIO415x[A & 0x0F] = V; }
static DECLFR(VT369ReadGPIO41Ex) { return VT369GPIO41Ex[A & 0x0F]; }
static DECLFW(VT369WriteGPIO41Ex) { VT369GPIO41Ex[A & 0x0F] = V; }

static DECLFR(VT369ReadSoundControl4162) { (void)A; return VT369SoundControl4162; }
static DECLFW(VT369WriteSoundControl4162) {
	UNLOneBusAudioFlush();
	VT369SoundControl4162 = V;
	if (V == 0x0D) {
		VT369SoundHLEActive = 1;
		VT369SoundHLECycles = 0;
		VT369SoundPhase6 = 0;
		VT369SoundIRQAcc = 0;
		VT369SoundPeriodN = 0;
		VT369SoundSample = 0;
		VT369SoundStartPrev = 0;
		memset(VT369Sound3Left, 0, sizeof(VT369Sound3Left));
		memset(VT369Sound3Frame, 0, sizeof(VT369Sound3Frame));
		VT369SoundRAM[0x7F5] = 0x01;
		if (!VT369SoundProgramKnown()) VT369SoundRAM[0x04D] = VT369SoundRAM[0x04C];
	} else {
		VT369SoundHLEActive = 0;
		VT369SoundHLECycles = VT369SoundPhase6 = 0;
		VT369SoundSample = 0;
	}
	OneBusUpdateCpuHook();
}

static void VT369RunSoundBootstrapHLE(uint32 cycles) {
	if (!OneBusIsVT369 || !VT369SoundHLEActive || !cycles) return;
	const uint16 period = VT369SoundCurrentPeriod();
	if (!period) {
		// Unknown sound CPU program: retain the Stage 2.2.1 boot handshake so
		// unsupported sets still reach the menu instead of deadlocking.
		VT369SoundHLECycles += cycles;
		while (VT369SoundHLECycles >= 256U) {
			VT369SoundHLECycles -= 256U; VT369SoundRAM[0x0F6]++;
		}
		return;
	}
	VT369SoundPhase6 += cycles * 6U;
	while (VT369SoundPhase6 >= period) {
		VT369SoundPhase6 -= period;
		UNLOneBusAudioFlush();
		VT369SoundDoTick();
	}
}
static DECLFR(VT369ReadRelative) {
	return VT369Relative[A & 1];
}
static DECLFW(VT369WriteRelative) {
	const uint8 index = A & 1;
	if (VT369Relative[index] != V) FCEUPPU_LineUpdate();
	VT369Relative[index] = V;
	Sync();
}
static DECLFW(VT369Write6000) {
	/* $411C.6 selects ROM at $6000-$7FFF; writes are ignored in ROM mode. */
	if (!(cpu410x[0x1C] & 0x40)) CartBW(A, V);
}

/* Stage 1 protection register.  Mapper-256 submappers 13/15 use bit 0:
 * 0=opcode scrambling enabled (power default), 1=disabled. */
static DECLFR(UNLOneBusReadProtection4169) { return VT369Protection4169; }
static DECLFW(UNLOneBusWriteProtection4169) {
	VT369Protection4169 = V;
	SyncEncryption();
}

/* VT369 adds a new PPU register block. Stage 1 provides correct CPU-visible
 * storage/readback and scanline-safe writes; renderer semantics are added in
 * the enhanced-renderer stage rather than guessed here. */
static DECLFR(VT369ReadPPUExt) { return VT369PPURegs[A - 0x2020]; }
static DECLFW(VT369WritePPUExt) {
	const uint32 off = A - 0x2020;
	if (VT369PPURegs[off] != V) FCEUPPU_LineUpdate();
	VT369PPURegs[off] = V;
}

/* CPU $5000-$53FF mirrors the VT369 PPU $3C00-$3FFF palette/CGRAM window.
 * Store the entire 1 KiB address space now; the 8bpp renderer consumes it in
 * Stage 2. The full 1 KiB forms 512 little-endian BGR555 entries:
 * 256 background colours followed by 256 sprite colours. */
static DECLFR(VT369ReadDirectPalette) {
	const uint32 off = (A - 0x5000) & 0x3FF;
	if (ppu201x[0x0E]) return VT369PaletteRAM[off];
	/* Outside VT369 extended mode this window remains compatible with the
	 * underlying PPU map: $5000-$52FF -> $3C00-$3EFF nametable mirror and
	 * $5300-$53FF -> ordinary $3F00-$3FFF palette space. */
	if (off < 0x300) return FFCEUX_PPURead_Default(0x3C00U + off);
	return FFCEUX_PPURead_Default(0x3F00U + (off - 0x300));
}
static void VT369WritePaletteByte(uint32 off, uint8 V) {
	off &= 0x3FF;
	if (VT369PaletteRAM[off] == V) return;
	FCEUPPU_LineUpdate();
	VT369PaletteRAM[off] = V;
	OneBusUpdateVT369DirectPaletteEntry((uint16)(off >> 1));
	if (OneBusIsVT369 && ppu201x[0x0E]) {
		/* The PPU snapshots RGB555 per pixel, so a direct-palette write must not
		 * dirty or modify the ordinary FCEUX output palette. */
		FCEU_DirectPalette512Enabled = true;
	}
}
static DECLFW(VT369WriteDirectPalette) {
	const uint32 off = (A - 0x5000) & 0x3FF;
	if (ppu201x[0x0E]) {
		VT369WritePaletteByte(off, V);
		return;
	}
	if (off < 0x300) FFCEUX_PPUWrite_Default(0x3C00U + off, V);
	else FFCEUX_PPUWrite_Default(0x3F00U + (off - 0x300), V);
}

/* Small VT369 status blocks are polled during boot/menu entry by a number of
 * commercial sets.  They are modeled only on VT369 so older OneBus boards keep
 * their original open-bus behavior. */
static DECLFR(VT369Read415C) { (void)A; return 0xF0; }
static DECLFR(VT369Read4199) { (void)A; return 0x02; }
static DECLFR(VT369Read41B7) { (void)A; return 0x04; }
static DECLFR(VT369Read4326) { (void)A; return 0x01; }

/* $4201 is the dedicated 512-byte OAM upload used by Cube Tech high-resolution
 * menus.  It writes the full private VT369 OAM image and deliberately avoids
 * $2004's auto-toggle side effect. */
static DECLFW(VT369Write4201) {
	const uint16 src = (uint16)V << 8;
	FCEUPPU_LineUpdate();
	for (uint32 i = 0; i < 0x200; ++i) VT369OAM[i] = X6502_DMR((src + i) & 0xFFFF);
}

static void OneBusInstallProtectionHandlers(void) {
	if (submapper == 13 || submapper == 15) {
		SetReadHandler(0x4169, 0x4169, UNLOneBusReadProtection4169);
		SetWriteHandler(0x4169, 0x4169, UNLOneBusWriteProtection4169);
	}
}

static void VT369InstallHandlers(void) {
	if (!OneBusIsVT369) return;
	if (GetWriteHandler(0x2008) != VT369Write2008 && !VT369OldWrite2008)
		VT369OldWrite2008 = GetWriteHandler(0x2008);
	if (GetReadHandler(0x2008) != VT369Read2008 && !VT369OldRead2008)
		VT369OldRead2008 = GetReadHandler(0x2008);
	SetWriteHandler(0x2008, 0x2008, VT369Write2008);
	SetReadHandler(0x2008, 0x2008, VT369Read2008);
	SetReadHandler(0x0800, 0x0FFF, VT369ReadExtraRAM);
	SetWriteHandler(0x0800, 0x0FFF, VT369WriteExtraRAM);
	if (MiscROM && MiscROM_size >= 0x1000) {
		SetReadHandler(0x1000, 0x1FFF, VT369ReadInternalROM);
		SetWriteHandler(0x1000, 0x1FFF, VT369IgnoreInternalROMWrite);
	}
	SetReadHandler(0x3000, 0x3FFF, VT369ReadNametableDirect);
	SetWriteHandler(0x3000, 0x3FFF, VT369WriteNametableDirect);
	SetReadHandler(0x4800, 0x4FFF, VT369ReadSoundRAM);
	SetWriteHandler(0x4800, 0x4FFF, VT369WriteSoundRAM);
	SetReadHandler(0x2020, 0x207F, VT369ReadPPUExt);
	SetWriteHandler(0x2020, 0x207F, VT369WritePPUExt);
	/* VT369 system/GPIO latch readback used by commercial boot code. */
	SetReadHandler(0x411F, 0x411F, VT369Read411F);
	SetReadHandler(0x4144, 0x414F, VT369ReadGPIO414x);
	SetWriteHandler(0x4144, 0x414F, VT369WriteGPIO414x);
	SetReadHandler(0x4150, 0x415F, VT369ReadGPIO415x);
	SetWriteHandler(0x4150, 0x415F, VT369WriteGPIO415x);
	SetReadHandler(0x41E4, 0x41EB, VT369ReadGPIO41Ex);
	SetWriteHandler(0x41E4, 0x41EB, VT369WriteGPIO41Ex);
	/* $415C is a VT369 status register, not a plain latch. */
	SetReadHandler(0x415C, 0x415C, VT369Read415C);
	SetReadHandler(0x4160, 0x4161, VT369ReadRelative);
	SetWriteHandler(0x4160, 0x4161, VT369WriteRelative);
	SetReadHandler(0x4162, 0x4162, VT369ReadSoundControl4162);
	SetWriteHandler(0x4162, 0x4162, VT369WriteSoundControl4162);
	SetReadHandler(0x4199, 0x4199, VT369Read4199);
	SetReadHandler(0x41B7, 0x41B7, VT369Read41B7);
	SetWriteHandler(0x4201, 0x4201, VT369Write4201);
	SetReadHandler(0x4326, 0x4326, VT369Read4326);
	SetReadHandler(0x5000, 0x53FF, VT369ReadDirectPalette);
	SetWriteHandler(0x5000, 0x53FF, VT369WriteDirectPalette);
	SetWriteHandler(0x6000, 0x7FFF, VT369Write6000);
}

static inline int OneBusCpuHookNeeded(void) {
	if (OneBusHasALU && vt32_alu_busy) return 1;
	if (OneBusIsVT369 && VT369SoundHLEActive) return 1;
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32)
		return vt32_pcm_playing[0] || vt32_pcm_playing[1];
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT03)
		return pcm_enable != 0;
	return 0;
}

static void OneBusUpdateCpuHook(void) {
	if (OneBusCpuHookNeeded()) {
		if (MapIRQHook != UNLOneBusCpuHook) MapIRQHook = UNLOneBusCpuHook;
	}
	else if (MapIRQHook == UNLOneBusCpuHook) {
		MapIRQHook = NULL;
	}
}

static void UNLOneBusCpuHook(int a) {
	uint32 cyclesLeft;
	uint32 rate;
	uint32 cpuClock;
	if (a <= 0)
		return;
	if (OneBusHasALU && vt32_alu_busy)
		vt32_alu_busy = ((uint32)a >= vt32_alu_busy) ? 0 : (uint8)(vt32_alu_busy - a);
	if (OneBusIsVT369) {
		uint32 baseCycles = (uint32)a;
		const uint8 mul = X6502_GetOneBusClockMultiplier();
		if (mul == 3) {
			const uint32 total = (uint32)VT369PeripheralCycleRemainder + baseCycles;
			baseCycles = total / 3;
			VT369PeripheralCycleRemainder = (uint8)(total % 3);
		}
		VT369RunSoundBootstrapHLE(baseCycles);
	}
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT369) {
		OneBusUpdateCpuHook();
		return;
	}
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32) {
		UNLOneBusVT32PCMRunCycles((uint32)a);
		OneBusUpdateCpuHook();
		return;
	}
	if (!pcm_enable) {
		OneBusUpdateCpuHook();
		return;
	}
	rate = UNLOneBusAudioSampleRate();
	cpuClock = UNLOneBusAudioCPUClock();
	cyclesLeft = (uint32)a;
	while (cyclesLeft) {
		const uint64 need = (uint64)cpuClock - pcm_phase;
		const uint32 cyclesToEvent = (uint32)((need + rate - 1) / rate);
		if (cyclesToEvent > cyclesLeft) {
			pcm_phase += (uint64)cyclesLeft * rate;
			break;
		}
		cyclesLeft -= cyclesToEvent;
		pcm_phase += (uint64)cyclesToEvent * rate - cpuClock;
		UNLOneBusAudioFlushBackdated(cyclesLeft);
		UNLOneBusPCMTick();
		if (!pcm_enable)
			break;
	}
	OneBusUpdateCpuHook();
}

static void UNLOneBusPower(void) {
	uint32 i;
	X6502_SetOneBusClockMultiplier(1);
	// Board-level debugger guard: only an active OneBus/VTxx board may ask
	// the generic debugger to decode encrypted opcodes.
	X6502_SetOpcodeDebugDecryptionEnabled(1);
	OneBusRestoreOutputPalette();
	IRQReload = IRQCount = IRQa = IRQPending = 0;
	X6502_IRQEnd(ONEBUS_TIMER_IRQ | ONEBUS_PCM_IRQ);
	UNLOneBusResetPCM();
	reg4242 = 0;

	memset(cpu410x, 0x00, sizeof(cpu410x));
	memset(ppu201x, 0x00, sizeof(ppu201x));
	memset(OneBusCGRAM, 0x00, sizeof(OneBusCGRAM));
	memset(apu40xx, 0x00, sizeof(apu40xx));
	if (OneBusIsVT369) {
		memset(VT369ExtraRAM, 0x00, sizeof(VT369ExtraRAM));
		memset(VT369SoundRAM, 0x00, sizeof(VT369SoundRAM));
		memset(VT369Relative, 0x00, sizeof(VT369Relative));
		memset(VT369PPURegs, 0x00, sizeof(VT369PPURegs));
		memset(VT369PaletteRAM, 0x00, sizeof(VT369PaletteRAM));
		memset(VT369OAM, 0x00, sizeof(VT369OAM));
		memset(VT369GPIO414x, 0x00, sizeof(VT369GPIO414x));
		memset(VT369GPIO415x, 0x00, sizeof(VT369GPIO415x));
		memset(VT369GPIO41Ex, 0x00, sizeof(VT369GPIO41Ex));
		VT369OAMHigh = 0;
		VT369SoundControl4162 = 0;
		VT369SoundHLEActive = 0;
		VT369SoundHLECycles = VT369SoundPhase6 = VT369SoundIRQAcc = 0;
		VT369SoundPeriodN = 0; VT369SoundSample = 0; VT369SoundStartPrev = 0;
		memset(VT369Sound3Left, 0, sizeof(VT369Sound3Left));
		memset(VT369Sound3Frame, 0, sizeof(VT369Sound3Frame));
		VT369PeripheralCycleRemainder = 0;
	}
	VT369Protection4169 = 0;
	cpu410x[0x0F] = 0xFF;
	if (submapper == 10 || submapper == 11) cpu410x[0x1C] = 0x02;
	if (submapper == 12 || submapper == 14) cpu410x[0x1C] = 0x40;

	/* OneBus uses PRG-ROM as the CHR source when the cartridge has no
	 * CHR-ROM.  Do not test VROM: in FCEUX that pointer can refer to the
	 * writable CHR-RAM buffer allocated from the NES 2.0 header.  Conversely,
	 * CHRRomSize preserves small (1/2/4 KiB) real CHR-ROM images that would be
	 * lost by testing the 8 KiB VROM_size bank count. */
	if (!OneBusHasCHRROM)
		SetupCartCHRMapping(0, PRGptr[0], PRGsize[0], 0);
	OneBusUpdateCHRFastWrap();
	SetupCartCHRMapping(ONEBUS_IVR_CHIP, NTARAM, sizeof(NTARAM), 1);
	UNLOneBusInstallPPUBus();
	OneBusUpdateAllPalette();

	for (i = 0; i < 64; i++) {
		defapuread[i] = GetReadHandler(0x4000 | i);
		defapuwrite[i] = GetWriteHandler(0x4000 | i);
	}
	SetReadHandler(0x4000, 0x403f, UNLOneBusReadAPU40XX);
	SetWriteHandler(0x4000, 0x403f, UNLOneBusWriteAPU40XX);

	SetReadHandler(0x412C, 0x412C, readDIP_FamilyPocket);
	SetReadHandler(0x6000, 0xFFFF, CartBR);
	SetWriteHandler(0x6000, 0x7FFF, CartBW);
	SetWriteHandler(0x2010, 0x201f, UNLOneBusWritePPU201X);
	SetWriteHandler(0x4100, 0x413f, UNLOneBusWriteCPU410X);
	OneBusInstallProtectionHandlers();
	if (OneBusHasALU)
		SetReadHandler(0x4130, 0x413D, UNLOneBusReadVT32ALU);
	SetWriteHandler(0x4242, 0x4242, UNLOneBusWriteCPU4242);
	SetWriteHandler(0x8000, 0xffff, UNLOneBusWriteMMC3);

	FCEU_CheatAddRAM(8, 0x6000, WRAM);
	setprg8r(0x10, 0x6000, 0);
	VT369InstallHandlers();
	dipswitch =0;
	OneBusUpdateCpuHook();
	Sync();
}

static void UNLOneBusReset(void) {
	X6502_SetOpcodeDebugDecryptionEnabled(1);
	X6502_SetOneBusClockMultiplier(1);
	OneBusRestoreOutputPalette();
	/* Defensive reinstall: cartridge reset normally preserves PPU hooks, but
	 * keeping the CPU PPUDATA bus override attached makes reset/load ordering
	 * robust against frontends that rebuild PPU hook state. */
	UNLOneBusInstallPPUBus();
	IRQReload = IRQCount = IRQa = IRQPending = 0;
	X6502_IRQEnd(ONEBUS_TIMER_IRQ | ONEBUS_PCM_IRQ);
	UNLOneBusResetPCM();

	memset(cpu410x, 0x00, sizeof(cpu410x));
	memset(ppu201x, 0x00, sizeof(ppu201x));
	memset(OneBusCGRAM, 0x00, sizeof(OneBusCGRAM));
	memset(apu40xx, 0x00, sizeof(apu40xx));
	if (OneBusIsVT369) {
		memset(VT369Relative, 0x00, sizeof(VT369Relative));
		memset(VT369PPURegs, 0x00, sizeof(VT369PPURegs));
		memset(VT369PaletteRAM, 0x00, sizeof(VT369PaletteRAM));
		memset(VT369OAM, 0x00, sizeof(VT369OAM));
		memset(VT369GPIO414x, 0x00, sizeof(VT369GPIO414x));
		memset(VT369GPIO415x, 0x00, sizeof(VT369GPIO415x));
		memset(VT369GPIO41Ex, 0x00, sizeof(VT369GPIO41Ex));
		VT369OAMHigh = 0;
		VT369SoundControl4162 = 0;
		VT369SoundHLEActive = 0;
		VT369SoundHLECycles = VT369SoundPhase6 = VT369SoundIRQAcc = 0;
		VT369SoundPeriodN = 0; VT369SoundSample = 0; VT369SoundStartPrev = 0;
		memset(VT369Sound3Left, 0, sizeof(VT369Sound3Left));
		memset(VT369Sound3Frame, 0, sizeof(VT369Sound3Frame));
		VT369PeripheralCycleRemainder = 0;
	}
	VT369Protection4169 = 0;
	cpu410x[0x0F] = 0xFF;
	if (submapper == 10 || submapper == 11) cpu410x[0x1C] = 0x02;
	if (submapper == 12 || submapper == 14) cpu410x[0x1C] = 0x40;
	reg4242 =0;
	dipswitch++;
	OneBusInstallProtectionHandlers();
	VT369InstallHandlers();
	OneBusUpdateCHRFastWrap();
	OneBusUpdateCpuHook();

	Sync();
	OneBusUpdateAllPalette();
}

static void StateRestore(int version) {
	UNLOneBusInstallPPUBus();
	X6502_SetOneBusClockMultiplier((OneBusIsVT369 && (cpu410x[0x1C] & 0x80)) ? 3 : 1);
	OneBusInstallProtectionHandlers();
	VT369InstallHandlers();
	if (IRQPending && IRQa)
		X6502_IRQBegin(ONEBUS_TIMER_IRQ);
	else
		X6502_IRQEnd(ONEBUS_TIMER_IRQ);
	if (OneBusAudioGeneration == ONEBUS_AUDIO_VT32) {
		/* VT32 PCM has no completion IRQ; do not resurrect the VT03 IRQ line
		 * when loading a state created on a different VT generation. */
		pcm_irq = 0;
		X6502_IRQEnd(ONEBUS_PCM_IRQ);
		if (!vt32_pcm_period[0]) vt32_pcm_period[0] = 0x6F;
		if (!vt32_pcm_period[1]) vt32_pcm_period[1] = vt32_pcm_period[0];
		vt32_pcm_programmed[1] = vt32_pcm_programmed[0];
	} else if (OneBusAudioGeneration == ONEBUS_AUDIO_VT369) {
		pcm_irq = 0;
		X6502_IRQEnd(ONEBUS_PCM_IRQ);
	} else {
		if (pcm_irq)
			X6502_IRQBegin(ONEBUS_PCM_IRQ);
		else
			X6502_IRQEnd(ONEBUS_PCM_IRQ);
		UNLOneBusPCMUpdateRate();
	}
	UNLOneBusAudioRateChanged();
	OneBusUpdateCHRFastWrap();
	OneBusUpdateCpuHook();
	Sync();
	if (OneBusIsVT369 && ppu201x[0x0E])
		OneBusUpdateVT369DirectPalette();
	else if (UNLOneBusGetExtMode() & 0x80)
		OneBusUpdateAllPalette();
	else
		OneBusRestoreOutputPalette();
}

static void UNLOneBus_Close(void) {
	X6502_SetOneBusClockMultiplier(1);
	// Disable debugger-only opcode decoding before handing control back to a
	// different board/game. CPU-global encryption state is also cleared by
	// ResetGameLoaded()/PowerNES().
	X6502_SetOpcodeDebugDecryptionEnabled(0);
	OneBusRestoreOutputPalette();
	X6502_IRQEnd(ONEBUS_TIMER_IRQ | ONEBUS_PCM_IRQ);
	UNLOneBusResetPCM();
	if (FFCEUX_PPUReadCPU == UNLOneBusPPUReadCPU)
		FFCEUX_PPUReadCPU = OneBusOldPPUReadCPU;
	if (FFCEUX_PPUWriteCPU == UNLOneBusPPUWriteCPU)
		FFCEUX_PPUWriteCPU = OneBusOldPPUWriteCPU;
	if (FFCEUX_PPUReadPatternExt == UNLOneBusPPUReadPatternExt)
		FFCEUX_PPUReadPatternExt = NULL;
	if (FFCEUX_PPUGetPackedMode == UNLOneBusGetPackedMode)
		FFCEUX_PPUGetPackedMode = NULL;
	if (FFCEUX_PPUReadPackedBG == UNLOneBusPPUReadPackedBG)
		FFCEUX_PPUReadPackedBG = NULL;
	if (FFCEUX_PPUReadPackedSprite == UNLOneBusPPUReadPackedSprite)
		FFCEUX_PPUReadPackedSprite = NULL;
	if (FFCEUX_PPUGetExtMode == UNLOneBusGetExtMode)
		FFCEUX_PPUGetExtMode = NULL;
	if (FFCEUX_PPUMapExtPalette == UNLOneBusMapExtPalette)
		FFCEUX_PPUMapExtPalette = NULL;
	if (FFCEUX_PPUUseFixedOSD == UNLOneBusUseFixedOSD)
		FFCEUX_PPUUseFixedOSD = NULL;
	if (FFCEUX_PPURefreshExtPalette == UNLOneBusRefreshExtPalette)
		FFCEUX_PPURefreshExtPalette = NULL;
	if (FFCEUX_PPUGetVT369Mode == UNLOneBusGetVT369Mode) FFCEUX_PPUGetVT369Mode = NULL;
	if (FFCEUX_PPUReadVT369Reg == UNLOneBusReadVT369Reg) FFCEUX_PPUReadVT369Reg = NULL;
	if (FFCEUX_PPUReadVT369BG == UNLOneBusPPUReadVT369BG) FFCEUX_PPUReadVT369BG = NULL;
	if (FFCEUX_PPUReadVT369BG16 == UNLOneBusPPUReadVT369BG16) FFCEUX_PPUReadVT369BG16 = NULL;
	if (FFCEUX_PPUReadVT369Sprite == UNLOneBusPPUReadVT369Sprite) FFCEUX_PPUReadVT369Sprite = NULL;
	if (FFCEUX_PPUGetVT369SpriteConfig == UNLOneBusGetVT369SpriteConfig) FFCEUX_PPUGetVT369SpriteConfig = NULL;
	if (FFCEUX_PPUReadVT369OAM == UNLOneBusPPUReadVT369OAM) FFCEUX_PPUReadVT369OAM = NULL;
	if (FFCEUX_PPUGetVT369OAMHigh == UNLOneBusPPUGetVT369OAMHigh) FFCEUX_PPUGetVT369OAMHigh = NULL;
	if (FFCEUX_PPUWriteVT369OAM == UNLOneBusPPUWriteVT369OAM) FFCEUX_PPUWriteVT369OAM = NULL;
	if (GetWriteHandler(0x2008) == VT369Write2008 && VT369OldWrite2008)
		SetWriteHandler(0x2008, 0x2008, VT369OldWrite2008);
	if (GetReadHandler(0x2008) == VT369Read2008 && VT369OldRead2008)
		SetReadHandler(0x2008, 0x2008, VT369OldRead2008);
	VT369OldWrite2008 = NULL;
	VT369OldRead2008 = NULL;
	OneBusOldPPUReadCPU = NULL;
	OneBusOldPPUWriteCPU = NULL;

	if (WRAM)
		FCEU_gfree(WRAM);
	WRAM = NULL;
	/* Mapper270_Init / Mapper436_Init allocate an additional CHRRAM after
	 * UNLOneBus_Init returns; free that here too to avoid leaking it on
	 * every load/unload cycle. */
	if (CHRRAM)
		FCEU_gfree(CHRRAM);
	CHRRAM = NULL;
	CHRRAMSIZE = 0;
	securityROM_data = NULL;
	OneBusHasCHRROM = 0;
	if (MapIRQHook == UNLOneBusCpuHook) MapIRQHook = NULL;
	OneBusCHRWrapMask = OneBusCHRSplitWrapMask = 0;
	OneBusCHRWrapPow2 = OneBusCHRSplitWrapPow2 = 0;
	OneBusIsMapper296 = 0;
	OneBusIsVT369 = 0;
	OneBusHasALU = 0;
	memset(VT369Relative, 0x00, sizeof(VT369Relative));
	memset(VT369PPURegs, 0x00, sizeof(VT369PPURegs));
	memset(VT369PaletteRAM, 0x00, sizeof(VT369PaletteRAM));
	memset(VT369OAM, 0x00, sizeof(VT369OAM));
	VT369OAMHigh = 0;
	VT369Protection4169 = 0;
	OneBusPaletteFormat = ONEBUS_PAL_VT03;
	OneBusAudioGeneration = ONEBUS_AUDIO_VT03;
}

void UNLOneBus_Init(CartInfo *info) {
	OneBusHasCHRROM = (info->CHRRomSize > 0) ? 1 : 0;
	OneBusCHRAnd = 0x7FFF;
	OneBusCHROr = 0;
	OneBusUseMappedCHR = 0;
	OneBusOldPPUReadCPU = NULL;
	OneBusOldPPUWriteCPU = NULL;
	info->Power = UNLOneBusPower;
	info->Reset = UNLOneBusReset;
	info->Close = UNLOneBus_Close;
	Sync = Sync256;

	if (info->ines2)
		submapper =info->submapper;
	else {
		/* Compare the first four MD5 bytes literally to avoid the previous
		 * endian-dependent (uint32) reinterpretation, which silently
		 * mismatched on big-endian hosts and disabled the PowerJoy Supermax
		 * variant detection. */
		const uint8 *md5 = info->MD5;
		int is_305fcdc3 = (md5[0] == 0xc3 && md5[1] == 0xcd && md5[2] == 0x5f && md5[3] == 0x30);
		int is_6abfce8e = (md5[0] == 0x8e && md5[1] == 0xce && md5[2] == 0xbf && md5[3] == 0x6a);
		submapper = (is_305fcdc3 || is_6abfce8e) ? 2 : 0; /* PowerJoy Supermax Carts */
	}

	/* NES 2.0 extended console type is the authoritative VT generation.
	 * Header-editor values: 6=VT02, 7=VT03, 8=VT09, 9=VT32, 10=VT369.
	 * Fall back to established mapper-256 submapper knowledge when absent. */
	OneBusHas4BPP = 1;
	OneBusHasEnhancedPalette = 1;
	OneBusHas16BitVideoBus = 0;
	OneBusPaletteFormat = ONEBUS_PAL_VT03;
	OneBusAudioGeneration = ONEBUS_AUDIO_VT03;
	OneBusIsMapper296 = (info->mapper == 296) ? 1 : 0;
	OneBusIsVT369 = (info->extendedConsoleType == 10 || info->mapper == 425) ? 1 : 0;
	OneBusHasALU = (info->extendedConsoleType == 9 || OneBusIsMapper296 || OneBusIsVT369) ? 1 : 0;
	if (info->extendedConsoleType == 6 || submapper == 5) {
		OneBusHas4BPP = 0;
		OneBusHasEnhancedPalette = 0;
	}
	else if (info->extendedConsoleType >= 8 && info->extendedConsoleType <= 10) {
		OneBusHas16BitVideoBus = 1;
	}
	/* VT31/VT32 replaced VT03 HSL-like enhanced colors with RGB444.
	 * Mapper 296 is a VT3x board, so use this even with an incomplete header. */
	if (info->extendedConsoleType == 9 || OneBusIsMapper296) {
		OneBusHas4BPP = 1;
		OneBusHasEnhancedPalette = 1;
		OneBusHas16BitVideoBus = 1;
		OneBusPaletteFormat = ONEBUS_PAL_RGB444;
		OneBusAudioGeneration = ONEBUS_AUDIO_VT32;
	}
	if (OneBusIsVT369) {
		OneBusHas4BPP = 1;
		OneBusHasEnhancedPalette = 1;
		OneBusHas16BitVideoBus = 1;
		/* VT369 keeps the VT03-compatible path but its enhanced palette is
		 * BGR555. Stage 2.3 routes the independent VT369 sound domain through its
		 * dedicated Sound-CPU HLE instead of the VT03/VT32 PCM engines. */
		OneBusPaletteFormat = ONEBUS_PAL_RGB555;
		OneBusAudioGeneration = ONEBUS_AUDIO_VT369;
	}

	cpuMangle =cpuMangles[submapper];
	ppuMangle =ppuMangles[submapper];
	mmc3Mangle =mmc3Mangles[submapper];

	GameHBIRQHook = UNLOneBusIRQHook;
	/* CPU-cycle work is armed dynamically only while PCM/ALU/Sound HLE needs it. */
	if (MapIRQHook == UNLOneBusCpuHook) MapIRQHook = NULL;
	GameStateRestore = StateRestore;
	UNLOneBusInstallAudio();
	AddExState(&StateRegs, ~0, 0, 0);
	
	WRAM = (uint8*)FCEU_gmalloc(8192);
	SetupCartPRGMapping(0x10, WRAM, 8192, 1);
}

void Mapper425_Init(CartInfo *info) {
	/* Cube Tech VT369 boards use the common OneBus register/banking core.
	 * UNLOneBus_Init detects mapper 425 and enables only the VT369 Stage 0
	 * hardware capabilities implemented above. */
	UNLOneBus_Init(info);
}

static void Sync270(void) {
	int OR =0;
	switch(submapper) {
		case 1:  OR = cpu410x[0x2C] &0x02? 0x0800: 0x0000;
		         break;
		case 2:  OR =(cpu410x[0x2C] &0x02? 0x0800: 0x0000) |
		             (cpu410x[0x2C] &0x01? 0x1000: 0x0000);
			 break;
		case 3:  OR = cpu410x[0x2C] &0x04? 0x0800: 0x0000;
		         break;
		default: OR =(cpu410x[0x2C] &0x06? 0x0800: 0x0000) |
		             (cpu410x[0x2C] &0x01? 0x1000: 0x0000);
			 break;
	}
	PSync(0x07FF, OR);
	if (reg4242 &1) {
		OneBusUseMappedCHR = 1;
		setchr8r(0x10, 0);
		setmirror((mirror ^ 1) & 1);
	} else
		CSync(0x3FFF, OR <<3);
	SyncEncryption();
}

void Mapper270_Init(CartInfo *info) {
	UNLOneBus_Init(info);
	cpuMangle = cpuMangles[0];
	ppuMangle = ppuMangles[0];
	mmc3Mangle = mmc3Mangles[0];
	
	CHRRAMSIZE = 8192;
	CHRRAM = (uint8*)FCEU_gmalloc(CHRRAMSIZE);
	SetupCartCHRMapping(0x10, CHRRAM, CHRRAMSIZE, 1);
	AddExState(CHRRAM, CHRRAMSIZE, 0, "CHRR");
	
	Sync = Sync270;
}

static uint8 MMC1_shift, MMC1_bits, MMC1_reg[4];
static uint8 securityROM_output;
static uint16 securityROM_state;
static uint8 securityROM_command;
static uint16 securityROM_bitPosition;
static SFORMAT StateRegs_408[] = {
	{ &MMC1_shift, 1, "MM1S" },
	{ &MMC1_bits, 1, "MM1B" },
	{ MMC1_reg, 4, "MM1R" },
	{ &securityROM_state, 2 | FCEUSTATE_RLSB, "SRST" },
	{ &securityROM_bitPosition, 2 | FCEUSTATE_RLSB, "SRBP" },
	{ &securityROM_command, 1, "SRCM" },
	{ &securityROM_output, 1, "SROU" },
	{ 0 }
};

static void PSyncMMC1(int AND, int OR) {
	uint8 bankmode = cpu410x[0xB] &7;
	uint8 mask = bankmode == 0x7? 0xFF: (0x3F >>bankmode);
	uint32 block = cpu410x[0x0] <<4 &0xF00 | cpu410x[0xA] &~mask;
	mask >>= 1;
	block >>= 1;
	AND >>= 1;
	OR >>= 1;
	if (MMC1_reg[0] &0x08) {
		if (MMC1_reg[0] &0x04) {
			setprg16(0x8000, (block | MMC1_reg[3] &mask) &AND | OR);
			setprg16(0xC000, (block | 0xFF        &mask) &AND | OR);
		} else {
			setprg16(0x8000, (block | 0x0         &mask) &AND | OR);
			setprg16(0xC000, (block | MMC1_reg[3] &mask) &AND | OR);
		}
	} else
		setprg32(0x8000, ((block | MMC1_reg[3] &mask) &AND | OR) >>1);
}

static void Sync408(void) {
	int bankSelect = 0;
	if (securityROM_data)
		bankSelect = cpu410x[0x0F] >>5;
	else
	if (submapper == 12 || submapper == 14)
		bankSelect = (cpu410x[0x39] &0x08? 1: 0) | (cpu410x[0x39] &0x04? 2: 0);
	else
		bankSelect = cpu410x[0x39] &0x02? 1: 0;
	
	if (cpu410x[0x0B] &0x20 && ~cpu410x[0x0B] &0x10)
		PSyncMMC1(0x07FF, bankSelect <<11);
	else
		PSync(0x07FF, bankSelect <<11);
	
	if (cpu410x[0x00] &0x08) {
		OneBusUseMappedCHR = 1;
		setchr8r(0x10, 0);
		setmirror((mirror ^ 1) & 1);
	} else
		CSync(0x3FFF, bankSelect <<14);
	SyncEncryption();
}

static DECLFW(UNLOneBusWriteUNROM) {
	FCEUPPU_LineUpdate();
	cpu410x[0x7] = V <<1 | 0;
	cpu410x[0x8] = V <<1 | 1;
	Sync();
}

static void OneBusApplyMMC1CHRRegs(void) {
	if (MMC1_reg[0] &0x10) {
		ppu201x[0x6] = MMC1_reg[1] <<2 |0;
		ppu201x[0x7] = MMC1_reg[1] <<2 |2;
		ppu201x[0x2] = MMC1_reg[2] <<2 |0;
		ppu201x[0x3] = MMC1_reg[2] <<2 |1;
		ppu201x[0x4] = MMC1_reg[2] <<2 |2;
		ppu201x[0x5] = MMC1_reg[2] <<2 |3;
	} else {
		ppu201x[0x6] = MMC1_reg[1] <<2 &~7 |0;
		ppu201x[0x7] = MMC1_reg[1] <<2 &~7 |2;
		ppu201x[0x2] = MMC1_reg[1] <<2 &~7 |4;
		ppu201x[0x3] = MMC1_reg[1] <<2 &~7 |5;
		ppu201x[0x4] = MMC1_reg[1] <<2 &~7 |6;
		ppu201x[0x5] = MMC1_reg[1] <<2 &~7 |7;
	}
}

static DECLFW(UNLOneBusWriteMMC1) {
	FCEUPPU_LineUpdate();
	if (V &0x80) {
		MMC1_reg[0] |= 0x0C;
		MMC1_shift = 0;
		MMC1_bits = 0;
	} else {
		MMC1_shift |= (V &1) << MMC1_bits++;
		if (MMC1_bits == 5) {
			MMC1_reg[A >>13 &3] = MMC1_shift;
			MMC1_shift = 0;
			MMC1_bits =0 ;
		}
	}
	OneBusApplyMMC1CHRRegs();
	Sync();
}

static DECLFW (Mapper408_WriteEmulatedASIC) {
	if (cpu410x[0x0B] &0x20) {
		if (cpu410x[0x0B] &0x10)
			UNLOneBusWriteUNROM(A, V);
		else
			UNLOneBusWriteMMC1(A, V);
	} else
		UNLOneBusWriteMMC3(A, V);
}

static DECLFR (Mapper408_ReadSecurityROM) {
	if (cpu410x[0x38] &0x08)
		return cpu410x[0x39] &~0x01;
	else
		return cpu410x[0x39] &~0x09 | (securityROM_output? 0x08: 0x00);
}

static DECLFW (Mapper408_WriteSecurityROM) {
	if (~V &0x04) { /* Chip Select */
		if (~cpu410x[0x39] &0x02 & V &0x02) { /* Rising clock edge */
			if (securityROM_state < 8) {
				securityROM_command = securityROM_command <<1 | (V &0x08? 1: 0);
				if (++securityROM_state == 8 && securityROM_command != 0x20)
					securityROM_state = 0;
				else
					securityROM_bitPosition = 0;
			} else {
				securityROM_output = securityROM_data[securityROM_bitPosition >>3 &0xFF] >> (7 - (securityROM_bitPosition &7)) &1;
				if (++securityROM_bitPosition >= 2048) securityROM_state = 0;
			}
		}
	} else {
		securityROM_state = 0;
		securityROM_command = 0;
		securityROM_bitPosition = 0;
	}
	cpu410x[0x39] = V;
}

static void Mapper408_Power (void) {
	// Reset the emulated ASIC before UNLOneBusPower performs its first Sync().
	// Otherwise a previous Mapper 408 cartridge's MMC1 bank registers could
	// influence the initial PRG/CHR mapping of the newly loaded cartridge.
	MMC1_shift = 0;
	MMC1_bits = 0;
	memset(MMC1_reg, 0, sizeof(MMC1_reg));
	MMC1_reg[0] = 0x0C;
	securityROM_output = 0;
	securityROM_state = 0;
	securityROM_command = 0;
	securityROM_bitPosition = 0;

	UNLOneBusPower();
	SetWriteHandler(0x8000, 0XFFFF, Mapper408_WriteEmulatedASIC);
	if (securityROM_data) {
		SetReadHandler(0x4139, 0x4139, Mapper408_ReadSecurityROM);
		SetWriteHandler(0x4139, 0x4139, Mapper408_WriteSecurityROM);
	}
}

void Mapper408_Init(CartInfo *info) {
	UNLOneBus_Init(info);
	info->Power = Mapper408_Power;
	cpuMangle = cpuMangles[0];
	ppuMangle = ppuMangles[0];
	mmc3Mangle = mmc3Mangles[0];
	securityROM_data = MiscROM && info->miscROMSize >= 256? MiscROM: NULL;
	
	CHRRAMSIZE = 8192;
	CHRRAM = (uint8*)FCEU_gmalloc(CHRRAMSIZE);
	SetupCartCHRMapping(0x10, CHRRAM, CHRRAMSIZE, 1);
	AddExState(CHRRAM, CHRRAMSIZE, 0, "CHRR");
	AddExState(&StateRegs_408, ~0, 0, 0);
	
	Sync = Sync408;
}


/* Mapper 296 / VT3x ------------------------------------------------------ */
static uint8 Mapper296ExtraRAM[0x800];
static uint8 Mapper296CNROMBank;

static SFORMAT StateRegs_296[] = {
	{ Mapper296ExtraRAM, 0x800, "M2XR" },
	{ &Mapper296CNROMBank, 1, "M2CB" },
	{ &encryptOpcodes, 1, "M2EO" },
	{ &encryptOpcodesPending, 1, "M2EP" },
	{ &encryptOpcodesPendingMode, 1, "M2EM" },
	{ 0 }
};

static uint32 Mapper296PRGOuter(void) {
	return ((cpu410x[0x2C] & 0x01) ? 0x1000U : 0U) |
	       ((cpu410x[0x2C] & 0x04) ? 0x2000U : 0U) |
	       ((cpu410x[0x2E] & 0x01) ? 0x4000U : 0U);
}

static uint32 Mapper296CHROuter(void) {
	return ((cpu410x[0x2C] & 0x02) ? 0x08000U : 0U) |
	       ((cpu410x[0x2C] & 0x08) ? 0x10000U : 0U) |
	       ((cpu410x[0x2E] & 0x01) ? 0x20000U : 0U);
}

static void Mapper296SetCNROMCHR(uint8 bank) {
	const uint8 b = (uint8)(bank << 3);
	ppu201x[0x6] = b | 0;
	ppu201x[0x7] = b | 2;
	ppu201x[0x2] = b | 4;
	ppu201x[0x3] = b | 5;
	ppu201x[0x4] = b | 6;
	ppu201x[0x5] = b | 7;
}

static void Mapper296SyncMMC1Mirror(void) {
	switch (MMC1_reg[0] & 3) {
	case 0: setmirror(MI_0); break;
	case 1: setmirror(MI_1); break;
	case 2: setmirror(MI_V); break;
	default:setmirror(MI_H); break;
	}
}

static void Sync296(void) {
	const uint32 prgOuter = Mapper296PRGOuter();
	const uint32 chrOuter = Mapper296CHROuter();

	/* $411D.1:0 selects the legacy mapper front-end.  The VT bank registers
	 * remain the backing store, which is how the menu can prepare a game's
	 * window before switching the forwarding mode. */
	if ((cpu410x[0x1D] & 3) == 1) {
		/* NRS Mapper296 derives the VT CHR registers from MMC1 on every sync,
		 * including the instant $411D switches into MMC1 mode. */
		OneBusApplyMMC1CHRRegs();
		PSyncMMC1(0x0FFF, (int)prgOuter);
	} else {
		if ((cpu410x[0x1D] & 3) == 3)
			Mapper296SetCNROMCHR(Mapper296CNROMBank);
		PSync(0x0FFF, (int)prgOuter);
	}

	if (cpu410x[0x1D] & 0x04) {
		/* VT3x unbanked 8 KiB CHR-RAM mode.  Legacy games using this path are
		 * normally 2bpp, so the established mapped-CHR renderer path applies. */
		OneBusUseMappedCHR = 1;
		OneBusCHRAnd = 7;
		OneBusCHROr = 0;
		setchr8r(MAPPER296_CHR_CHIP, 0);
		setmirror((mirror ^ 1) & 1);
	} else {
		CSync(0x7FFF, (int)chrOuter);
	}
	/* CSync applies the OneBus HV mirror. Mapper296 MMC1 mode instead uses
	 * MMC1 control bits, so override it after CHR synchronization. */
	if ((cpu410x[0x1D] & 3) == 1)
		Mapper296SyncMMC1Mirror();
}

static DECLFR(Mapper296Read4119) { return 0x00; }
static DECLFR(Mapper296Read412C) { return 0x00; }
static DECLFR(Mapper296Read412D) {
	/* NRS NintendoEmulator defaults the cartridge jumper/DIP field to 3
	 * when no frontend override is present. Several VT32 menus probe it. */
	return 0x03;
}
static DECLFR(Mapper296Read414A) { return 0x80; }

static DECLFR(Mapper296ReadExtraRAM) { return Mapper296ExtraRAM[A & 0x7FF]; }
static DECLFW(Mapper296WriteExtraRAM) { Mapper296ExtraRAM[A & 0x7FF] = V; }

static DECLFW(Mapper296Write411E) {
	cpu410x[0x1E] = V;
	/* VT32 changes opcode scrambling only after the next absolute/indirect
	 * JMP. DGUN2573 uses mode 5 (XOR $A1); NRS NintendoEmulator also
	 * documents mode 7 (XOR $7E), used by other VT32-family software. */
	encryptOpcodesPending = 1;
	switch (V & 0x07) {
	case 0x05: encryptOpcodesPendingMode = 15; break; /* XOR $A1 */
	case 0x07: encryptOpcodesPendingMode = 16; break; /* XOR $7E */
	default:   encryptOpcodesPendingMode = 0;  break;
	}
}

static DECLFW(Mapper296WriteCNROM) {
	Mapper296CNROMBank = V;
	Mapper296SetCNROMCHR(V);
	Sync296();
}

static DECLFW(Mapper296WriteLegacy) {
	switch (cpu410x[0x1D] & 3) {
	case 0: UNLOneBusWriteMMC3(A, V); break;
	case 1: UNLOneBusWriteMMC1(A, V); break;
	case 2:
		/* NRS uses one shared latch for UNROM and CNROM modes. */
		Mapper296CNROMBank = V;
		UNLOneBusWriteUNROM(A, V);
		break;
	default: Mapper296WriteCNROM(A, V); break;
	}
}

static DECLFW(Mapper296Write4A00) {
	/* Observed on VT32-family software; no display-side behavior is known. */
	(void)A; (void)V;
}

static void Mapper296InstallHandlers(void) {
	/* VT09+ exposes 4 KiB internal CPU RAM.  Keep FCEUX's normal 2 KiB at
	 * $0000-$07FF/$1000-$17FF and supply the additional 2 KiB at the other
	 * half of each 4 KiB window. */
	SetReadHandler(0x0800, 0x0FFF, Mapper296ReadExtraRAM);
	SetWriteHandler(0x0800, 0x0FFF, Mapper296WriteExtraRAM);
	SetReadHandler(0x1800, 0x1FFF, Mapper296ReadExtraRAM);
	SetWriteHandler(0x1800, 0x1FFF, Mapper296WriteExtraRAM);

	SetReadHandler(0x4119, 0x4119, Mapper296Read4119);
	SetWriteHandler(0x411E, 0x411E, Mapper296Write411E);
	/* $412C is write-only on the documented mapper; override OneBus's
	 * FamilyPocket DIP read alias so Mapper296 software does not see a DIP. */
	SetReadHandler(0x412C, 0x412C, Mapper296Read412C);
	SetReadHandler(0x412D, 0x412D, Mapper296Read412D);
	SetReadHandler(0x414A, 0x414A, Mapper296Read414A);
	SetWriteHandler(0x4A00, 0x4A00, Mapper296Write4A00);
	SetWriteHandler(0x8000, 0xFFFF, Mapper296WriteLegacy);
}

static void Mapper296Power(void) {
	MMC1_shift = MMC1_bits = 0;
	memset(MMC1_reg, 0, sizeof(MMC1_reg));
	MMC1_reg[0] = 0x0C;
	Mapper296CNROMBank = 0;
	memset(Mapper296ExtraRAM, 0, sizeof(Mapper296ExtraRAM));
	encryptOpcodes = 0;
	encryptOpcodesPending = 0;
	encryptOpcodesPendingMode = 0;
	UNLOneBusPower();
	Mapper296InstallHandlers();
}

static void Mapper296Reset(void) {
	encryptOpcodes = 0;
	encryptOpcodesPending = 0;
	encryptOpcodesPendingMode = 0;
	UNLOneBusReset();
	Mapper296InstallHandlers();
}

void Mapper296_Init(CartInfo *info) {
	UNLOneBus_Init(info);
	info->Power = Mapper296Power;
	info->Reset = Mapper296Reset;
	cpuMangle = cpuMangles[0];
	ppuMangle = ppuMangles[0];
	mmc3Mangle = mmc3Mangles[0];
	Sync = Sync296;

	/* $411D.2 selects this unbanked RAM instead of ROM-backed CHR. */
	CHRRAMSIZE = 8192;
	CHRRAM = (uint8*)FCEU_gmalloc(CHRRAMSIZE);
	memset(CHRRAM, 0, CHRRAMSIZE);
	SetupCartCHRMapping(MAPPER296_CHR_CHIP, CHRRAM, CHRRAMSIZE, 1);
	AddExState(CHRRAM, CHRRAMSIZE, 0, "M2CR");
	AddExState(&StateRegs_296, ~0, 0, 0);
}

static void Sync436(void) {
	int OR =0;
	switch(submapper) {
		case 1:  OR = cpu410x[0x1C] &0x01 && ~cpu410x[0x1C] &0x04? 0x0800: 0x0000;
	                 PSync(0xF7FF, OR);
                         CSync(0xBFFF, OR <<3);
		         break;
		default: PSync(0xF3FF, (cpu410x[0x0F] &0x20? 0x0400: 0x0000) | (cpu410x[0x00] &0x40? 0x0800: 0x0000));
                         CSync(0x9FFF, (cpu410x[0x0F] &0x20? 0x2000: 0x0000) | (cpu410x[0x00] &0x04? 0x4000: 0x0000));
			 break;
	}
	SyncEncryption();
}

void Mapper436_OneBus_Init(CartInfo *info) {
	UNLOneBus_Init(info);
	cpuMangle = cpuMangles[0];
	ppuMangle = ppuMangles[0];
	mmc3Mangle = mmc3Mangles[0];
	
	CHRRAMSIZE = 8192;
	CHRRAM = (uint8*)FCEU_gmalloc(CHRRAMSIZE);
	SetupCartCHRMapping(0x10, CHRRAM, CHRRAMSIZE, 1);
	AddExState(CHRRAM, CHRRAMSIZE, 0, "CHRR");
	
	Sync = Sync436;
}
