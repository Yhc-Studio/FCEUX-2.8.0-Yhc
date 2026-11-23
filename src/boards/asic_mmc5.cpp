/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
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

 /* Yhc independent ASIC instance, for Mapper 500.6 only.
 * Reference design: NRS h_MMC5.cpp / h_MMC5.h (register separation,
 * PPU, ExRAM, split-screen, IRQ state). The implementation below uses
 * the FCEUX-specific PPU/audio core from the user's existing mmc5.cpp.
 * Original Mapper 5 implementation is left entirely unchanged.
 */

#include "mapinc.h"
#include "asic_mmc5.h"

#include <array>

// The FCEUX PPU exposes its current scanline as an integer.
// GameHBIRQHook has signature void (*)(void), not void (*)(int).
extern int scanline;

// This translation unit owns its own state and never registers Mapper5_Init.
// Only when the future multicart activates this ASIC are shared PPU/audio
// callbacks assigned. Do not activate two MMC5 instances simultaneously.

static uint32 YHC5_prgBase8 = 0, YHC5_prgBanks8 = 16;
static uint32 YHC5_chrBase1 = 0, YHC5_chrBanks1 = 128;
static bool YHC5_chrRAMMode = false;
static bool YHC5_softReset = false;
static uint8 YHC5_chrRAM[8192];
static const int YHC5_CHR_RAM_CHIP = 0x11;

static bool YHC5_powerOfTwo(uint32 v) { return v && !(v & (v - 1)); }
static uint32 YHC5_resolvePRG8(uint32 bank) {
    return YHC5_prgBase8 + (bank & (YHC5_prgBanks8 - 1));
}
static void YHC5_mapPRG8(uint32 address, uint32 bank) {
    setprg8(address, YHC5_resolvePRG8(bank));
}
static uint32 YHC5_resolveCHR1(uint32 bank) {
    return YHC5_chrBase1 + (bank & (YHC5_chrBanks1 - 1));
}
static uint8* YHC5_chrData(uint32 firstLocalBank1) {
    if (YHC5_chrRAMMode)
        return YHC5_chrRAM + ((firstLocalBank1 & 7) << 10);
    if (!CHRptr[0]) return NULL;
    return CHRptr[0] + (YHC5_resolveCHR1(firstLocalBank1) << 10);
}
static void YHC5_mapCHR1(uint32 a, uint32 v) {
    if (YHC5_chrRAMMode) setchr1r(YHC5_CHR_RAM_CHIP, a, v & 7);
    else setchr1(a, YHC5_resolveCHR1(v));
}
static void YHC5_mapCHR2(uint32 a, uint32 v) {
    if (YHC5_chrRAMMode) setchr2r(YHC5_CHR_RAM_CHIP, a, v & 3);
    else setchr2(a, YHC5_resolveCHR1(v << 1) >> 1);
}
static void YHC5_mapCHR4(uint32 a, uint32 v) {
    if (YHC5_chrRAMMode) setchr4r(YHC5_CHR_RAM_CHIP, a, v & 1);
    else setchr4(a, YHC5_resolveCHR1(v << 2) >> 2);
}
static void YHC5_mapCHR8(uint32 v) {
    if (YHC5_chrRAMMode) setchr8r(YHC5_CHR_RAM_CHIP, v & 0);
    else setchr8(YHC5_resolveCHR1(v << 3) >> 3);
}

#define ABANKS MMC5SPRVPage
#define BBANKS MMC5BGVPage
#define SpriteON    (PPU[1] & 0x10)	//Show Sprite
#define ScreenON    (PPU[1] & 0x08)	//Show screen
#define PPUON       (PPU[1] & 0x18)	//PPU should operate
#define Sprite16    (PPU[0] & 0x20)	//Sprites 8x16/8x8

static void (*sfun)(int P);
static void (*psfun)(void);

void YHC5_MMC5RunSound(int Count);
void YHC5_MMC5RunSoundHQ(void);

// MMC5 uses separate sprite/background PPU pages. Resolve both through
// the board's CHR group; changing only setchr* would leave these stale.
static void YHC5_mapPage(bool sprite, uint32 a, uint32 bank, uint32 size1) {
    uint8* ptr = YHC5_chrData(bank * size1);
    if (!ptr) return;
    for (uint32 i = 0; i < size1; ++i) {
        if (sprite) MMC5SPRVPage[(a >> 10) + i] = ptr - a;
        else        MMC5BGVPage [(a >> 10) + i] = ptr - a;
    }
}
static void MMC5SPRVROM_BANK1(uint32 a, uint32 v) { YHC5_mapPage(true, a, v, 1); }
static void MMC5SPRVROM_BANK2(uint32 a, uint32 v) { YHC5_mapPage(true, a, v, 2); }
static void MMC5SPRVROM_BANK4(uint32 a, uint32 v) { YHC5_mapPage(true, a, v, 4); }
static void MMC5SPRVROM_BANK8(uint32 v) { YHC5_mapPage(true, 0, v, 8); }
static void MMC5BGVROM_BANK1(uint32 a, uint32 v) { YHC5_mapPage(false, a, v, 1); }
static void MMC5BGVROM_BANK2(uint32 a, uint32 v) { YHC5_mapPage(false, a, v, 2); }
static void MMC5BGVROM_BANK4(uint32 a, uint32 v) { YHC5_mapPage(false, a, v, 4); }
static void MMC5BGVROM_BANK8(uint32 v) { YHC5_mapPage(false, 0, v, 8); }

static std::array<uint8, 4> PRGBanks;
static uint8 WRAMPage;
static std::array<uint16, 8> CHRBanksA;
static std::array<uint16, 4> CHRBanksB;
static std::array<uint8, 2> WRAMMaskEnable;
static uint8 YHC5_mmc5ABMode;                /* A=0, B=1 */

static uint8 IRQScanline, IRQEnable;
static uint16 YHC5_timerCount = 0;
static uint8 YHC5_timerRunning = 0, YHC5_timerIRQ = 0;
static uint8 CHRMode, NTAMirroring, NTFill, ATFill;

static uint8 MMC5IRQR;
static uint8 MMC5LineCounter;
static uint8 mmc5psize, mmc5vsize;
static std::array<uint8, 2> mul;

static uint32 WRAMSIZE = 0;
static uint8* WRAM = NULL;
static uint8* MMC5fill = NULL;
static uint8* ExRAM = NULL;
static uint8 MMC5battery = 0;

const int MMC5WRAMMAX = 1 << 7; // 7 bits in register interface (real MMC5 has only 4 pins, however)
static uint8 MMC5WRAMsize = 0; //configuration, not state
static uint8 MMC5WRAMIndex[MMC5WRAMMAX]; //configuration, not state

