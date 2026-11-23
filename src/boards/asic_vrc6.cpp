/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2025 NewRisingSun
 *  VRC6 audio derived from FCEUX Mapper 24/26 by CaH4e3 (2009).
 *  This dedicated ASIC module does not replace the original mapper.
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
 */

#include "mapinc.h"
#include "asic_vrc6.h"

static void (*VRC6_cbSync)();
static int VRC6_A0, VRC6_A1;
static int (*VRC6_cbGetPRGBank)(uint8);
static int (*VRC6_cbGetCHRBank)(uint8);
static DECLFR ((*VRC6_cbReadWRAM));
static DECLFW ((*VRC6_cbWriteWRAM));
static uint8 VRC6_prg[2];
static uint8 VRC6_chr[8];
static uint8 VRC6_misc;
static uint8 VRC6_latch;
static uint8 VRC6_mode;
static uint8 VRC6_count;
static signed short int VRC6_cycles;

static SFORMAT VRC6_stateRegs[] = {
	{ VRC6_prg, 2, "VC6P" },
	{ VRC6_chr, 8, "VC6C" },
	{ &VRC6_misc, 1, "VC6M" },
	{ &VRC6_latch, 1, "V6LT" },
	{ &VRC6_mode, 1, "V6MO" },
	{ &VRC6_count, 1, "V6CT" },
	{ &VRC6_cycles, 2 | FCEUSTATE_RLSB, "V6CY" },
	{ NULL, 0, NULL }
};


// 500.7 uses its own VRC6 audio state; original Mapper 24/26 and NSF stay unchanged.
// FCEUX 2.6.6 sound callbacks are single-expansion callbacks, not a mixer list.
static void (*VRC6ASIC_sfun[3])(void) = { NULL, NULL, NULL };
static uint8 VRC6ASIC_PSG1[8] = {0};
static uint8 VRC6ASIC_PSG2[4] = {0};
static int32 VRC6ASIC_BC[3] = {0};
static int32 VRC6ASIC_vcount[3] = {0};
static int32 VRC6ASIC_dcount[2] = {0};
static int32 VRC6ASIC_sawLowClock = 0;
static uint8 VRC6ASIC_sawLowStep = 0;
static int32 VRC6ASIC_sawLowPhase = 0;
static uint32 VRC6ASIC_sawLowSample = 0;
static uint8 VRC6ASIC_sawHighStep = 0;
static int32 VRC6ASIC_sawHighPhase = 0;
static bool VRC6ASIC_active = false;
static bool VRC6ASIC_preserveOnBind = false;

static SFORMAT VRC6ASIC_soundState[] = {
    { VRC6ASIC_PSG1, 8, "6PS1" }, { VRC6ASIC_PSG2, 4, "6PS2" },
    { VRC6ASIC_BC, 12, "6SBC" }, { VRC6ASIC_vcount, 12, "6SVC" },
    { VRC6ASIC_dcount, 8, "6SDC" },
    { &VRC6ASIC_sawLowClock, 4, "6SLC" },
    { &VRC6ASIC_sawLowStep, 1, "6SLS" },
    { &VRC6ASIC_sawLowPhase, 4, "6SLP" },
    { &VRC6ASIC_sawLowSample, 4, "6SLV" },
    { &VRC6ASIC_sawHighStep, 1, "6SHS" },
    { &VRC6ASIC_sawHighPhase, 4, "6SHP" },
    { NULL, 0, NULL }
};

static INLINE int32 VRC6ASIC_ApplyVolume(int32 v)
{
	return (int32)(((int64)v * FSettings.VRC6Volume) >> 8);
}

static void VRC6ASIC_DoSQV1(void);
static void VRC6ASIC_DoSQV2(void);
static void VRC6ASIC_DoSawV(void);

static INLINE void VRC6ASIC_DoSQV(int x) {
	int32 V;
	int32 amp = VRC6ASIC_ApplyVolume(((((VRC6ASIC_PSG1[x << 2] & 15) << 8) * 6 / 8) >> 4));
	int32 start, end;

	start = VRC6ASIC_BC[x];
	end = (SOUNDTS << 16) / soundtsinc;
	if (end <= start) return;
	VRC6ASIC_BC[x] = end;

	if (VRC6ASIC_PSG1[(x << 2) | 0x2] & 0x80) {
		if (VRC6ASIC_PSG1[x << 2] & 0x80) {
			for (V = start; V < end; V++)
				Wave[V >> 4] += amp;
		}
		else {
			int32 thresh = (VRC6ASIC_PSG1[x << 2] >> 4) & 7;
			int32 freq = ((VRC6ASIC_PSG1[(x << 2) | 0x1] | ((VRC6ASIC_PSG1[(x << 2) | 0x2] & 15) << 8)) + 1) << 17;
			for (V = start; V < end; V++) {
				if (VRC6ASIC_dcount[x] > thresh)
					Wave[V >> 4] += amp;
				VRC6ASIC_vcount[x] -= nesincsize;
				while (VRC6ASIC_vcount[x] <= 0) {
					VRC6ASIC_vcount[x] += freq;
					VRC6ASIC_dcount[x] = (VRC6ASIC_dcount[x] + 1) & 15;
				}
			}
		}
	}
}

