/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2025 NewRisingSun
 *  VRC7 FM audio derived from FCEUX Mapper 85 by CaH4e3 (2012).
 *  Uses the existing FCEUX emu2413 OPLL core; no new FM library is bundled.
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
#include "asic_vrc7.h"


#include "emu2413.h"  // Same FM core as existing Mapper 85; unique ASIC instance.
static OPLL* VRC7ASIC_OpLL = NULL;
static OPLL** VRC7ASIC_SavePtr = &VRC7ASIC_OpLL;
static int32 VRC7ASIC_dwave = 0;
static uint8 VRC7ASIC_index = 0;
static uint8 VRC7ASIC_enableFM = 1; // default: original VRC7a, 500.7 may select VRC7b
static uint8 VRC7ASIC_pirateIRQ = 0; // optional VRC7P (YOKO clone): IRQ at 248, scanline only

static void (*VRC7_cbSync)();
static int VRC7_A0;
static uint8 VRC7_prg[3];
static uint8 VRC7_chr[8];
static uint8 VRC7_misc;
static uint8 VRC7_latch;
static uint8 VRC7_mode;
static uint8 VRC7_count;
static signed short int VRC7_cycles;

static SFORMAT VRC7_stateRegs[] = {
	{ VRC7_prg, 3, "VC7P" },
	{ VRC7_chr, 8, "VC7C" },
	{ &VRC7_misc, 1, "VC7M" },
	{ &VRC7_latch, 1, "V7LT" },
	{ &VRC7_mode, 1, "V7MO" },
	{ &VRC7_count, 1, "V7CT" },
	{ &VRC7_cycles, 2 | FCEUSTATE_RLSB, "V7CY" },
	{ NULL, 0, NULL }
};

#define VRC7_MIXBUF_SIZE 4096
static int32 VRC7ASIC_MixBuf[VRC7_MIXBUF_SIZE];

/*
 * VRC7/OPLL has long continuous FM output.
 * If the UI changes FSettings.VRC7Volume in the middle of playback and we
 * immediately jump from old gain to new gain, the waveform amplitude becomes
 * discontinuous for one buffer, which is heard as a pop/click.
 *
 * Keep a private applied gain and ramp it to the target gain inside the next
 * generated audio block.  Other chip state is untouched.
 */
static int32 VRC7ASIC_CurrentVolume = -1;

/*
 * VRC7 is used in two different situations:
 *  1) normal Mapper 85 cartridges, such as Lagrange Point;
 *  2) NSF/NSFe playback, where VRC7 may be mixed with VRC6/FDS/MMC5/etc.
 *
 * The NSF multi-expansion mixer needs a temporary buffer and additive mixing so
 * that VRC7 does not overwrite other expansion chips.  However, using that path
 * for normal Mapper 85 games can subtly change OPLL buffer timing/behavior and
 * may cause short notes/SFX to sound jumpy.  Therefore Mapper 85 cartridges are
 * kept on the exact legacy OPLL_fillbuf path, while NSF/NSFe uses the safe
 * additive/scaled path.
 */
static int VRC7ASIC_NSFMode = 0;

static INLINE int32 VRC7ASIC_ClampVolume(int32 v)
{
	if (v < 0) return 0;
	if (v > 512) return 512;
	return v;
}

static void VRC7ASIC_FillScaled(int32* dst, int count, int quality)
{
	int done = 0;
	int32 targetVolume = VRC7ASIC_ClampVolume(FSettings.VRC7Volume);
	int32 startVolume;

	if (!dst || !VRC7ASIC_OpLL || count <= 0)
		return;

	if (VRC7ASIC_CurrentVolume < 0)
		VRC7ASIC_CurrentVolume = targetVolume;

	startVolume = VRC7ASIC_CurrentVolume;

	while (done < count)
	{
		int todo = count - done;
		if (todo > VRC7_MIXBUF_SIZE)
			todo = VRC7_MIXBUF_SIZE;

		memset(VRC7ASIC_MixBuf, 0, sizeof(int32) * todo);
		OPLL_fillbuf(VRC7ASIC_OpLL, VRC7ASIC_MixBuf, todo, quality);

		for (int i = 0; i < todo; i++)
		{
			int sampleIndex = done + i + 1;
			int32 vol = startVolume;

			if (targetVolume != startVolume)
				vol = startVolume + (int32)(((int64)(targetVolume - startVolume) * sampleIndex) / count);

			dst[done + i] += (int32)(((int64)VRC7ASIC_MixBuf[i] * vol) >> 8);
		}

		done += todo;
	}

	VRC7ASIC_CurrentVolume = targetVolume;
}