static std::array<uint8, 4> MMC5ROMWrProtect;
static std::array<uint8, 5> MMC5MemIn;

static void YHC5_refreshIRQ(void) {
    if (((MMC5IRQR & 0x80) && (IRQEnable & 0x80)) || YHC5_timerIRQ)
        X6502_IRQBegin(FCEU_IQEXT);
    else
        X6502_IRQEnd(FCEU_IQEXT);
}
static void YHC5_cpuCycle(int cycles) {
    while (cycles-- > 0 && YHC5_timerRunning) {
        if (--YHC5_timerCount == 0) {
            YHC5_timerIRQ = 1;
            YHC5_refreshIRQ();
        }
    }
}
static void MMC5CHRA(void);
static void MMC5CHRB(void);

#define MMC5SPRVRAMADR(V)   &MMC5SPRVPage[(V) >> 10][(V)]

uint8* YHC5_MMC5BGVRAMADR(uint32 A)
{
	if (newppu)
	{
		if (Sprite16)
		{
			bool isPattern = PPUON != 0;
			if (ppuphase == PPUPHASE_OBJ && isPattern)
				return &ABANKS[(A) >> 10][(A)];
			if (ppuphase == PPUPHASE_BG && isPattern)
				return &BBANKS[(A) >> 10][(A)];
			else if (YHC5_mmc5ABMode == 0)
				return &ABANKS[(A) >> 10][(A)];
			else
				return &BBANKS[(A) >> 10][(A)];
		}
		else return &ABANKS[(A) >> 10][(A)];;
	}

	if (!Sprite16) {
		if (YHC5_mmc5ABMode == 0)
			return &ABANKS[(A) >> 10][(A)];
		else
			return &BBANKS[(A) >> 10][(A)];
	}
	else return &BBANKS[(A) >> 10][(A)];
}

static void YHC5_mmc5_PPUWrite(uint32 A, uint8 V) {
	uint32 tmp = A;
	extern uint8 PALRAM[0x20];
	extern uint8 UPALRAM[0x03];

	if (tmp >= 0x3F00) {
		if (!(tmp & 3)) {
			if (!(tmp & 0xC)) {
				PALRAM[0x00] = PALRAM[0x04] = PALRAM[0x08] = PALRAM[0x0C] = V & 0x3F;
				PALRAM[0x10] = PALRAM[0x14] = PALRAM[0x18] = PALRAM[0x1C] = V & 0x3F;
			}
			else
				UPALRAM[((tmp & 0xC) >> 2) - 1] = V & 0x3F;
		}
		else
			PALRAM[tmp & 0x1F] = V & 0x3F;
	}
	else if (tmp < 0x2000) {
		if (PPUCHRRAM & (1 << (tmp >> 10)))
			VPage[tmp >> 10][tmp] = V;
	}
	else {
		if (PPUNTARAM & (1 << ((tmp & 0xF00) >> 10)))
			vnapage[((tmp & 0xF00) >> 10)][tmp & 0x3FF] = V;
	}
}

extern uint32 NTRefreshAddr;
uint8 FASTCALL YHC5_mmc5_PPURead(uint32 A)
{
	bool split = false;
	if (newppu)
	{
		if ((MMC5HackSPMode & 0x80) && !(MMC5HackCHRMode & 2))
		{
			int target = MMC5HackSPMode & 0x1f;
			int side = MMC5HackSPMode & 0x40;
			int ht = NTRefreshAddr & 31;

			if (side == 0)
			{
				if (ht < target) split = true;
			}
			else
			{
				if (ht >= target) split = true;
			}
		}
	}

	if (A < 0x2000)
	{
		if (Sprite16)
		{
			bool isPattern = !!PPUON;
			if (ppuphase == PPUPHASE_OBJ && isPattern)
				return ABANKS[(A) >> 10][(A)];
			if (ppuphase == PPUPHASE_BG && isPattern)
			{
				if (split)
					return MMC5HackVROMPTR[MMC5HackSPPage * 0x1000 + (A & 0xFFF)];

				//uhhh call through to this more sophisticated function, only if it's really needed?
				//we should probably reuse it completely, if we can
				if (MMC5HackCHRMode == 1)
					return *FCEUPPU_GetCHR(A, NTRefreshAddr);

				return BBANKS[(A) >> 10][(A)];
			}
			else if (YHC5_mmc5ABMode == 0)
				return ABANKS[(A) >> 10][(A)];
			else
				return BBANKS[(A) >> 10][(A)];
		}
		else
		{
			if (ppuphase == PPUPHASE_BG && ScreenON)
			{
				if (split)
					return MMC5HackVROMPTR[MMC5HackSPPage * 0x1000 + (A & 0xFFF)];

				//uhhh call through to this more sophisticated function, only if it's really needed?
				//we should probably reuse it completely, if we can
				if (MMC5HackCHRMode == 1)
					return *FCEUPPU_GetCHR(A, NTRefreshAddr);
			}

			return ABANKS[(A) >> 10][(A)];
		}
	}
	else
	{
		if (split)
		{
			static const int kHack = -1; //dunno if theres science to this or if it just fixes SDF (cant be bothered to think about it)
			int linetile = (newppu_get_scanline() + kHack) / 8 + MMC5HackSPScroll;

			//REF NT: return 0x2000 | (v << 0xB) | (h << 0xA) | (vt << 5) | ht;
			//REF AT: return 0x2000 | (v << 0xB) | (h << 0xA) | 0x3C0 | ((vt & 0x1C) << 1) | ((ht & 0x1C) >> 2);

			if ((A & 0x3FF) >= 0x3C0)
			{
				A &= ~(0x1C << 1); //mask off VT
				A |= (linetile & 0x1C) << 1; //mask on adjusted VT
				return ExRAM[A & 0x3FF];
			}
			else
			{
				A &= ~((0x1F << 5) | (1 << 0xB)); //mask off VT and V
				A |= (linetile & 31) << 5; //mask on adjusted VT (V doesnt make any sense, I think)
				return ExRAM[A & 0x3FF];
			}
		}

		if (MMC5HackCHRMode == 1)
		{
			if ((A & 0x3FF) >= 0x3C0)
			{
				uint8 byte = ExRAM[NTRefreshAddr & 0x3ff];
				//get attribute part and paste it 4x across the byte
				byte >>= 6;
				byte *= 0x55;
				return byte;
			}
		}

		return vnapage[(A >> 10) & 0x3][A & 0x3FF];
	}
}

// CRC-specific MMC5 board table intentionally stays in original mmc5.cpp.

