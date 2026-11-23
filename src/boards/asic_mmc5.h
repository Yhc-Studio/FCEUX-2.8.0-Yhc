/* FCE Ultra - NES/Famicom Emulator
 * Yhc standalone MMC5 ASIC for Mapper 500.6, GPL-2.0-or-later.
 * Does not replace the existing mmc5.cpp / Mapper5_Init.
 * Inspired by NRS h_MMC5 state separation, adapted to FCEUX APIs.
 */
#ifndef YHC_ASIC_MMC5_H
#define YHC_ASIC_MMC5_H

#include "mapinc.h"

struct ASIC_MMC5_Config {
    uint32 prgBase8;     // 8-KiB PRG-ROM bank index, aligned to prgBanks8
    uint32 prgBanks8;    // power-of-two bank group, e.g. 16=128KiB
    uint32 chrBase1;     // 1-KiB CHR-ROM bank index, aligned to chrBanks1
    uint32 chrBanks1;    // power-of-two group, e.g. 128=128KiB
    int wramKiB;         // initialize once: 0/8/16/32/64 typical
    uint8 battery;
    uint8 chrRAM;       // whole selected game uses 8-KiB CHR-RAM
};

void ASIC_MMC5_Init(CartInfo* info, const ASIC_MMC5_Config* cfg);
bool ASIC_MMC5_SelectGame(uint32 prgBase8, uint32 prgBanks8,
                          uint32 chrBase1, uint32 chrBanks1, bool chrRAM);
void ASIC_MMC5_Power(void);
void ASIC_MMC5_Reset(void);
void ASIC_MMC5_Restore(int version);
// Change the visible MMC5 PRG-RAM banking without reallocating WRAM.
// Supports 0/8/16/32/64 KiB; physical storage is allocated once in Init.
bool ASIC_MMC5_SetWRAMKiB(int kib);
void ASIC_MMC5_ClearWRAM(void);
void ASIC_MMC5_ClearCHRRAM(void);
void ASIC_MMC5_Close(void);

#endif