void VRC7ASIC_DoSound(void) {
	int32 z, a;
	if (FSettings.soundq >= 1)
		return;
	z = ((SOUNDTS << 16) / soundtsinc) >> 4;
	a = z - VRC7ASIC_dwave;

	if (VRC7ASIC_OpLL && a)
	{
		/*
		 * Mapper85/VRC7 cartridge path:
		 * keep the exact legacy OPLL_fillbuf() call style.  This avoids the
		 * one-frame skip/jump that can be heard in Lagrange Point's short menu
		 * confirmation SFX when the NSF additive/scaled path is used for carts.
		 */
		if (!VRC7ASIC_NSFMode)
			OPLL_fillbuf(VRC7ASIC_OpLL, &Wave[VRC7ASIC_dwave], a, 1);
		else
			VRC7ASIC_FillScaled(&Wave[VRC7ASIC_dwave], a, 1);
	}
	VRC7ASIC_dwave += a;
}

void VRC7ASIC_UpdateNeo(int32* Wave, int Count) {
	if (VRC7ASIC_OpLL && Count)
	{
		if (!VRC7ASIC_NSFMode)
			OPLL_fillbuf(VRC7ASIC_OpLL, Wave, Count, 4);
		else
			VRC7ASIC_FillScaled(Wave, Count, 4);
	}
}

void VRC7ASIC_Update(int Count) {
	int32 z, a;
	z = ((SOUNDTS << 16) / soundtsinc) >> 4;
	a = z - VRC7ASIC_dwave;
	if (VRC7ASIC_OpLL && a)
	{
		if (!VRC7ASIC_NSFMode)
			OPLL_fillbuf(VRC7ASIC_OpLL, &Wave[VRC7ASIC_dwave], a, 1);
		else
			VRC7ASIC_FillScaled(&Wave[VRC7ASIC_dwave], a, 1);
	}
	VRC7ASIC_dwave = 0;
}

static void VRC7ASIC_RateChange(void) {
	if (VRC7ASIC_OpLL)
		OPLL_set_rate(VRC7ASIC_OpLL, FSettings.SndRate ? FSettings.SndRate : 48000);
}

static void VRC7ASIC_Kill(void) {
    if (VRC7ASIC_OpLL) OPLL_delete(VRC7ASIC_OpLL);
    VRC7ASIC_OpLL = NULL;
	VRC7ASIC_CurrentVolume = -1;
	VRC7ASIC_NSFMode = 0;
}

static bool VRC7ASIC_preserveOnBind = false;

static void VRC7ASIC_ESI(void) {
	GameExpSound.RChange = VRC7ASIC_RateChange;
	GameExpSound.Kill = VRC7ASIC_Kill;
    if (!VRC7ASIC_OpLL)
        VRC7ASIC_OpLL = OPLL_new(3579545, FSettings.SndRate ? FSettings.SndRate : 48000);
    // Reinstall callbacks on SaveState load without resetting restored OPLL.
    if (!VRC7ASIC_preserveOnBind) {
        if (VRC7ASIC_OpLL) { OPLL_reset(VRC7ASIC_OpLL); OPLL_reset(VRC7ASIC_OpLL); }
        VRC7ASIC_dwave = 0;
        VRC7ASIC_CurrentVolume = VRC7ASIC_ClampVolume(FSettings.VRC7Volume);
    }
}


static void VRC7ASIC_DetachSound(void) {
    if (GameExpSound.Fill == VRC7ASIC_Update) GameExpSound.Fill = NULL;
    if (GameExpSound.NeoFill == VRC7ASIC_UpdateNeo) GameExpSound.NeoFill = NULL;
    if (GameExpSound.RChange == VRC7ASIC_RateChange) GameExpSound.RChange = NULL;
    if (GameExpSound.Kill == VRC7ASIC_Kill) GameExpSound.Kill = NULL;
}
static void VRC7ASIC_ActivateSound(void) {
    if (!VRC7ASIC_enableFM) return;
    VRC7ASIC_ESI();
    GameExpSound.Fill = VRC7ASIC_Update;
    GameExpSound.NeoFill = VRC7ASIC_UpdateNeo;
}
static void VRC7ASIC_ResetSound(void) {
    VRC7ASIC_index = 0; VRC7ASIC_dwave = 0;
    if (VRC7ASIC_OpLL) { OPLL_reset(VRC7ASIC_OpLL); OPLL_reset(VRC7ASIC_OpLL); }
}