static void BuildWRAMSizeTable(void) {
	bool other = false; // non-standard configuration
	// fill first 8 entries
	int x;
	for (x = 0; x < 8; x++) {
		switch (MMC5WRAMsize) {
		case 0: MMC5WRAMIndex[x] = 255; break;                      //X,X,X,X,X,X,X,X
		case 1: MMC5WRAMIndex[x] = (x > 3) ? 255 : 0; break;        //0,0,0,0,X,X,X,X
		case 2: MMC5WRAMIndex[x] = (x & 4) >> 2; break;             //0,0,0,0,1,1,1,1
		case 4: MMC5WRAMIndex[x] = (x > 3) ? 255 : (x & 3); break;  //0,1,2,3,X,X,X,X
		case 8: MMC5WRAMIndex[x] = x; break;                        //0,1,2,3,4,5,6,7
		default: MMC5WRAMIndex[x] = x; other = true; break;         //0,1,2...
		}
	}
	// extend to fill complete table
	if (other)
	{
		for (x = 0; x < MMC5WRAMMAX && x < MMC5WRAMsize; ++x) MMC5WRAMIndex[x] = x; // linear mapping
		for (x = MMC5WRAMsize; x < MMC5WRAMMAX; ++x) MMC5WRAMIndex[x] = MMC5WRAMIndex[x - MMC5WRAMsize]; // repeat to fill table
		// theoretically the table fill should decompose into powers of two for possible mismatched SRAM combos,
		// but I don't want to complicate the code with unnecessary hypotheticals
	}
	else
	{
		for (x = 8; x < MMC5WRAMMAX; ++x) MMC5WRAMIndex[x] = MMC5WRAMIndex[x & 7]; // fill table, repeating groups of 8
	}
}

static void MMC5CHRA(void) {
	int x;
	switch (mmc5vsize & 3) {
	case 0:
		YHC5_mapCHR8(CHRBanksA[7]);
		MMC5SPRVROM_BANK8(CHRBanksA[7]);
		break;
	case 1:
		YHC5_mapCHR4(0x0000, CHRBanksA[3]);
		YHC5_mapCHR4(0x1000, CHRBanksA[7]);
		MMC5SPRVROM_BANK4(0x0000, CHRBanksA[3]);
		MMC5SPRVROM_BANK4(0x1000, CHRBanksA[7]);
		break;
	case 2:
		YHC5_mapCHR2(0x0000, CHRBanksA[1]);
		YHC5_mapCHR2(0x0800, CHRBanksA[3]);
		YHC5_mapCHR2(0x1000, CHRBanksA[5]);
		YHC5_mapCHR2(0x1800, CHRBanksA[7]);
		MMC5SPRVROM_BANK2(0x0000, CHRBanksA[1]);
		MMC5SPRVROM_BANK2(0x0800, CHRBanksA[3]);
		MMC5SPRVROM_BANK2(0x1000, CHRBanksA[5]);
		MMC5SPRVROM_BANK2(0x1800, CHRBanksA[7]);
		break;
	case 3:
		for (x = 0; x < 8; x++) {
			YHC5_mapCHR1(x << 10, CHRBanksA[x]);
			MMC5SPRVROM_BANK1(x << 10, CHRBanksA[x]);
		}
		break;
	}
}

static void MMC5CHRB(void) {
	int x;
	switch (mmc5vsize & 3) {
	case 0:
		YHC5_mapCHR8(CHRBanksB[3]);
		MMC5BGVROM_BANK8(CHRBanksB[3]);
		break;
	case 1:
		YHC5_mapCHR4(0x0000, CHRBanksB[3]);
		YHC5_mapCHR4(0x1000, CHRBanksB[3]);
		MMC5BGVROM_BANK4(0x0000, CHRBanksB[3]);
		MMC5BGVROM_BANK4(0x1000, CHRBanksB[3]);
		break;
	case 2:
		YHC5_mapCHR2(0x0000, CHRBanksB[1]);
		YHC5_mapCHR2(0x0800, CHRBanksB[3]);
		YHC5_mapCHR2(0x1000, CHRBanksB[1]);
		YHC5_mapCHR2(0x1800, CHRBanksB[3]);
		MMC5BGVROM_BANK2(0x0000, CHRBanksB[1]);
		MMC5BGVROM_BANK2(0x0800, CHRBanksB[3]);
		MMC5BGVROM_BANK2(0x1000, CHRBanksB[1]);
		MMC5BGVROM_BANK2(0x1800, CHRBanksB[3]);
		break;
	case 3:
		for (x = 0; x < 8; x++) {
			YHC5_mapCHR1(x << 10, CHRBanksB[x & 3]);
			MMC5BGVROM_BANK1(x << 10, CHRBanksB[x & 3]);
		}
		break;
	}
}

static void MMC5WRAM(uint32 A, uint32 V) {
	V = MMC5WRAMIndex[V & (MMC5WRAMMAX - 1)];
	if (V != 255) {
		setprg8r(0x10, A, V);
		FCEU_CheatAddRAM(8, 0x6000, (WRAM + ((V * 8192) & (WRAMSIZE - 1))));
		MMC5MemIn[(A - 0x6000) >> 13] = 1;
	}
	else
		MMC5MemIn[(A - 0x6000) >> 13] = 0;
}

static void MMC5PRG(void) {
    // Every MMC5 ROM bank maps inside the selected multicart PRG group.
    // WRAM slots keep the original writable RAM mapping and protection bits.
    uint32 bank8[4] = {0,0,0,0};
    bool rom[4] = {true,true,true,true};
    uint32 ram[4] = {0,0,0,0};
    switch (mmc5psize & 3) {
    case 0:
        for (int i=0;i<4;++i) bank8[i] = (PRGBanks[3] & 0x7C) + i;
        break;
    case 1:
        for (int i=0;i<2;++i) {
            rom[i] = (PRGBanks[1] & 0x80) != 0;
            bank8[i] = (PRGBanks[1] & 0x7E) + i;
            ram[i] = ((PRGBanks[1] & 0x7E) + i) & (MMC5WRAMMAX - 1);
        }
        bank8[2] = PRGBanks[3] & 0x7E;
        bank8[3] = bank8[2] + 1;
        break;
    case 2:
        for (int i=0;i<2;++i) {
            rom[i] = (PRGBanks[1] & 0x80) != 0;
            bank8[i] = (PRGBanks[1] & 0x7E) + i;
            ram[i] = ((PRGBanks[1] & 0x7E) + i) & (MMC5WRAMMAX - 1);
        }
        rom[2] = (PRGBanks[2] & 0x80) != 0;
        bank8[2] = PRGBanks[2] & 0x7F;
        ram[2] = PRGBanks[2] & (MMC5WRAMMAX - 1);
        bank8[3] = PRGBanks[3] & 0x7F;
        break;
    case 3:
        for (int i=0;i<3;++i) {
            rom[i] = (PRGBanks[i] & 0x80) != 0;
            bank8[i] = PRGBanks[i] & 0x7F;
            ram[i] = PRGBanks[i] & (MMC5WRAMMAX - 1);
        }
        bank8[3] = PRGBanks[3] & 0x7F;
        break;
    }
    for (int i=0;i<4;++i) {
        const uint32 a = 0x8000 + (i << 13);
        MMC5ROMWrProtect[i] = rom[i] ? 1 : 0;
        MMC5MemIn[i+1] = 1;
        if (rom[i]) YHC5_mapPRG8(a, bank8[i]);
        else MMC5WRAM(a, ram[i]);
    }
}