static void VRC6ASIC_DoSQV1(void) {
	VRC6ASIC_DoSQV(0);
}

static void VRC6ASIC_DoSQV2(void) {
	VRC6ASIC_DoSQV(1);
}

static void VRC6ASIC_DoSawV(void) {
	int V;
	int32 start, end;

	start = VRC6ASIC_BC[2];
	end = (SOUNDTS << 16) / soundtsinc;
	if (end <= start) return;
	VRC6ASIC_BC[2] = end;

	if (VRC6ASIC_PSG2[2] & 0x80) {
		/* preserved in VRC6ASIC_sawLowClock */
		uint32 freq3;
		/* preserved in VRC6ASIC_sawLowStep */
		/* preserved in VRC6ASIC_sawLowPhase */
		/* preserved in VRC6ASIC_sawLowSample */

		freq3 = (VRC6ASIC_PSG2[1] + ((VRC6ASIC_PSG2[2] & 15) << 8) + 1);

		for (V = start; V < end; V++) {
			VRC6ASIC_sawLowClock -= nesincsize;
			if (VRC6ASIC_sawLowClock <= 0) {
				int32 t;
			rea:
				t = freq3;
				t <<= 18;
				VRC6ASIC_sawLowClock += t;
				VRC6ASIC_sawLowPhase += VRC6ASIC_PSG2[0] & 0x3f;
				VRC6ASIC_sawLowStep++;
				if (VRC6ASIC_sawLowStep == 7) {
					VRC6ASIC_sawLowStep = 0;
					VRC6ASIC_sawLowPhase = 0;
				}
				if (VRC6ASIC_sawLowClock <= 0)
					goto rea;
				VRC6ASIC_sawLowSample = VRC6ASIC_ApplyVolume((((VRC6ASIC_sawLowPhase >> 3) & 0x1f) << 4) * 6 / 8);
			}
			Wave[V >> 4] += VRC6ASIC_sawLowSample;
		}
	}
}

static INLINE void VRC6ASIC_DoSQVHQ(int x) {
	int32 V;
	int32 amp = VRC6ASIC_ApplyVolume(((VRC6ASIC_PSG1[x << 2] & 15) << 8) * 6 / 8);

	if (VRC6ASIC_PSG1[(x << 2) | 0x2] & 0x80) {
		if (VRC6ASIC_PSG1[x << 2] & 0x80) {
			for (V = VRC6ASIC_BC[x]; V < (int)SOUNDTS; V++)
				WaveHi[V] += amp;
		}
		else {
			int32 thresh = (VRC6ASIC_PSG1[x << 2] >> 4) & 7;
			for (V = VRC6ASIC_BC[x]; V < (int)SOUNDTS; V++) {
				if (VRC6ASIC_dcount[x] > thresh)
					WaveHi[V] += amp;
				VRC6ASIC_vcount[x]--;
				if (VRC6ASIC_vcount[x] <= 0) {
					VRC6ASIC_vcount[x] = (VRC6ASIC_PSG1[(x << 2) | 0x1] | ((VRC6ASIC_PSG1[(x << 2) | 0x2] & 15) << 8)) + 1;
					VRC6ASIC_dcount[x] = (VRC6ASIC_dcount[x] + 1) & 15;
				}
			}
		}
	}
	VRC6ASIC_BC[x] = SOUNDTS;
}

static void VRC6ASIC_DoSQV1HQ(void) {
	VRC6ASIC_DoSQVHQ(0);
}

static void VRC6ASIC_DoSQV2HQ(void) {
	VRC6ASIC_DoSQVHQ(1);
}