void VRC7_syncWRAM (int OR) {
	if (PRGsize[0x10]) setprg8r(0x10, 0x6000, OR);
}

void VRC7_syncPRG (int AND, int OR) {
	setprg8(0x8000, (VRC7_prg[0] & AND) | OR);
	setprg8(0xA000, (VRC7_prg[1] & AND) | OR);
	setprg8(0xC000, (VRC7_prg[2] & AND) | OR);
	setprg8(0xE000,        (0xFF & AND) | OR);
}

int VRC7_getCHRBank(uint8 bank) { return VRC7_chr[bank & 7]; }

void VRC7_syncCHR (int AND, int OR) {
	setchr1(0x0000, (VRC7_chr[0] & AND) | OR);
	setchr1(0x0400, (VRC7_chr[1] & AND) | OR);
	setchr1(0x0800, (VRC7_chr[2] & AND) | OR);
	setchr1(0x0C00, (VRC7_chr[3] & AND) | OR);
	setchr1(0x1000, (VRC7_chr[4] & AND) | OR);
	setchr1(0x1400, (VRC7_chr[5] & AND) | OR);
	setchr1(0x1800, (VRC7_chr[6] & AND) | OR);
	setchr1(0x1C00, (VRC7_chr[7] & AND) | OR);
}

void VRC7_syncMirror () {
	setmirror(VRC7_misc &2? (VRC7_misc &1? MI_1: MI_0): VRC7_misc &1? MI_H: MI_V);
}

DECLFR (VRC7_readWRAM) {
	return VRC7_misc &0x80? CartBR(A): A >>8;
}

DECLFW (VRC7_writeWRAM) {
	if (VRC7_misc &0x80) CartBW(A, V);
}

DECLFW (VRC7_writeReg) {
	unsigned int index;
	index = A &VRC7_A0? 1: 0;
    // $9010/$9030: FM register index/data, with Mapper 85 A3/A4 alias.
    // Never interpret $9030 as a PRG bank write.
    if ((A & 0xF000) == 0x9000) {
        if ((A & 0x0030) == 0x0010 && (A & VRC7_A0)) {
            VRC7ASIC_index = V;
            return;
        }
        if ((A & 0x0030) == 0x0030 && (A & VRC7_A0)) {
            if (VRC7ASIC_enableFM && VRC7ASIC_OpLL && FSettings.SndRate) {
                // Finish pending normal-quality FM samples before register change.
                if (GameExpSound.Fill == VRC7ASIC_Update) VRC7ASIC_DoSound();
                OPLL_writeReg(VRC7ASIC_OpLL, VRC7ASIC_index, V);
            }
            return;
        }
    }
    switch (A >>12 &7) {
		case 0:
			VRC7_prg[index &1] = V;
			VRC7_cbSync();
			break;
		case 1:
			if (!index) {
				VRC7_prg[2] = V;
				VRC7_cbSync();
			}
			break;
		case 2: case 3: case 4: case 5:
			VRC7_chr[(((A - 0xA000) >> 11) & 6) | index] = V;
			VRC7_cbSync();
			break;
		case 6:	if (!index) {
				VRC7_misc = V;
				VRC7_cbSync();
			} else
				VRC7_latch = V;
			break;
		case 7:
			if (!index) {
				VRC7_mode = V;
				if (VRC7_mode &0x02) {
					VRC7_count = VRC7_latch;
					VRC7_cycles = 341;
				}
				X6502_IRQEnd(FCEU_IQEXT);
			} else {
				VRC7_mode = (VRC7_mode & ~0x02) | ((VRC7_mode << 1) & 0x02);
				X6502_IRQEnd(FCEU_IQEXT);
			}
			break;
	}
}

void VRC7_cpuCycle (int a) {
    while (a-- > 0) {
        if (VRC7ASIC_pirateIRQ) {
            // vrc7p.cpp: YOKO clone clocks only at PPU scanline cadence.
            if (VRC7_mode & 0x02) {
                VRC7_cycles -= 3;
                if (VRC7_cycles <= 0) {
                    VRC7_cycles += 341;
                    if (++VRC7_count == 248) {
                        VRC7_count = VRC7_latch;
                        X6502_IRQBegin(FCEU_IQEXT);
                    }
                }
            }
            continue;
        }
        if (VRC7_mode & 0x02 && (VRC7_mode & 0x04 || (VRC7_cycles -= 3) <= 0)) {
			if (~VRC7_mode &0x04) VRC7_cycles += 341;
			if (!++VRC7_count) {
				VRC7_count = VRC7_latch;
				X6502_IRQBegin(FCEU_IQEXT);
			}
		}
	}
}