static DECLFW(Mapper5_write) {
	switch (A) {
	case 0x5100:
		mmc5psize = V;
		MMC5PRG();
		break;
	case 0x5101:
		mmc5vsize = V;
		if (!YHC5_mmc5ABMode) {
			MMC5CHRB();
			MMC5CHRA();
		}
		else {
			MMC5CHRA();
			MMC5CHRB();
		}
		break;
	case 0x5102:
		WRAMMaskEnable[0] = V;
		break;
	case 0x5103:
		WRAMMaskEnable[1] = V;
		break;
	case 0x5104:
		CHRMode = V;
		MMC5HackCHRMode = V & 3;
		break;
	case 0x5105:
	{
		int x;
		for (x = 0; x < 4; x++) {
			switch ((V >> (x << 1)) & 3) {
			case 0: PPUNTARAM |= 1 << x; vnapage[x] = NTARAM; break;
			case 1: PPUNTARAM |= 1 << x; vnapage[x] = NTARAM + 0x400; break;
			case 2: PPUNTARAM |= 1 << x; vnapage[x] = ExRAM; break;
			case 3: PPUNTARAM &= ~(1 << x); vnapage[x] = MMC5fill; break;
			}
		}
		NTAMirroring = V;
		break;
	}
	case 0x5106:
		if (V != NTFill)
			FCEU_dwmemset(MMC5fill, (V | (V << 8) | (V << 16) | (V << 24)), 0x3c0);
		NTFill = V;
		break;
	case 0x5107:
		if (V != ATFill) {
			unsigned char moop = V | (V << 2) | (V << 4) | (V << 6);
			FCEU_dwmemset(MMC5fill + 0x3c0, moop | (moop << 8) | (moop << 16) | (moop << 24), 0x40);
		}
		ATFill = V;
		break;
	case 0x5113:
		WRAMPage = V;
		MMC5WRAM(0x6000, V & (MMC5WRAMMAX - 1));
		break;
	case 0x5114:
	case 0x5115:
	case 0x5116:
	case 0x5117:
		PRGBanks[A & 3] = V;
		MMC5PRG();
		break;
	case 0x5120:
	case 0x5121:
	case 0x5122:
	case 0x5123:
	case 0x5124:
	case 0x5125:
	case 0x5126:
	case 0x5127:
		YHC5_mmc5ABMode = 0;
		CHRBanksA[A & 7] = V | ((MMC50x5130 & 0x3) << 8);
		MMC5CHRA();
		break;
	case 0x5128:
	case 0x5129:
	case 0x512a:
	case 0x512b:
		YHC5_mmc5ABMode = 1;
		CHRBanksB[A & 3] = V | ((MMC50x5130 & 0x3) << 8);
		MMC5CHRB();
		break;
	case 0x5130: MMC50x5130 = V; break;
	case 0x5200: MMC5HackSPMode = V; break;
	case 0x5201: MMC5HackSPScroll = (V >> 3) & 0x1F; break;
	case 0x5202: MMC5HackSPPage = (V & 0x3F) & (YHC5_chrRAMMode ? 1u : ((YHC5_chrBanks1 >> 2) - 1u)); break;
	case 0x5203: IRQScanline = V; YHC5_refreshIRQ(); break;
	case 0x5204: IRQEnable = V & 0x80; YHC5_refreshIRQ(); break;
	case 0x5205: mul[0] = V; break;
	case 0x5206: mul[1] = V; break;
    case 0x5209: YHC5_timerCount = (YHC5_timerCount & 0xFF00) | V;
                 YHC5_timerRunning = 1; break;
    case 0x520A: YHC5_timerCount = (YHC5_timerCount & 0x00FF) | (uint16(V) << 8); break;
	}
}

static DECLFR(MMC5_ReadROMRAM) {
	if (MMC5MemIn[(A - 0x6000) >> 13])
		return Page[A >> 11][A];
	else
		return X.DB;
}

static DECLFW(MMC5_WriteROMRAM) {
	if ((A >= 0x8000) && (MMC5ROMWrProtect[(A - 0x8000) >> 13]))
		return;
	if (MMC5MemIn[(A - 0x6000) >> 13])
		if (((WRAMMaskEnable[0] & 3) | ((WRAMMaskEnable[1] & 3) << 2)) == 6)
			Page[A >> 11][A] = V;
}

static DECLFW(MMC5_ExRAMWr) {
	if (MMC5HackCHRMode != 3)
		ExRAM[A & 0x3ff] = V;
}

static DECLFR(MMC5_ExRAMRd) {
	return ExRAM[A & 0x3ff];
}

static DECLFR(MMC5_read) {
	switch (A) {
	case 0x5204: {
		uint8 x;
        x = MMC5IRQR;
#ifdef FCEUDEF_DEBUGGER
        if (!fceuindbg)
#endif
        {
            MMC5IRQR &= 0x40;
            YHC5_refreshIRQ();
        }
        return x;
	}
	case 0x5205:
		return(mul[0] * mul[1]);
	case 0x5206:
		return((mul[0] * mul[1]) >> 8);
    case 0x5209: {
        const uint8 value = YHC5_timerIRQ ? 0x80 : 0;
#ifdef FCEUDEF_DEBUGGER
        if (!fceuindbg)
#endif
        {
            YHC5_timerIRQ = 0;
            YHC5_refreshIRQ();
        }
        return value;
    }
	}
	return(X.DB);
}