static void VRC6ASIC_DoSawVHQ(void) {
	/* preserved in VRC6ASIC_sawHighStep */
	/* preserved in VRC6ASIC_sawHighPhase */
	int32 V;

	if (VRC6ASIC_PSG2[2] & 0x80) {
		for (V = VRC6ASIC_BC[2]; V < (int)SOUNDTS; V++) {
			WaveHi[V] += VRC6ASIC_ApplyVolume((((VRC6ASIC_sawHighPhase >> 3) & 0x1f) << 8) * 6 / 8);
			VRC6ASIC_vcount[2]--;
			if (VRC6ASIC_vcount[2] <= 0) {
				VRC6ASIC_vcount[2] = (VRC6ASIC_PSG2[1] + ((VRC6ASIC_PSG2[2] & 15) << 8) + 1) << 1;
				VRC6ASIC_sawHighPhase += VRC6ASIC_PSG2[0] & 0x3f;
				VRC6ASIC_sawHighStep++;
				if (VRC6ASIC_sawHighStep == 7) {
					VRC6ASIC_sawHighStep = 0;
					VRC6ASIC_sawHighPhase = 0;
				}
			}
		}
	}
	VRC6ASIC_BC[2] = SOUNDTS;
}


void VRC6ASIC_RunSound(int Count) {
	int x;

	VRC6ASIC_DoSQV1();
	VRC6ASIC_DoSQV2();
	VRC6ASIC_DoSawV();
	for (x = 0; x < 3; x++)
		VRC6ASIC_BC[x] = Count;
}

void VRC6ASIC_RunSoundHQ(void) {
	VRC6ASIC_DoSQV1HQ();
	VRC6ASIC_DoSQV2HQ();
	VRC6ASIC_DoSawVHQ();
}

void VRC6ASIC_HiSync(int32 ts) {
	int x;
	for (x = 0; x < 3; x++) VRC6ASIC_BC[x] = ts;
}

static void VRC6ASIC_ESI(void) {
	GameExpSound.RChange = VRC6ASIC_ESI;
	GameExpSound.Fill = VRC6ASIC_RunSound;
	GameExpSound.HiFill = VRC6ASIC_RunSoundHQ;
	GameExpSound.HiSync = VRC6ASIC_HiSync;

    // SaveState restore must rebind audio without erasing saved phases.
    if (!VRC6ASIC_preserveOnBind) {
        memset(VRC6ASIC_BC, 0, sizeof(VRC6ASIC_BC));
        memset(VRC6ASIC_vcount, 0, sizeof(VRC6ASIC_vcount));
        memset(VRC6ASIC_dcount, 0, sizeof(VRC6ASIC_dcount));
    }
	if (FSettings.SndRate) {
		if (FSettings.soundq >= 1) {
			VRC6ASIC_sfun[0] = VRC6ASIC_DoSQV1HQ;
			VRC6ASIC_sfun[1] = VRC6ASIC_DoSQV2HQ;
			VRC6ASIC_sfun[2] = VRC6ASIC_DoSawVHQ;
		}
		else {
			VRC6ASIC_sfun[0] = VRC6ASIC_DoSQV1;
			VRC6ASIC_sfun[1] = VRC6ASIC_DoSQV2;
			VRC6ASIC_sfun[2] = VRC6ASIC_DoSawV;
		}
	}
	else
		memset(VRC6ASIC_sfun, 0, sizeof(VRC6ASIC_sfun));
	/* Registered once by VRC6_addExState(), not at every sound rate change. */
}