void VRC7_clear () {
	VRC7_prg[0] = 0; VRC7_prg[1] = 1; VRC7_prg[2] = 0xFE;
	VRC7_chr[0] = 0; VRC7_chr[1] = 1; VRC7_chr[2] = 2; VRC7_chr[3] = 3; VRC7_chr[4] = 4; VRC7_chr[5] = 5; VRC7_chr[6] = 6; VRC7_chr[7] = 7;
    VRC7_misc = VRC7_latch = VRC7_mode = VRC7_count = 0;
    VRC7_cycles = 341;
    X6502_IRQEnd(FCEU_IQEXT);
    VRC7ASIC_ResetSound();
    if (VRC7_cbSync) VRC7_cbSync();
}

static void VRC7_setHandlers () {
	SetReadHandler(0x6000, 0x7FFF,  VRC7_readWRAM);
	SetWriteHandler(0x6000, 0x7FFF, VRC7_writeWRAM);
	SetReadHandler(0x8000, 0xFFFF, CartBR);
	SetWriteHandler(0x8000, 0xFFFF, VRC7_writeReg);
	MapIRQHook =VRC7_cpuCycle;
}

static void VRC7_configure (void (*sync)(), int A0) {
	VRC7_A0 = A0;
	VRC7_cbSync = sync;
}

void VRC7_activate (uint8 clear, void (*sync)(), int A0) {
	VRC7_configure(sync, A0);
    VRC7_setHandlers();
    VRC7ASIC_preserveOnBind = (clear == 0);
    VRC7ASIC_ActivateSound();
    VRC7ASIC_preserveOnBind = false;
    if (clear) VRC7_clear();
    else if (VRC7_cbSync) VRC7_cbSync();
}

void VRC7_addExState () {
    // Indirect OPLL state must have an allocated backing object when saved.
    if (!VRC7ASIC_OpLL)
        VRC7ASIC_OpLL = OPLL_new(3579545, FSettings.SndRate ? FSettings.SndRate : 48000);
    AddExState(VRC7_stateRegs, ~0, 0, 0);
    if (VRC7ASIC_OpLL)
        AddExState((void**)VRC7ASIC_SavePtr, sizeof(*VRC7ASIC_OpLL) | FCEUSTATE_INDIRECT, 0, "7OPL");
    AddExState(&VRC7ASIC_index, 1, 0, "7IDX");
    AddExState(&VRC7ASIC_enableFM, 1, 0, "7FME");
    AddExState(&VRC7ASIC_pirateIRQ, 1, 0, "7PIR");
    AddExState(&VRC7ASIC_dwave, 4, 0, "7DWA");
    AddExState(&VRC7ASIC_CurrentVolume, 4, 0, "7VOL");
}

void VRC7_restore (int version) {
	VRC7_cbSync();
}

void VRC7_power () {
    VRC7_setHandlers();
    VRC7ASIC_ActivateSound();
    VRC7_clear();
}

void VRC7_init (CartInfo *info, void (*sync)(), int A0) {
	VRC7_addExState();
	VRC7_configure(sync, A0);
	info->Power = VRC7_power;
	info->Reset = VRC7_cbSync;
    GameStateRestore = VRC7_restore;
    VRC7ASIC_ActivateSound();
}

// Per-game VRC7a (FM) vs VRC7b/non-FM configuration for Mapper 500.7.
void VRC7_setFMEnabled(uint8 enabled) {
    VRC7ASIC_enableFM = enabled ? 1 : 0;
    if (!VRC7ASIC_enableFM) VRC7ASIC_DetachSound();
}
void VRC7_deactivate(void) {
    if (MapIRQHook == VRC7_cpuCycle) MapIRQHook = NULL;
    X6502_IRQEnd(FCEU_IQEXT);
    VRC7ASIC_DetachSound();
}
// Board Close (not per-game switch); frees private OPLL core after savestates stop.
void VRC7_close(void) {
    VRC7_deactivate();
    VRC7ASIC_Kill();
}

// Optional YOKO pirate VRC7 variant from user-provided vrc7p.cpp.
// This changes ONLY the IRQ threshold (248 instead of 256); the game must
// also select A3 routing and FM disabled through the board profile.
void VRC7_setPirateMode(uint8 enabled) {
    VRC7ASIC_pirateIRQ = enabled ? 1 : 0;
}