void YHC5_MMC5Synco(void) {
	int x;

	MMC5PRG();
	for (x = 0; x < 4; x++) {
		switch ((NTAMirroring >> (x << 1)) & 3) {
		case 0: PPUNTARAM |= 1 << x; vnapage[x] = NTARAM; break;
		case 1: PPUNTARAM |= 1 << x; vnapage[x] = NTARAM + 0x400; break;
		case 2: PPUNTARAM |= 1 << x; vnapage[x] = ExRAM; break;
		case 3: PPUNTARAM &= ~(1 << x); vnapage[x] = MMC5fill; break;
		}
	}
	MMC5WRAM(0x6000, WRAMPage & (MMC5WRAMMAX - 1));
	if (!YHC5_mmc5ABMode) {
		MMC5CHRB();
		MMC5CHRA();
	}
	else {
		MMC5CHRA();
		MMC5CHRB();
	}

	//in case the fill register changed, we need to overwrite the fill buffer
	FCEU_dwmemset(MMC5fill, NTFill | (NTFill << 8) | (NTFill << 16) | (NTFill << 24), 0x3c0);
	{
		unsigned char moop = ATFill | (ATFill << 2) | (ATFill << 4) | (ATFill << 6);
		FCEU_dwmemset(MMC5fill + 0x3c0, moop | (moop << 8) | (moop << 16) | (moop << 24), 0x40);
	}

	MMC5HackCHRMode = CHRMode & 3;

	//zero 17-apr-2013 - why the heck should this happen here? anything in a `synco` should be depending on the state.
	//im going to leave it commented out to see what happens
	//X6502_IRQEnd(FCEU_IQEXT);
}

void YHC5_MMC5_hb(int scanline) {
	//zero 24-jul-2014 - revised for newer understanding, to fix metal slader glory credits. see r7371 in bizhawk

	int sl = scanline + 1;
	int ppuon = (PPU[1] & 0x18);

	if (!ppuon || sl >= 241)
	{
		// whenever rendering is off for any reason (vblank or forced disable
		// the irq counter resets, as well as the inframe flag (easily verifiable from software)
		MMC5IRQR &= ~0x40;
		MMC5IRQR &= ~0x80;
		MMC5LineCounter = 0;
		YHC5_refreshIRQ();
		return;
	}

	if (!(MMC5IRQR & 0x40))
	{
		MMC5IRQR |= 0x40;
		MMC5IRQR &= ~0x80;
		MMC5LineCounter = 0;
		YHC5_refreshIRQ();
	}
	else
	{
		MMC5LineCounter++;
		if (MMC5LineCounter == IRQScanline)
		{
			MMC5IRQR |= 0x80;
            YHC5_refreshIRQ();
		}
	}

}

// FCEUX calls this HBlank hook without arguments.  Use the PPU's current
// scanline to preserve the existing MMC5_hb(int) scanline-IRQ behavior.
static void YHC5_MMC5_hb_callback(void) {
    YHC5_MMC5_hb(scanline);
}

void YHC5_MMC5_StateRestore(int version) {
	YHC5_MMC5Synco();
}

typedef struct {
	uint16 wl[2];
	uint8 env[2];
	uint8 enable;
	uint8 running;
	uint8 raw;
	uint8 rawcontrol;
	int32 dcount[2];
	int32 BC[3];
	int32 vcount[2];
} MMC5APU;

static MMC5APU MMC5Sound;

/*
 * NSF player note:
 * MMC5 has 2 square channels and one raw PCM/DAC path ($5010/$5011).
 * A lot of multi-expansion NSF/NSFe files use MMC5 only as two extra square
 * channels.  If their driver touches $5011 accidentally or leaves a non-zero
 * DAC value while $5010 permits PCM output, FCEUX's legacy raw PCM renderer
 * outputs a DC/stepped signal that is heard as buzz/noise in mixed-chip songs.
 *
 * Keep normal MMC5 cartridges unchanged.  In NSF mode, mute only the raw
 * PCM/DAC path by default, while leaving both MMC5 square channels active.
 * Set MMC5_NSF_MUTE_RAW_PCM to 0 if you need to test a NSF that intentionally
 * uses MMC5 PCM samples.
 */
#define MMC5_NSF_MUTE_RAW_PCM 1
static int MMC5NSFMode = 0;


static INLINE int32 MMC5ApplyVolume(int32 v)
{
	return (int32)(((int64)v * FSettings.MMC5Volume) >> 8);
}


static void Do5PCM() {
	int32 V;
	int32 start, end;

#if MMC5_NSF_MUTE_RAW_PCM
	if (MMC5NSFMode)
	{
		end = (SOUNDTS << 16) / soundtsinc;
		if (end > MMC5Sound.BC[2])
			MMC5Sound.BC[2] = end;
		return;
	}
#endif

	start = MMC5Sound.BC[2];
	end = (SOUNDTS << 16) / soundtsinc;
	if (end <= start) return;
	MMC5Sound.BC[2] = end;

	if (!(MMC5Sound.rawcontrol & 0x40) && MMC5Sound.raw)
		for (V = start; V < end; V++)
			Wave[V >> 4] += MMC5ApplyVolume(MMC5Sound.raw << 1);
}

static void Do5PCMHQ() {
	uint32 V;
#if MMC5_NSF_MUTE_RAW_PCM
	if (MMC5NSFMode)
	{
		MMC5Sound.BC[2] = SOUNDTS;
		return;
	}
#endif
	if (!(MMC5Sound.rawcontrol & 0x40) && MMC5Sound.raw)
		for (V = MMC5Sound.BC[2]; V < SOUNDTS; V++)
			WaveHi[V] += MMC5ApplyVolume(MMC5Sound.raw << 5);
	MMC5Sound.BC[2] = SOUNDTS;
}


static DECLFW(Mapper5_SW) {
	A &= 0x1F;

	GameExpSound.Fill = YHC5_MMC5RunSound;
	GameExpSound.HiFill = YHC5_MMC5RunSoundHQ;

	switch (A) {
	case 0x10: if (psfun) psfun(); MMC5Sound.rawcontrol = V; break;
	case 0x11: if (psfun) psfun(); MMC5Sound.raw = V; break;

	case 0x0:
	case 0x4:
		if (sfun) sfun(A >> 2);
		MMC5Sound.env[A >> 2] = V;
		break;
	case 0x2:
	case 0x6:
		if (sfun) sfun(A >> 2);
		MMC5Sound.wl[A >> 2] &= ~0x00FF;
		MMC5Sound.wl[A >> 2] |= V & 0xFF;
		break;
	case 0x3:
	case 0x7:
		MMC5Sound.wl[A >> 2] &= ~0x0700;
		MMC5Sound.wl[A >> 2] |= (V & 0x07) << 8;
		MMC5Sound.running |= 1 << (A >> 2);
		break;
	case 0x15:
		if (sfun) {
			sfun(0);
			sfun(1);
		}
		MMC5Sound.running &= V;
		MMC5Sound.enable = V;
		break;
	}
}