static void VRC6ASIC_ResetSound(void) {
    memset(VRC6ASIC_PSG1, 0, sizeof(VRC6ASIC_PSG1));
    memset(VRC6ASIC_PSG2, 0, sizeof(VRC6ASIC_PSG2));
    memset(VRC6ASIC_BC, 0, sizeof(VRC6ASIC_BC));
    memset(VRC6ASIC_vcount, 0, sizeof(VRC6ASIC_vcount));
    memset(VRC6ASIC_dcount, 0, sizeof(VRC6ASIC_dcount));
    VRC6ASIC_sawLowClock = VRC6ASIC_sawLowPhase = 0;
    VRC6ASIC_sawLowStep = 0; VRC6ASIC_sawLowSample = 0;
    VRC6ASIC_sawHighStep = 0; VRC6ASIC_sawHighPhase = 0;
}
static void VRC6ASIC_DetachSound(void) {
    if (GameExpSound.Fill == VRC6ASIC_RunSound) GameExpSound.Fill = NULL;
    if (GameExpSound.HiFill == VRC6ASIC_RunSoundHQ) GameExpSound.HiFill = NULL;
    if (GameExpSound.HiSync == VRC6ASIC_HiSync) GameExpSound.HiSync = NULL;
    if (GameExpSound.RChange == VRC6ASIC_ESI) GameExpSound.RChange = NULL;
}
static void VRC6ASIC_CloseSound(void) {
    VRC6ASIC_DetachSound();
    if (GameExpSound.Kill == VRC6ASIC_CloseSound) GameExpSound.Kill = NULL;
    VRC6ASIC_active = false;
    memset(VRC6ASIC_sfun, 0, sizeof(VRC6ASIC_sfun));
}
static void VRC6ASIC_StartSound(bool restore = false) {
    VRC6ASIC_active = true;
    VRC6ASIC_preserveOnBind = restore;
    VRC6ASIC_ESI();
    VRC6ASIC_preserveOnBind = false;
    GameExpSound.Kill = VRC6ASIC_CloseSound;
}
static void VRC6ASIC_WriteSound(uint32 address, uint8 value) {
    uint8 index = (uint8)(address & 3);
    uint8 group = (uint8)((address >> 12) & 0xF);
    if (index > 2) return;
    if (group == 9 || group == 10) {
        const int voice = group - 9;
        if (VRC6ASIC_sfun[voice]) VRC6ASIC_sfun[voice]();
        VRC6ASIC_PSG1[(voice << 2) | index] = value;
    } else if (group == 11) {
        if (VRC6ASIC_sfun[2]) VRC6ASIC_sfun[2]();
        VRC6ASIC_PSG2[index] = value;
    }
}

void VRC6_syncWRAM (int OR) {
	if (PRGsize[0x10]) setprg8r(0x10, 0x6000, OR);
}

void VRC6_syncPRG (int AND, int OR) {
	setprg8(0x8000, (VRC6_cbGetPRGBank(0) & AND) | OR);
	setprg8(0xA000, (VRC6_cbGetPRGBank(1) & AND) | OR);
	setprg8(0xC000, (VRC6_cbGetPRGBank(2) & AND) | OR);
	setprg8(0xE000, (VRC6_cbGetPRGBank(3) & AND) | OR);
}

void VRC6_syncCHR (int AND, int OR) {
	setchr1(0x0000, (VRC6_cbGetCHRBank(0) & AND) | OR);
	setchr1(0x0400, (VRC6_cbGetCHRBank(1) & AND) | OR);
	setchr1(0x0800, (VRC6_cbGetCHRBank(2) & AND) | OR);
	setchr1(0x0C00, (VRC6_cbGetCHRBank(3) & AND) | OR);
	setchr1(0x1000, (VRC6_cbGetCHRBank(4) & AND) | OR);
	setchr1(0x1400, (VRC6_cbGetCHRBank(5) & AND) | OR);
	setchr1(0x1800, (VRC6_cbGetCHRBank(6) & AND) | OR);
	setchr1(0x1C00, (VRC6_cbGetCHRBank(7) & AND) | OR);
}

void VRC6_syncMirror () { /* Only emulates features used by known games, meaning mode 0 with CHR A10 substitution */
	setmirror(VRC6_misc &8? (VRC6_misc &4? MI_1: MI_0): VRC6_misc &4? MI_H: MI_V);
}

int VRC6_getPRGBank (uint8 bank) {
	if (bank &2)
		return bank &1? 0xFF: VRC6_prg[1];
	else
		return (VRC6_prg[0] << 1) | (bank & 1);
}

int VRC6_getCHRBank (uint8 bank) { /* Only emulates features used by known games, meaning mode 0 with CHR A10 substitution */
	return VRC6_chr[bank &7];
}

DECLFR (VRC6_readWRAM) {
	if (VRC6_misc &0x80)
		return VRC6_cbReadWRAM? VRC6_cbReadWRAM(A): CartBR(A);
	else
		return A >>8;
}

DECLFW (VRC6_writeWRAM) {
	if (VRC6_misc &0x80) {
		CartBW(A, V);
		if (VRC6_cbWriteWRAM) VRC6_cbWriteWRAM(A, V);
	}
}