static void Do5SQ(int P) {
	static int tal[4] = { 1, 2, 4, 6 };
	int32 V, amp, rthresh, wl;
	int32 start, end;

	start = MMC5Sound.BC[P];
	end = (SOUNDTS << 16) / soundtsinc;
	if (end <= start) return;
	MMC5Sound.BC[P] = end;

	wl = MMC5Sound.wl[P] + 1;
	//amp = (MMC5Sound.env[P] & 0xF) << 4;
	amp = MMC5ApplyVolume((MMC5Sound.env[P] & 0xF) * 12);
	rthresh = tal[(MMC5Sound.env[P] & 0xC0) >> 6];

	if (wl >= 8 && (MMC5Sound.running & (P + 1))) {
		int dc, vc;

		wl <<= 18;
		dc = MMC5Sound.dcount[P];
		vc = MMC5Sound.vcount[P];

		for (V = start; V < end; V++) {
			if (dc < rthresh)
				Wave[V >> 4] += amp;
			vc -= nesincsize;
			while (vc <= 0) {
				vc += wl;
				dc = (dc + 1) & 7;
			}
		}
		MMC5Sound.dcount[P] = dc;
		MMC5Sound.vcount[P] = vc;
	}
}

static void Do5SQHQ(int P) {
	static int tal[4] = { 1, 2, 4, 6 };
	uint32 V;
	int32 amp, rthresh, wl;

	wl = MMC5Sound.wl[P] + 1;
	//amp = ((MMC5Sound.env[P] & 0xF) << 8);
	amp = MMC5ApplyVolume((MMC5Sound.env[P] & 0xF) * 192);
	rthresh = tal[(MMC5Sound.env[P] & 0xC0) >> 6];

	if (wl >= 8 && (MMC5Sound.running & (P + 1))) {
		int dc, vc;

		wl <<= 1;

		dc = MMC5Sound.dcount[P];
		vc = MMC5Sound.vcount[P];
		for (V = MMC5Sound.BC[P]; V < SOUNDTS; V++) {
			if (dc < rthresh)
				WaveHi[V] += amp;
			vc--;
			if (vc <= 0) { /* Less than zero when first started. */
				vc = wl;
				dc = (dc + 1) & 7;
			}
		}
		MMC5Sound.dcount[P] = dc;
		MMC5Sound.vcount[P] = vc;
	}
	MMC5Sound.BC[P] = SOUNDTS;
}

void YHC5_MMC5RunSoundHQ(void) {
	Do5SQHQ(0);
	Do5SQHQ(1);
	Do5PCMHQ();
}

void YHC5_MMC5HiSync(int32 ts) {
	int x;
	for (x = 0; x < 3; x++)
		MMC5Sound.BC[x] = ts;
}

void YHC5_MMC5RunSound(int Count) {
	int x;
	Do5SQ(0);
	Do5SQ(1);
	Do5PCM();
	for (x = 0; x < 3; x++)
		MMC5Sound.BC[x] = Count;
}

void YHC5_Mapper5_ESI(void) {
	GameExpSound.RChange = YHC5_Mapper5_ESI;
	GameExpSound.Fill = YHC5_MMC5RunSound;
	GameExpSound.HiFill = YHC5_MMC5RunSoundHQ;
	if (FSettings.SndRate) {
		if (FSettings.soundq >= 1) {
			sfun = Do5SQHQ;
			psfun = Do5PCMHQ;
		}
		else {
			sfun = Do5SQ;
			psfun = Do5PCM;
		}
	}
	else {
		sfun = 0;
		psfun = 0;
	}
	memset(MMC5Sound.BC, 0, sizeof(MMC5Sound.BC));
	memset(MMC5Sound.vcount, 0, sizeof(MMC5Sound.vcount));
	GameExpSound.HiSync = YHC5_MMC5HiSync;
}

static void GenMMC5Power(void) {

	PRGBanks.fill(0xFF);
	WRAMPage = 0;
	CHRBanksA.fill(0);
	CHRBanksB.fill(0);
	WRAMMaskEnable.fill(0xFF);
	YHC5_mmc5ABMode = 0;
	IRQScanline = 0;
	IRQEnable = 0;
	CHRMode = 0;
    // These FCEUX PPU/MMC5 globals are registers too. In a multicart,
    // retaining them would leak the previous game's CHR upper-bank bits
    // or vertical split into the new game/menu after Reset/COMMIT.
    MMC50x5130 = 0;
    MMC5HackSPMode = 0;
    MMC5HackSPScroll = 0;
    MMC5HackSPPage = 0;
    MMC5HackCHRMode = 0;
	NTAMirroring = NTFill = ATFill = 0;
	MMC5IRQR = 0;
	MMC5LineCounter = 0;
    YHC5_timerCount = 0;
    YHC5_timerRunning = 0;
    YHC5_timerIRQ = 0;
    YHC5_refreshIRQ();
	mmc5psize = mmc5vsize = 3;
	mul.fill(0);

	MMC5ROMWrProtect.fill(0);
	MMC5MemIn.fill(0);

	// MMC5fill is and 8-bit tile index, and a 2-bit attribute implented as a mirrored nametable
	u8 nval = MMC5fill[0x000];
	u8 aval = MMC5fill[0x3C0] & 3; aval = aval | (aval << 2) | (aval << 4) | (aval << 6);
	FCEU_dwmemset(MMC5fill + 0x000, nval | (nval << 8) | (nval << 16) | (nval << 24), 0x3C0);
	FCEU_dwmemset(MMC5fill + 0x3C0, aval | (aval << 8) | (aval << 16) | (aval << 24), 0x040);

	if (!YHC5_softReset && MMC5battery == 0) {
        if (WRAM) FCEU_MemoryRand(WRAM, MMC5WRAMsize * 8 * 1024);
		FCEU_MemoryRand(MMC5fill, 1024);
		FCEU_MemoryRand(ExRAM, 1024);
	}

    // A new game/reset must not inherit MMC5 PCM or envelope state.
    memset(&MMC5Sound, 0, sizeof(MMC5Sound));
    YHC5_Mapper5_ESI();
    YHC5_MMC5Synco();

	SetWriteHandler(0x4020, 0x5bff, Mapper5_write);
	SetReadHandler(0x4020, 0x5bff, MMC5_read);

	SetWriteHandler(0x5c00, 0x5fff, MMC5_ExRAMWr);
	SetReadHandler(0x5c00, 0x5fff, MMC5_ExRAMRd);

	SetWriteHandler(0x6000, 0xFFFF, MMC5_WriteROMRAM);
	SetReadHandler(0x6000, 0xFFFF, MMC5_ReadROMRAM);

	SetWriteHandler(0x5000, 0x5015, Mapper5_SW);
	SetWriteHandler(0x5205, 0x5206, Mapper5_write);
	SetReadHandler(0x5205, 0x5206, MMC5_read);

    GameHBIRQHook=YHC5_MMC5_hb_callback;
    MapIRQHook=YHC5_cpuCycle;
	//	FCEU_CheatAddRAM(8, 0x6000, WRAM);
	FCEU_CheatAddRAM(1, 0x5c00, ExRAM);
}

static SFORMAT MMC5_StateRegs[] = {
	{ &PRGBanks, 4, "PRGB" },
	{ &CHRBanksA, 16, "CHRA" },
	{ &CHRBanksB, 8, "CHRB" },
	{ &WRAMPage, 1, "WRMP" },
	{ &WRAMMaskEnable, 2, "WRME" },
	{ &YHC5_mmc5ABMode, 1, "ABMD" },
	{ &IRQScanline, 1, "IRQS" },
	{ &IRQEnable, 1, "IRQE" },
    { &YHC5_timerCount, 2 | FCEUSTATE_RLSB, "Y5TC" },
    { &YHC5_timerRunning, 1, "Y5TR" },
    { &YHC5_timerIRQ, 1, "Y5TI" },
	{ &CHRMode, 1, "CHRM" },
	{ &NTAMirroring, 1, "NTAM" },
	{ &NTFill, 1, "NTFL" },
	{ &ATFill, 1, "ATFL" },

	//zero 17-apr-2013 - added
	{ &MMC5IRQR, 1, "IRQR" },
	{ &MMC5LineCounter, 1, "LCTR" },
	{ &mmc5psize, 1, "PSIZ" },
	{ &mmc5vsize, 1, "VSIZ" },
	{ &mul, 2, "MUL2" },
	{ &MMC5ROMWrProtect, 4, "WRPR" },
	{ &MMC5MemIn, 5, "MEMI" },

	{ &MMC5Sound.wl[0], 2 | FCEUSTATE_RLSB, "SDW0" },
	{ &MMC5Sound.wl[1], 2 | FCEUSTATE_RLSB, "SDW1" },
	{ MMC5Sound.env, 2, "SDEV" },
	{ &MMC5Sound.enable, 1, "SDEN" },
	{ &MMC5Sound.running, 1, "SDRU" },
	{ &MMC5Sound.raw, 1, "SDRW" },
	{ &MMC5Sound.rawcontrol, 1, "SDRC" },

	//zero 17-apr-2013 - added
	{ &MMC5Sound.dcount[0], 4 | FCEUSTATE_RLSB, "DCT0" },
	{ &MMC5Sound.dcount[1], 4 | FCEUSTATE_RLSB, "DCT1" },
	{ &MMC5Sound.BC[0], 4 | FCEUSTATE_RLSB, "BC00" },
	{ &MMC5Sound.BC[1], 4 | FCEUSTATE_RLSB, "BC01" },
	{ &MMC5Sound.BC[2], 4 | FCEUSTATE_RLSB, "BC02" },
	{ &MMC5Sound.vcount[0], 4 | FCEUSTATE_RLSB, "VCT0" },
	{ &MMC5Sound.vcount[1], 4 | FCEUSTATE_RLSB, "VCT1" },
	{ 0 }
};

static void YHC5_GenMMC5_Init(CartInfo* info, int wsize, int battery) {
	MMC5NSFMode = 0;
	if (wsize) {
		WRAM = (uint8*)FCEU_malloc(wsize * 1024);
		FCEU_MemoryRand(WRAM, wsize * 1024);
		SetupCartPRGMapping(0x10, WRAM, wsize * 1024, 1);
		AddExState(WRAM, wsize * 1024, 0, "WRAM");
	}

	MMC5fill = (uint8*)FCEU_malloc(1024);
	ExRAM = (uint8*)FCEU_malloc(1024);

	FCEU_MemoryRand(MMC5fill, 1024);
	FCEU_MemoryRand(ExRAM, 1024);

	AddExState(ExRAM, 1024, 0, "ERAM");
	AddExState(&MMC5HackSPMode, 1, 0, "SPLM");
	AddExState(&MMC5HackSPScroll, 1, 0, "SPLS");
	AddExState(&MMC5HackSPPage, 1, 0, "SPLP");
	AddExState(&MMC50x5130, 1, 0, "5130");
	AddExState(MMC5_StateRegs, ~0, 0, 0);
    AddExState(&YHC5_prgBase8, 4 | FCEUSTATE_RLSB, 0, "Y5PB");
    AddExState(&YHC5_prgBanks8, 4 | FCEUSTATE_RLSB, 0, "Y5PS");
    AddExState(&YHC5_chrBase1, 4 | FCEUSTATE_RLSB, 0, "Y5CB");
    AddExState(&YHC5_chrBanks1, 4 | FCEUSTATE_RLSB, 0, "Y5CS");
    AddExState(&YHC5_chrRAMMode, 1, 0, "Y5RM");
    AddExState(YHC5_chrRAM, sizeof(YHC5_chrRAM), 0, "Y5CR");

	MMC5WRAMsize = wsize / 8;
	BuildWRAMSizeTable();
    // The active amount of WRAM can change per multicart game.
    // The full RAM backing is still allocated only once.
    AddExState(&MMC5WRAMsize, 1, 0, "Y5WS");
	GameStateRestore = ASIC_MMC5_Restore;
	info->Power = GenMMC5Power;

	MMC5battery = battery;
	if (battery && WRAM) {
		uint32 saveGameSize = 0;
		if (info->ines2)
		{
			saveGameSize = info->battery_wram_size;
		}
		else
		{
			//this is more complex than it looks because it MUST BE, I guess. is there an assumption that only 8KB of 16KB is battery backed? That's NES mappers for you
			//I added 64KB for the new 64KB homebrews
			if (wsize <= 16)
				saveGameSize = 8192;
			else if (wsize == 64)
				saveGameSize = 64 * 1024;
			else
				saveGameSize = 32768;
		}
        if (saveGameSize > (uint32)wsize * 1024u) saveGameSize = (uint32)wsize * 1024u;
		info->addSaveGameBuf(WRAM, saveGameSize);
	}

	MMC5HackVROMMask = (YHC5_chrRAMMode ? 1u : ((YHC5_chrBanks1 >> 2) - 1u));
	MMC5HackExNTARAMPtr = ExRAM;
	MMC5Hack = 1;
	MMC5HackVROMPTR = YHC5_chrRAMMode ? YHC5_chrRAM : (CHRptr[0] ? CHRptr[0] + (YHC5_chrBase1 << 10) : NULL);
	MMC5HackCHRMode = 0;
	MMC5HackSPMode = MMC5HackSPScroll = MMC5HackSPPage = 0;
	YHC5_Mapper5_ESI();

	FFCEUX_PPURead = YHC5_mmc5_PPURead;
	FFCEUX_PPUWrite = YHC5_mmc5_PPUWrite;
}