DECLFW (VRC6_writeReg) {
	unsigned int index;
	index = (A &VRC6_A0? 1: 0) | (A &VRC6_A1? 2: 0);
	switch (A >>12 &7) {
		case 0: case 4:
			VRC6_prg[A >>14 &1] = V;
			VRC6_cbSync();
			break;
		case 1: case 2: // $9000/$A000: two pulse channels
            VRC6ASIC_WriteSound((A & 0xF000) | index, V);
            break;
		case 3:
            if (index == 3) {
                VRC6_misc = V;
                VRC6_cbSync();
            } else {
                // $B000-$B002: sawtooth audio registers.
                VRC6ASIC_WriteSound(0xB000 | index, V);
            }
            break;
		case 5: case 6:
			VRC6_chr[((A >> 11) & 4) | index] = V;
			VRC6_cbSync();
			break;
		case 7:
			switch (index) {
				case 0:
					VRC6_latch = V;
					break;
				case 1:
					VRC6_mode = V;
					if (VRC6_mode &0x02) {
						VRC6_count = VRC6_latch;
						VRC6_cycles = 341;
					}
					X6502_IRQEnd(FCEU_IQEXT);
					break;
				case 2:
					VRC6_mode = (VRC6_mode & ~0x02) | ((VRC6_mode << 1) & 0x02);
					X6502_IRQEnd(FCEU_IQEXT);
					break;
			}
			break;
	}
}

void VRC6_cpuCycle (int a) {
	while (a-- > 0) {
		if (VRC6_mode &0x02 && (VRC6_mode &0x04 || (VRC6_cycles -= 3) <= 0)) {
			if (~VRC6_mode &0x04) VRC6_cycles += 341;
			if (!++VRC6_count) {
				VRC6_count = VRC6_latch;
				X6502_IRQBegin(FCEU_IQEXT);
			}
		}
	}
}

void VRC6_clear () {
	VRC6_prg[0] = 0; VRC6_prg[1] = 0xFE;
	VRC6_chr[0] = 0; VRC6_chr[1] = 1; VRC6_chr[2] = 2; VRC6_chr[3] = 3; VRC6_chr[4] = 4; VRC6_chr[5] = 5; VRC6_chr[6] = 6; VRC6_chr[7] = 7;
	VRC6_misc = VRC6_latch = VRC6_mode = VRC6_count = 0;
    VRC6_cycles = 341;
    X6502_IRQEnd(FCEU_IQEXT);
    VRC6ASIC_ResetSound();
    if (VRC6_cbSync) VRC6_cbSync();
}

static void VRC6_setHandlers () {
	SetReadHandler(0x6000, 0x7FFF,  VRC6_readWRAM);
	SetWriteHandler(0x6000, 0x7FFF, VRC6_writeWRAM);
	SetReadHandler(0x8000, 0xFFFF, CartBR);
	SetWriteHandler(0x8000, 0xFFFF, VRC6_writeReg);
	MapIRQHook = VRC6_cpuCycle;
}

static void VRC6_configure (void (*sync)(), int A0, int A1, int (*prg)(uint8), int (*chr)(uint8), DECLFR((*read)), DECLFW((*write))) {
	VRC6_A0 = A0;
	VRC6_A1 = A1;
	VRC6_cbSync = sync;
	VRC6_cbGetPRGBank = prg? prg: VRC6_getPRGBank;
	VRC6_cbGetCHRBank = chr? chr: VRC6_getCHRBank;
	VRC6_cbReadWRAM = read;
	VRC6_cbWriteWRAM = write;
}

void VRC6_activate (uint8 clear, void (*sync)(), int A0, int A1, int (*prg)(uint8), int (*chr)(uint8), DECLFR((*read)), DECLFW((*write))) {
	VRC6_configure(sync, A0, A1, prg, chr, read, write);
    VRC6_setHandlers();
    if (clear) VRC6_clear();
    else if (VRC6_cbSync) VRC6_cbSync();
    VRC6ASIC_StartSound(!clear);
}

void VRC6_addExState () {
    AddExState(VRC6_stateRegs, ~0, 0, 0);
    AddExState(VRC6ASIC_soundState, ~0, 0, 0);
}

void VRC6_restore (int version) {
	VRC6_cbSync();
}

void VRC6_power () {
    VRC6_setHandlers();
    VRC6_clear();
    VRC6ASIC_StartSound();
}

void VRC6_init (CartInfo *info, void (*sync)(), int A0, int A1, int (*prg)(uint8), int (*chr)(uint8), DECLFR((*read)), DECLFW((*write))) {
	VRC6_addExState();
	VRC6_configure(sync, A0, A1, prg, chr, read, write);
	info->Power = VRC6_power;
	info->Reset = VRC6_cbSync;
    GameStateRestore = VRC6_restore;
    VRC6ASIC_StartSound();
}

// Called by Mapper 500.7 before switching to another VRC ASIC or menu.
void VRC6_deactivate(void) {
    if (MapIRQHook == VRC6_cpuCycle) MapIRQHook = NULL;
    X6502_IRQEnd(FCEU_IQEXT);
    VRC6ASIC_CloseSound();
}