/* --------------------------------------------------------------
 * Public ASIC API for a future Yhc 500.6 board. No mapper 5 hooks
 * are registered until ASIC_MMC5_Init is explicitly called.
 * -------------------------------------------------------------- */

// This is an ASIC-only initialization. A board still must provide the
// selection register protocol and Reset-to-menu implementation.
void ASIC_MMC5_Init(CartInfo* info, const ASIC_MMC5_Config* cfg) {
    if (!info || !cfg) return;
    if (cfg->wramKiB < 0 || cfg->wramKiB > 64 || (cfg->wramKiB & 7) ||
        !YHC5_powerOfTwo(cfg->prgBanks8) || cfg->prgBanks8 < 4 ||
        !YHC5_powerOfTwo(cfg->chrBanks1) || cfg->chrBanks1 < 8 ||
        (cfg->prgBase8 & (cfg->prgBanks8 - 1)) ||
        (cfg->chrBase1 & (cfg->chrBanks1 - 1)) ||
        cfg->prgBase8 > 8192u || cfg->prgBanks8 > 8192u-cfg->prgBase8 ||
        cfg->chrBase1 > 65536u || cfg->chrBanks1 > 65536u-cfg->chrBase1)
        return;
    YHC5_prgBase8 = cfg->prgBase8;
    YHC5_prgBanks8 = cfg->prgBanks8;
    YHC5_chrBase1 = cfg->chrBase1;
    YHC5_chrBanks1 = cfg->chrBanks1;
    YHC5_chrRAMMode = cfg->chrRAM != 0;
    YHC5_softReset = false;
    WRAMSIZE = (uint32)cfg->wramKiB * 1024u;
    memset(YHC5_chrRAM, 0, sizeof(YHC5_chrRAM));
    SetupCartCHRMapping(YHC5_CHR_RAM_CHIP, YHC5_chrRAM, sizeof(YHC5_chrRAM), 1);
    YHC5_GenMMC5_Init(info, cfg->wramKiB, cfg->battery);
    info->Power = ASIC_MMC5_Power;
    info->Reset = ASIC_MMC5_Reset;
    info->Close = ASIC_MMC5_Close;
}

bool ASIC_MMC5_SelectGame(uint32 prgBase8, uint32 prgBanks8,
                          uint32 chrBase1, uint32 chrBanks1, bool chrRAM) {
    // Does not infer the physical ROM size: the board/packer must ensure
    // these regions exist and don't overlap other games.
    if (!YHC5_powerOfTwo(prgBanks8) || prgBanks8 < 4 ||
        !YHC5_powerOfTwo(chrBanks1) || chrBanks1 < 8 ||
        (prgBase8 & (prgBanks8-1)) || (chrBase1 & (chrBanks1-1)) ||
        prgBase8 > 8192u || prgBanks8 > 8192u-prgBase8 ||
        chrBase1 > 65536u || chrBanks1 > 65536u-chrBase1)
        return false;
    // Unlike FCEUX setchr* (which applies the loaded cart's CHR mask),
    // the special MMC5 sprite/background and split-screen paths use
    // direct CHRptr[0] pointers. Refuse out-of-image regions rather
    // than allowing those direct pointers to reach beyond the ROM.
    if (!chrRAM && (!CHRptr[0] || chrBase1 > CHRmask1[0] ||
        chrBanks1 - 1u > CHRmask1[0] - chrBase1))
        return false;
    YHC5_prgBase8 = prgBase8;
    YHC5_prgBanks8 = prgBanks8;
    YHC5_chrBase1 = chrBase1;
    YHC5_chrBanks1 = chrBanks1;
    YHC5_chrRAMMode = chrRAM;
    MMC5HackVROMMask = chrRAM ? 1u : (chrBanks1 >> 2) - 1u;
    MMC5HackVROMPTR = chrRAM ? YHC5_chrRAM :
        (CHRptr[0] ? CHRptr[0] + (chrBase1 << 10) : NULL);
    YHC5_MMC5Synco();
    return true;
}

void ASIC_MMC5_Power(void) {
    YHC5_softReset = false;
    GenMMC5Power();
}
void ASIC_MMC5_Reset(void) {
    YHC5_softReset = true;
    GenMMC5Power();
    YHC5_softReset = false;
}
void ASIC_MMC5_Restore(int version) {
    (void)version;
    BuildWRAMSizeTable();
    MMC5HackVROMMask = YHC5_chrRAMMode ? 1u : ((YHC5_chrBanks1 >> 2) - 1u);
    MMC5HackVROMPTR = YHC5_chrRAMMode ? YHC5_chrRAM :
        (CHRptr[0] ? CHRptr[0] + (YHC5_chrBase1 << 10) : NULL);
    YHC5_MMC5Synco();
    YHC5_refreshIRQ();
}
bool ASIC_MMC5_SetWRAMKiB(int kib) {
    if (!(kib == 0 || kib == 8 || kib == 16 || kib == 32 || kib == 64) ||
        (uint32)kib * 1024u > WRAMSIZE)
        return false;
    MMC5WRAMsize = (uint8)(kib / 8);
    BuildWRAMSizeTable();
    // Reapply the running MMC5 RAM-bank registers using the new mask.
    YHC5_MMC5Synco();
    return true;
}
void ASIC_MMC5_ClearWRAM(void) {
    if (WRAM && WRAMSIZE)
        memset(WRAM, 0, WRAMSIZE);
}
void ASIC_MMC5_ClearCHRRAM(void) {
    memset(YHC5_chrRAM, 0, sizeof(YHC5_chrRAM));
}

void ASIC_MMC5_Close(void) {
    // Only free resources allocated by this ASIC instance.
    if (WRAM) { FCEU_gfree(WRAM); WRAM = NULL; }
    if (MMC5fill) { FCEU_gfree(MMC5fill); MMC5fill = NULL; }
    if (ExRAM) { FCEU_gfree(ExRAM); ExRAM = NULL; }
    MMC5Hack = 0;
    if (GameHBIRQHook == YHC5_MMC5_hb_callback) GameHBIRQHook = NULL;
    if (MapIRQHook == YHC5_cpuCycle) MapIRQHook = NULL;
    if (GameExpSound.Fill == YHC5_MMC5RunSound) {
        GameExpSound.Fill = NULL;
        GameExpSound.HiFill = NULL;
        GameExpSound.RChange = NULL;
        GameExpSound.HiSync = NULL;
    }
}
