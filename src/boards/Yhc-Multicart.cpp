/* FCE Ultra - NES/Famicom Emulator
 *
 * Yhc MultiCart - unified NES 2.0 mapper 500, v2.2 (MMC5/500.6 extension)
 *
 *   500.0: UNROM / UOROM, up to 32 MiB PRG
 *   500.1: AxROM, up to 16 MiB PRG (original banking retained)
 *   500.2: A/B/UxROM, up to 64 MiB PRG
 *   500.3: NROM/CNROM/GxROM/BNROM/Color Dreams, up to 32 MiB PRG + 16 MiB CHR
 *   500.4: MMC1-based multicart, up to 32 MiB PRG + 16 MiB CHR
 *   500.5: MMC3 TxROM, TxSROM (118), TQROM (119)
 *   500.5: MMC3 TxROM family, up to 64 MiB PRG + 64 MiB CHR
 *   500.6: MMC5 ExROM family, up to 64 MiB PRG + 64 MiB CHR
 *
 * Protocol additions (new, not in the older mapper files):
 *   500.0: reg[1] bit 2 = 0: UNROM 8x16 KiB per outer bank
 *                           1: UOROM 16x16 KiB per outer bank
 *   500.0: reg[1] bit 3 = extended 512 KiB UxROM game block
 *   500.1: reg[1] bit 1 = AxROM 512 KiB game block
 *   500.2: reg[1] bit 6 = PRG outer bank bit 8 (upper 32 MiB)
 *   500.1: reg[1] bit 2 selects screen source (0=latch bit4, 1=$6001 bit0)
 *
 * reg[0] and reg[1] are written at $6000..$6FFF using (address & 1).
 * reg[1] bit 7 still locks further register writes.
 * The latch remains controlled by the pre-existing Latch_Init behavior.
 *
 * Derived from the original Yhc Mapper500/501/502 implementations.
 * This program is free software under GPL-2.0-or-later.
 */

#include "mapinc.h"
#include "latch.h"
#include "asic_mmc1.h"
#include "asic_mmc3.h"
#include "asic_mmc5.h"
#include "asic_vrc1.h"
#include "asic_vrc2and4.h"
#include "asic_vrc3.h"
#include "asic_vrc6.h"
#include "asic_vrc7.h"
#include <string.h>

static uint8 reg[2];
static uint8 submapper;

static SFORMAT StateRegs[] = {
    { reg, 2, "REGS" },
    { 0 }
};

/*
 * Mapper 500.3: Yhc discrete mapper family (v1.4)
 *
 * $5000: PRG base, 16 KiB bank, bits 0..5
 * $5001: CHR base,  8 KiB bank, bits 0..6
 * $5002: [L. TT HPCC] -- TT: 0=NROM/CNROM, 1=GxROM,
 *                                    2=BNROM,      3=Color Dreams
 *          L: board registers lock; H: 1=H mirror; P: legacy 32K PRG
 *          CC: CHR group 8/16/32/64 KiB (1/2/4/8 banks)
 * $5003: PRG base bits 6..10
 * $5004: CHR base bits 7..10
 * $5005: PRG group code 0..4 = 32/64/128/256/512 KiB
 *        (used by modes 1..3, ignored in legacy mode 0)
 * $5006: bit0: use 8 KiB CHR-RAM in mode 0 (mode 2 always uses RAM)
 * $5007: reserved; writes have no effect for submapper 500.3
 * $8000-$FFFF: legacy CHR bank write, or mode-dependent PRG+CHR latch
 *
 * This is a multicart's own extended control protocol. It does not
 * claim bit-for-bit identity with a standalone Nintendo mapper board.
 *
 * PRG-ROM maximum 32 MiB; CHR-ROM maximum 16 MiB. Each game starts
 * at a power-of-two aligned PRG and CHR group allocated by the packer.
 * setchr8r(0x10, 0) maps the dedicated 8 KiB CHR-RAM for BNROM.
 */
enum {
    YHC_M3_LEGACY = 0,
    YHC_M3_GXROM = 1,
    YHC_M3_BNROM = 2,
    YHC_M3_COLORDREAMS = 3,
    YHC_M3_CHRRAM_CHIP = 0x10
};

static uint8 M5003Regs[7];
static uint8 M5003GameLatch;
static uint8 M5003CHRRAM[8192];

static SFORMAT M5003StateRegs[] = {
    { M5003Regs, 7, "M3RG" },
    { &M5003GameLatch, 1, "M3LT" },
    { M5003CHRRAM, 8192, "M3RM" },
    { 0 }
};

static uint32 Mapper5003_PRGBase16(void)
{
    return ((uint32)(M5003Regs[3] & 0x1F) << 6) |
        (uint32)(M5003Regs[0] & 0x3F);
}

static uint32 Mapper5003_CHRBase8(void)
{
    return ((uint32)(M5003Regs[4] & 0x0F) << 7) |
        (uint32)(M5003Regs[1] & 0x7F);
}

static void Mapper5003_Sync(void)
{
    const uint32 prgBase16 = Mapper5003_PRGBase16();
    const uint32 chrBase8 = Mapper5003_CHRBase8();
    const uint32 chrGroupBanks = 1u << (M5003Regs[2] & 0x03);
    const uint32 chrGroupBase = chrBase8 & ~(chrGroupBanks - 1u);
    const uint8 mode = (M5003Regs[2] >> 4) & 3;
    uint32 chrInner = 0;

    if (mode == YHC_M3_LEGACY)
    {
        // Existing v1.3 16/32 KiB PRG mapping; do not reinterpret $5005.
        if (M5003Regs[2] & 0x04)
        {
            const uint32 evenBase16 = prgBase16 & ~1u;
            setprg16(0x8000, evenBase16);
            setprg16(0xC000, evenBase16 + 1u);
        }
        else
        {
            setprg16(0x8000, prgBase16);
            setprg16(0xC000, prgBase16);
        }
        chrInner = (uint32)M5003GameLatch;
    }
    else
    {
        // PRG base is in 16 KiB units, group size is in 32 KiB banks.
        // Align the group start, and mask the game's inner bank number.
        uint32 prgGroupBanks32 = 1u << (M5003Regs[5] & 7);
        if (prgGroupBanks32 > 16u)
            prgGroupBanks32 = 16u;
        const uint32 prgGroupBanks16 = prgGroupBanks32 << 1;
        const uint32 prgGroupBase16 = prgBase16 & ~(prgGroupBanks16 - 1u);
        uint32 prgInner = 0;

        if (mode == YHC_M3_GXROM)
        {
            // Mapper 66 compatible: D7..4 PRG, D3..0 CHR.
            // Group masks reduce these to the available game banks.
            prgInner = (M5003GameLatch >> 4) & 0x0F;
            chrInner = M5003GameLatch & 0x0F;
        }
        else if (mode == YHC_M3_BNROM)
        {
            // Mapper 34 BNROM: D3..0 PRG; dedicated 8 KiB CHR-RAM.
            prgInner = M5003GameLatch & 0x0F;
        }
        else  // YHC_M3_COLORDREAMS
        {
            // Mapper 11: D3..0 PRG, D7..4 CHR (opposite of GxROM).
            prgInner = M5003GameLatch & 0x0F;
            chrInner = (M5003GameLatch >> 4) & 0x0F;
        }

        setprg32(0x8000,
            (prgGroupBase16 >> 1) | (prgInner & (prgGroupBanks32 - 1u)));
    }

    if (mode == YHC_M3_BNROM ||
        (mode == YHC_M3_LEGACY && (M5003Regs[6] & 1)))
    {
        // CHR-RAM must remain writable with CHR-ROM present elsewhere.
        setchr8r(YHC_M3_CHRRAM_CHIP, 0);
    }
    else
    {
        setchr8(chrGroupBase | (chrInner & (chrGroupBanks - 1u)));
    }

    setmirror((M5003Regs[2] & 0x08) ? MI_H : MI_V);
}

static DECLFW(Mapper5003_WriteReg)
{
    // The lock applies only to the multicart's board-level configuration.
    if (M5003Regs[2] & 0x80)
        return;

    switch (A & 7)
    {
    case 0: M5003Regs[0] = V & 0x3F; break;
    case 1: M5003Regs[1] = V & 0x7F; break;
    case 2:
        M5003Regs[2] = V & 0xBF; // bit 6 reserved; mode in bits 5:4
        M5003GameLatch = 0;
        break;
    case 3: M5003Regs[3] = V & 0x1F; break;
    case 4: M5003Regs[4] = V & 0x0F; break;
    case 5:
        M5003Regs[5] = V & 7;
        if (M5003Regs[5] > 4)
            M5003Regs[5] = 4;
        break;
    case 6: M5003Regs[6] = V & 1; break;
    default: return;
    }
    Mapper5003_Sync();
}

static DECLFW(Mapper5003_WriteGame)
{
    (void)A;
    // Game writes still work after $5002 bit7 locks board configuration.
    M5003GameLatch = V;
    Mapper5003_Sync();
}

static void Mapper5003_Reset(void)
{
    for (int i = 0; i < 7; ++i)
        M5003Regs[i] = 0;
    M5003Regs[2] = 0x0C; // v1.3 default: NROM-256, H mirroring.
    M5003GameLatch = 0;

    SetReadHandler(0x8000, 0xFFFF, CartBR);
    SetWriteHandler(0x5000, 0x5FFF, Mapper5003_WriteReg);
    SetWriteHandler(0x8000, 0xFFFF, Mapper5003_WriteGame);
    Mapper5003_Sync();
}

static void Mapper5003_Power(void)
{
    // Real power-cycle resets volatile CHR-RAM; soft reset retains it.
    memset(M5003CHRRAM, 0, sizeof(M5003CHRRAM));
    Mapper5003_Reset();
}

static void Mapper5003_StateRestore(int version)
{
    (void)version;
    Mapper5003_Sync();
}

static void Mapper5003_Init(CartInfo* info)
{
    // Using a separate CHR chip index allows simultaneous CHR-ROM and RAM
    // in a multi-game image. Slot 0 stays mapped to the image's CHR-ROM.
    SetupCartCHRMapping(YHC_M3_CHRRAM_CHIP,
        M5003CHRRAM, sizeof(M5003CHRRAM), 1);

    info->Power = Mapper5003_Power;
    info->Reset = Mapper5003_Reset;
    GameStateRestore = Mapper5003_StateRestore;
    AddExState(M5003StateRegs, ~0, 0, NULL);
}


/*
 * Mapper 500.4: MMC1 multicart extension, using the existing ASIC_MMC1.
 *
 * The MMC1 core owns: serial register writes at $8000-$FFFF, 2-cycle
 * write filter, PRG/CHR registers, WRAM gating, mirroring, savestates.
 * This board owns: game selection and aligned PRG/CHR segments.
 *
 * BOARD REGISTERS (only when bit 7 of $5002 is clear):
 * $5000: PRG segment base, low 6 bits, unit = 16 KiB
 * $5001: CHR segment base, low 7 bits, unit =  8 KiB
 * $5002: [L R PP SCCC]
 *        L: lock board registers (not MMC1 game writes)
 *        R: use 8 KiB CHR-RAM instead of CHR-ROM
 *        PP: PRG segment size selector (bits 5:4)
 *        S: small-PRG extension (bit 3, only PP=0 or PP=1)
 *           PP=0, S=0: 128 KiB; PP=1, S=0: 256 KiB
 *           PP=2: 512 KiB; PP=3: legacy 512 KiB fallback
 *           PP=0, S=1:  32 KiB; PP=1, S=1:  64 KiB
 *        CCC: CHR-ROM segment size 0=8KiB, 1=16KiB, 2=32KiB,
 *             3=64KiB, 4=128KiB; ignored when R=1
 * $5003: PRG segment base bits 6..10 (32 MiB / 16 KiB = 2048)
 * $5004: CHR segment base bits 7..10 (16 MiB /  8 KiB = 2048)
 * $5005: bit0 = reset MMC1 registers; bit1 = clear CHR-RAM;
 *        bit2 = clear WRAM (write-only commands, executed before reset)
 * $5006..$5007: reserved
 *
 * 512KiB PRG mode emulates SUROM-style A18 control through CHR
 * register 0 bit4. It is intended for MMC1/CHR-RAM games; normal
 * CHR-ROM MMC1 games should use <=256KiB PRG groups.
 *
 * v1.8 Small-PRG extension: S=1 with PP=0/1 selects 32/64 KiB.
 * PP=2/3 keeps the existing 512 KiB meaning and ignores S.
 * Older builders should write bit3=0, as it was previously reserved.
 * Low-capacity games may have larger CHR than PRG (e.g. 32+128 KiB).
 * PRG and CHR block alignment are independent.
 *
 * Packing convention: PRG base is aligned to group length in 16KiB
 * banks; CHR base is aligned to CHR group length in 4KiB banks.
 * The packer MUST fill alignment gaps. Software masks cannot repair
 * incorrectly packed ROM content.
 *
 * Physical layout: PRG bank 0..7  = 128 KiB menu PRG;
 *                  CHR bank 0..15 = 128 KiB menu CHR.
 * Game PRG and CHR start at or above offset 128 KiB of their own
 * separate ROM areas, aligned to each selected game's group size.
 * On reset default maps 128 KiB PRG + 128 KiB CHR menu (not 256 KiB).
 * $5005 can clear RAM and reset MMC1, before jumping into selected game.
 * The menu must configure the board from CPU RAM, since writes to
 * $5000..$5004 immediately re-map PRG and may unmap its own code.
 */
static uint8 M5004Regs[5];
static uint8 M5004WRAM[8192];
static uint8 M5004CHRRAM[8192];

enum {
    YHC_M4_RAM_CHIP = 0x10,
    YHC_M4_DEFAULT_CONTROL = 0x04 // 128 KiB PRG + 128 KiB CHR-ROM menu
};

static SFORMAT M5004StateRegs[] = {
    { M5004Regs,   5, "M4RG" },
    { M5004WRAM,8192, "M4WR" },
    { M5004CHRRAM,8192, "M4CR" },
    { 0 }
};

static uint32 Mapper5004_PRGBase16(void)
{
    return ((uint32)(M5004Regs[3] & 0x1F) << 6) |
        (uint32)(M5004Regs[0] & 0x3F);
}

static uint32 Mapper5004_CHRBase8(void)
{
    return ((uint32)(M5004Regs[4] & 0x0F) << 7) |
        (uint32)(M5004Regs[1] & 0x7F);
}

static void Mapper5004_Sync(void)
{
    // MMC1's $6000-$7FFF WRAM enable/disable is handled by the ASIC.
    MMC1_syncWRAM(0);
    MMC1_syncMirror();

    // Board PRG group size, in 16 KiB banks.  This affects OUTER
    // multicart isolation only; serial MMC1 game writes remain unchanged.
    // PP=0/1 + S=1 is a new opt-in 32/64 KiB grouping mode.
    // PP=2/3 continues to mean 512 KiB, even when S is set.
    const uint32 prgCode = (M5004Regs[2] >> 4) & 3u;
    const bool smallPRG = (M5004Regs[2] & 0x08) != 0;
    const uint32 prgGroup16 = (smallPRG && prgCode < 2u)
        ? (2u << prgCode)                         // 32 / 64 KiB
        : (8u << (prgCode > 2u ? 2u : prgCode)); // 128 / 256 / 512 KiB
    const uint32 prgBase16 = Mapper5004_PRGBase16() & ~(prgGroup16 - 1u);
    const bool chrRAM = (M5004Regs[2] & 0x40) != 0;

    uint32 prgHi16 = 0;
    if (prgGroup16 == 32 && chrRAM)
    {
        // SUROM-style 512 KiB: CHR bank 0 bit4 selects PRG A18.
        // Both 16 KiB PRG windows remain inside the selected half.
        prgHi16 = (uint32)(MMC1_getCHRBank(0) & 0x10);
    }

    for (uint32 bank = 0; bank < 2; ++bank)
    {
        uint32 inner16 = (uint32)MMC1_getPRGBank((uint8)bank);
        inner16 |= prgHi16;
        const uint32 address = bank ? 0xC000 : 0x8000;
        setprg16(address, prgBase16 | (inner16 & (prgGroup16 - 1u)));
    }

    if (chrRAM)
    {
        // Dedicated CHR RAM alongside the multicart's CHR-ROM.
        // The ASIC selects 4 KiB windows, so RAM can be 4K/8K switched.
        setchr4r(YHC_M4_RAM_CHIP, 0x0000, MMC1_getCHRBank(0) & 1);
        setchr4r(YHC_M4_RAM_CHIP, 0x1000, MMC1_getCHRBank(1) & 1);
    }
    else
    {
        uint32 chrCode = M5004Regs[2] & 7;
        if (chrCode > 4)
            chrCode = 4;
        const uint32 chrGroup4 = 2u << chrCode; // 8-128 KiB
        const uint32 chrBase4 = (Mapper5004_CHRBase8() << 1) & ~(chrGroup4 - 1u);
        setchr4(0x0000, chrBase4 | ((uint32)MMC1_getCHRBank(0) & (chrGroup4 - 1u)));
        setchr4(0x1000, chrBase4 | ((uint32)MMC1_getCHRBank(1) & (chrGroup4 - 1u)));
    }
}

static DECLFW(Mapper5004_WriteReg)
{
    // The board lock never blocks MMC1's writes at $8000-$FFFF.
    if (M5004Regs[2] & 0x80)
        return;

    switch (A & 7)
    {
    case 0: M5004Regs[0] = V & 0x3F; break;
    case 1: M5004Regs[1] = V & 0x7F; break;
    case 2:
        // Retain bit3 only for PP=0/1 (new 32/64 KiB group opt-in).
        // In PP=2/3 the old 512 KiB behavior is unchanged.
        M5004Regs[2] = (V & 0xF7) |
            (((V & 0x30) <= 0x10) ? (V & 0x08) : 0);
        break;
    case 3: M5004Regs[3] = V & 0x1F; break;
    case 4: M5004Regs[4] = V & 0x0F; break;
    case 5:
        // Selective reset/clear commands; no RAM is cleared merely
        // because CHR-ROM/CHR-RAM mode changes.
        if (V & 0x02)
            memset(M5004CHRRAM, 0, sizeof(M5004CHRRAM));
        if (V & 0x04)
            memset(M5004WRAM, 0, sizeof(M5004WRAM));
        if (V & 0x01)
            MMC1_clear(); // calls Mapper5004_Sync through the ASIC
        return;
    default: return;
    }
    Mapper5004_Sync();
}

static void Mapper5004_ResetBoard(void)
{
    memset(M5004Regs, 0, sizeof(M5004Regs));
    M5004Regs[2] = YHC_M4_DEFAULT_CONTROL;

    // Restores the MMC1 game handler, WRAM gating and CPU-cycle filter.
    MMC1_power();
    SetWriteHandler(0x5000, 0x5FFF, Mapper5004_WriteReg);
}

static void Mapper5004_Power(void)
{
    memset(M5004WRAM, 0, sizeof(M5004WRAM));
    memset(M5004CHRRAM, 0, sizeof(M5004CHRRAM));
    Mapper5004_ResetBoard();
}

static void Mapper5004_Restore(int version)
{
    // The ASIC's saved registers and our board state have been loaded.
    MMC1_restore(version);
}

static void Mapper5004_Init(CartInfo* info)
{
    SetupCartPRGMapping(YHC_M4_RAM_CHIP,
        M5004WRAM, sizeof(M5004WRAM), 1);
    SetupCartCHRMapping(YHC_M4_RAM_CHIP,
        M5004CHRRAM, sizeof(M5004CHRRAM), 1);

    // Delegate all MMC1 serial timing, write handlers and internal state
    // to the user's existing ASIC module. Only Sync is board-specific.
    MMC1_init(info, Mapper5004_Sync, MMC1_TYPE_MMC1B,
        NULL, NULL, NULL, NULL);

    info->Power = Mapper5004_Power;
    info->Reset = Mapper5004_ResetBoard;
    GameStateRestore = Mapper5004_Restore;
    AddExState(M5004StateRegs, ~0, 0, NULL);
}


/*
 * Mapper 500.5: MMC3 TxROM / TxSROM (118) / TQROM (119) / Mapper 74 (v2.1)
 *
 * MMC3 ASIC is reused without changes. Modes 74, 118 and 119 are board-level
 * choices implemented by this file. MMC6, 191/192/194/195 and external
 * four-screen VRAM remain OUT OF SCOPE.
 *
 * Entire multicart capacities: 64 MiB PRG-ROM + 64 MiB CHR-ROM.
 * Separate PRG and CHR ROM areas begin with a 128 KiB menu each.
 * Each game's PRG and CHR blocks may have independent aligned sizes.
 *
 * $5000: PRG base low 6 bits, in 16 KiB units
 * $5001: CHR base low 7 bits, in  8 KiB units
 * $5002: [L R PP S CCC]
 *         L    board register lock (MMC3's internal writes stay live)
 *         R    1: dedicated 8 KiB CHR-RAM; 0: CHR-ROM
 *         PP   0=128, 1=256, 2=512, 3=64 KiB PRG group
 *         S    only when PP=3: 1=32 KiB PRG group, 0=64 KiB
 *              (reserved/ignored for PP=0,1,2; backward compatible)
 *         CCC  0=8, 1=16, 2=32, 3=64, 4=128, 5=256 KiB CHR group
 * $5003: PRG base high 5 bits
 * $5004: CHR base high 4 bits
 * $5005: write-only command: bit0 clear MMC3 registers/IRQ;
 *         bit1 clear CHR-RAM; bit2 clear WRAM
 * $5006: write board MMC3 variant: 0=TxROM (old default),
 *         1=TxSROM / Mapper 118, 2=TQROM / Mapper 119,
 *         3=Waixing MMC3 / Mapper 74 (CHR banks 8/9 => 2KiB CHR-RAM).
 *         Only 0..3 recognized. The board lock ($5002.7) also locks $5006.
 * $5007: [.... G CCP] - new extension; write 0 for v2.0 behavior.
 *         P (bit0): PRG base bit 11 (unit 16 KiB; upper 32 MiB)
 *         CC (bits2:1): CHR base bits 12:11 (unit 8 KiB; 16/32/48 MiB)
 *         G (bit3): 1=1024 KiB PRG group, overrides $5002 PP/S;
 *                  0=previous 32/64/128/256/512 KiB groups.
 *         bits7:4 reserved, ignored on writes.
 *
 * For TxSROM, CHR A17 is the CIRAM A10 input: the MMC3 CHR bank bit7
 * selects one-screen lower/upper per PPU CHR fetch. CHR bank numbers
 * use only bits 0..6 (up to 128 KiB per game). $A000 mirroring ignored.
 * For TQROM, CHR bank bit6 selects 8 KiB writable CHR-RAM (1KiB windows)
 * when set; if clear, CHR bits 0..5 select ROM (up to 64 KiB per game).
 * Mapper 74 replaces CHR-ROM banks #8 and #9 with two writable 1KiB
 * pages of dedicated CHR-RAM. Other bank indices still address CHR-ROM.
 * In these modes the whole-game $5002.6 RAM selector is ignored.
 * PRG games are mapped using four MMC3 8 KiB windows, while CHR
 * games use eight MMC3 1 KiB windows.  The selected game is confined
 * to its independently aligned PRG/CHR group even when MMC3 R6/R7,
 * CHR registers, PRG mode, or CHR inversion are changed.
 *
 * MENU: board reset maps PRG group 0 (128 KiB) and CHR group 0
 * (128 KiB CHR-ROM).  Writes to board configuration immediately
 * change the mapping, so the menu's game launch must use a routine
 * copied into CPU INTERNAL RAM (not $6000 WRAM or current PRG-ROM).
 * IMPORTANT: On CPU Reset, $FFFC-$FFFD must be the reset vector stored
 * in menu PRG bank 15 (PRG offset 0x1FFFC). The board switches back
 * to menu banks BEFORE the emulator fetches this CPU reset vector.
 * This ROM image requirement cannot be fixed by changing the CPU core.
 */
enum {
    YHC_M5_RAM_CHIP = 0x10,
    YHC_M5_MENU_CONTROL = 0x04, // 128KiB PRG + 128KiB CHR-ROM
    YHC_M5_TXROM = 0,
    YHC_M5_TXSROM = 1,
    YHC_M5_TQROM = 2,
    YHC_M5_MAPPER74 = 3
};

static uint8 M5005Regs[5];
static uint8 M5005Mode;
static uint8 M5005Extra; // $5007: high PRG/CHR base bits + 1024KiB group enable
static uint8 M5005TxSLastSlot;
static uint8 M5005WRAM[8192];
static uint8 M5005CHRRAM[8192];

static SFORMAT M5005StateRegs[] = {
    { M5005Regs,      5, "M5RG" },
    { &M5005Mode,      1, "M5MD" },
    { &M5005Extra,     1, "M5EX" },
    { &M5005TxSLastSlot, 1, "M5SL" },
    { M5005WRAM,   8192, "M5WR" },
    { M5005CHRRAM,8192, "M5CR" },
    { 0 }
};

static uint32 Mapper5005_PRGBase16(void)
{
    // 64 MiB PRG-ROM / 16 KiB = 4096 possible base banks (12 bits).
    return ((uint32)(M5005Extra & 0x01) << 11) |
        ((uint32)(M5005Regs[3] & 0x1F) << 6) |
        (uint32)(M5005Regs[0] & 0x3F);
}

static uint32 Mapper5005_CHRBase8(void)
{
    // 64 MiB CHR-ROM / 8 KiB = 8192 possible base banks (13 bits).
    return ((uint32)((M5005Extra >> 1) & 0x03) << 11) |
        ((uint32)(M5005Regs[4] & 0x0F) << 7) |
        (uint32)(M5005Regs[1] & 0x7F);
}

// Mapper 118 / TxSROM: the CHR-bank A17 line is wired to CIRAM A10.
// This must follow PPU CHR/nametable accesses, not the global $A000 bit.
// FCEUX's original TxSROM implementation uses the same PPU_hook approach.
// $2000-$2FFF mirrors to $0000-$0FFF for the MMC3's CHR A17 logic.
static void Mapper5005_TxSROMPPUHook(uint32 A)
{
    const uint8 slot = (uint8)((A & 0x1FFFu) >> 10);
    M5005TxSLastSlot = slot;
    const uint8 nt = (uint8)((MMC3_getCHRBank(slot) >> 7) & 1);
    setmirror(nt ? MI_1 : MI_0);
}

static void Mapper5005_Sync(void)
{
    // PRG base is 16 KiB; MMC3's actual switching unit is 8 KiB.
    const uint32 prgCode = (M5005Regs[2] >> 4) & 3;
    // Backward-compatible v1.7 extension: PP=3 + bit3 -> 32 KiB.
    // PP=3 without bit3 stays at the v1.6 64 KiB group size.
    // Four 8 KiB MMC3 windows fit a 32 KiB PRG group exactly.
    const uint32 prgGroup8 = (M5005Extra & 0x08)
        ? 128u // 1024 KiB / 8 KiB; 1 MiB extended board group.
        : prgCode == 3
        ? ((M5005Regs[2] & 0x08) ? 4u : 8u)
        : (16u << prgCode);
    const uint32 prgBase8 = (Mapper5005_PRGBase16() << 1) &
        ~(prgGroup8 - 1u);

    // The normal MMC3 has two switchable 8 KiB windows and two fixed
    // 8 KiB windows, including the PRG-mode inversion from $8000.bit6.
    for (uint32 i = 0; i < 4; ++i)
    {
        const uint32 inner8 = (uint32)MMC3_getPRGBank((uint8)i);
        setprg8(0x8000u + (i << 13),
            prgBase8 | (inner8 & (prgGroup8 - 1u)));
    }

    // The old full-8KiB CHR-RAM mode is only meaningful for TxROM.
    // 118 uses CHR bit7 for nametable; 119 uses bit6 for ROM/RAM;
    // 74 uses only CHR bank numbers #8 and #9 for 2KiB RAM.
    if (M5005Mode == YHC_M5_TXROM && (M5005Regs[2] & 0x40))
    {
        for (uint32 i = 0; i < 8; ++i)
        {
            const uint32 inner1 = (uint32)MMC3_getCHRBank((uint8)i);
            setchr1r(YHC_M5_RAM_CHIP, i << 10, inner1 & 7u);
        }
    }
    else
    {
        uint32 chrCode = M5005Regs[2] & 7u;
        // TxSROM only has seven CHR bank bits, and TQROM uses bit6
        // for RAM selection. Their per-game ROM maxima are 128/64KiB.
        const uint32 maxCode = M5005Mode == YHC_M5_TXSROM ? 4u :
            M5005Mode == YHC_M5_TQROM ? 3u : 5u;
        if (chrCode > maxCode)
            chrCode = maxCode;
        const uint32 chrGroup1 = 8u << chrCode;
        const uint32 chrBase1 = (Mapper5005_CHRBase8() << 3) &
            ~(chrGroup1 - 1u);

        for (uint32 i = 0; i < 8; ++i)
        {
            const uint32 inner1 = (uint32)MMC3_getCHRBank((uint8)i);
            if (M5005Mode == YHC_M5_TQROM && (inner1 & 0x40u))
            {
                // Mapper 119: use bit6 to select the 8KiB CHR-RAM.
                setchr1r(YHC_M5_RAM_CHIP, i << 10, inner1 & 7u);
            }
            else if (M5005Mode == YHC_M5_MAPPER74 &&
                (inner1 == 8u || inner1 == 9u))
            {
                // Mapper 74 / Waixing: exactly CHR bank 8 and 9
                // are redirected to the first two 1KiB CHR-RAM pages.
                // Test the *raw* ASIC bank number, BEFORE ROM masking.
                // Unlike TQROM, bit6 is not a RAM enable signal.
                setchr1r(YHC_M5_RAM_CHIP, i << 10, inner1 - 8u);
            }
            else
            {
                // TxSROM strips bit7, TQROM strips bit6; Mapper 74
                // retains the full CHR-ROM bank number for all other pages.
                const uint32 effective = M5005Mode == YHC_M5_TXSROM
                    ? (inner1 & 0x7Fu)
                    : M5005Mode == YHC_M5_TQROM
                    ? (inner1 & 0x3Fu) : inner1;
                setchr1(i << 10, chrBase1 | (effective & (chrGroup1 - 1u)));
            }
        }
    }

    if (M5005Mode == YHC_M5_TXSROM)
    {
        // Do not call MMC3_syncMirror here: $A000 has no control over
        // nametable mapping on TxSROM. Update the hook on each Sync so
        // it also survives save-state restores and register writes.
        PPU_hook = Mapper5005_TxSROMPPUHook;
        const uint8 slot = M5005TxSLastSlot & 7u;
        const uint8 nt = (uint8)((MMC3_getCHRBank(slot) >> 7) & 1);
        setmirror(nt ? MI_1 : MI_0);
    }
    else
    {
        PPU_hook = NULL;
        MMC3_syncMirror();
    }
    MMC3_syncWRAM(0);
}

static DECLFW(Mapper5005_WriteReg)
{
    // Lock protects the multicart's OUTER banks, never MMC3 game writes.
    if (M5005Regs[2] & 0x80)
        return;

    switch (A & 7)
    {
    case 0: M5005Regs[0] = V & 0x3F; break;
    case 1: M5005Regs[1] = V & 0x7F; break;
    case 2:
        // Bit 3 is newly assigned only if PP=3; keep it ignored
        // in all other modes to preserve the v1.6 register protocol.
        M5005Regs[2] = (V & 0xF7) |
            ((V & 0x38) == 0x38 ? 0x08 : 0);
        break;
    case 3: M5005Regs[3] = V & 0x1F; break;
    case 4: M5005Regs[4] = V & 0x0F; break;
    case 5:
        // These are explicit commands: changing CHR type itself never
        // destroys a previous game's RAM contents.
        if (V & 0x02)
            memset(M5005CHRRAM, 0, sizeof(M5005CHRRAM));
        if (V & 0x04)
            memset(M5005WRAM, 0, sizeof(M5005WRAM));
        if (V & 0x01)
        {
            // MMC3_clear alone does not deassert a pending CPU IRQ.
            X6502_IRQEnd(FCEU_IQEXT);
            MMC3_clear(); // Re-syncs mappings through our callback.
        }
        return;
    case 6:
        // Board-specific ASIC variant selected by the multicart menu.
        // Reset the last CHR bus slot to a deterministic value.
        if (V > YHC_M5_MAPPER74)
            return; // reject undefined variants, do not silently alias.
        M5005Mode = V;
        M5005TxSLastSlot = 0;
        break;
    case 7:
        // External PRG A25, CHR A24/A25 and extended 1MiB group.
        // Registers remain writable until $5002 bit7 locks selection.
        M5005Extra = V & 0x0F;
        break;
    default:
        return;
    }
    Mapper5005_Sync();
}

static void Mapper5005_ResetBoard(void)
{
    // FCEUX calls CartInfo::Reset before X6502_Reset, so the
    // hardware menu bank mapping MUST be restored BEFORE the CPU
    // reads its reset vector at $FFFC/$FFFD.
    memset(M5005Regs, 0, sizeof(M5005Regs));
    M5005Regs[2] = YHC_M5_MENU_CONTROL;
    M5005Mode = YHC_M5_TXROM;
    M5005Extra = 0; // clear large-ROM/high-bank extension on soft reset
    M5005TxSLastSlot = 0;
    PPU_hook = NULL; // disable any previous TxSROM callback on soft reset

    // Disable a pending IRQ and restore the MMC3 register defaults,
    // including R6=0, R7=1 and its fixed last bank at $E000.
    X6502_IRQEnd(FCEU_IQEXT);
    MMC3_power();
    SetWriteHandler(0x5000, 0x5FFF, Mapper5005_WriteReg);
    // Explicit final sync documents and reinforces the soft-reset
    // invariant: $8000=bank0, $A000=bank1, $C000=bank14,
    // $E000=bank15 (inside the first 128 KiB menu PRG group).
    Mapper5005_Sync();
    // Do NOT clear WRAM/CHR-RAM on soft reset. The menu init code
    // is responsible for its own RAM state after reading the vector.
}

static void Mapper5005_Power(void)
{
    memset(M5005WRAM, 0, sizeof(M5005WRAM));
    memset(M5005CHRRAM, 0, sizeof(M5005CHRRAM));
    Mapper5005_ResetBoard();
}

static void Mapper5005_Restore(int version)
{
    MMC3_restore(version);
}

static void Mapper5005_Init(CartInfo* info)
{
    SetupCartPRGMapping(YHC_M5_RAM_CHIP,
        M5005WRAM, sizeof(M5005WRAM), 1);
    SetupCartCHRMapping(YHC_M5_RAM_CHIP,
        M5005CHRRAM, sizeof(M5005CHRRAM), 1);

    // ASIC manages R0-R7, $8000/$8001/$A000/$A001,
    // IRQ counter, PRG-RAM gating, all MMC3 save state fields.
    MMC3_init(info, Mapper5005_Sync, MMC3_TYPE_SHARP,
        NULL, NULL, NULL, NULL);

    info->Power = Mapper5005_Power;
    info->Reset = Mapper5005_ResetBoard;
    GameStateRestore = Mapper5005_Restore;
    AddExState(M5005StateRegs, ~0, 0, NULL);
}


/*
 * Mapper 500.6: Yhc MMC5 / ExROM multicart (v2.2)
 *
 * Dedicated board registers at $4800-$4806. These addresses avoid the
 * normal MMC5 registers ($5000-$520A) and ExRAM ($5C00-$5FFF).
 * On COMMIT the selected bank changes IMMEDIATELY. The menu MUST execute
 * its selection/jump trampoline from CPU internal RAM ($0000-$07FF), not
 * from the PRG-ROM window that is being replaced.
 *
 * $4800 PRG base bits  7:0, units 16KiB (0..4095)
 * $4801 PRG base bits 11:8 (low nibble only)
 * $4802 CHR base bits  7:0, units 8KiB (0..8191)
 * $4803 CHR base bits 12:8 (low 5 bits only)
 * $4804 [PPPP CCCC]: PRG size 0..5 = 32/64/128/256/512/1024KiB
 *                      CHR size 0..7 = 8/16/32/64/128/256/512/1024KiB
 * $4805 [LRWWW...]: bit7=lock after successful COMMIT,
 *                   bit6=CHR-RAM (uses dedicated 8KiB RAM),
 *                   bits5:3=WRAM size code 0/1/2/3/4=0/8/16/32/64KiB
 * $4806 WRITE: bit0=COMMIT+reset ASIC; bit1=clear CHR-RAM;
 *              bit2=clear all 64KiB shared WRAM before game entry.
 *       READ: bit0=last COMMIT success; bit1=board locked;
 *             bit7=invalid COMMIT detected.
 * All other register bits are reserved and ignored on writes.
 *
 * Menu occupies first 128KiB of PRG AND first 128KiB of CHR independently.
 * ASIC MMC5 reset ROM selection maps bank 15 into all CPU PRG windows;
 * menu reset vector must be at PRG file offset 0x1FFFC/0x1FFFD.
 * Reset via FCEUX Reset returns to menu (not an in-game reset).
 *
 * WRAM is one shared 64KiB allocation, not isolated by game; menu is
 * responsible for RAM persistence/isolation and any SRAM organization.
 * The ROM loader must load the full NES2 64+64MiB sizes; these registers
 * only perform mapping, not ROM allocation or ROM code conversion.
 */
static uint8 M5006Regs[6];
static uint8 M5006Locked;
static uint8 M5006Status;
static const uint32 YHC_M6_MENU_PRG_BANKS8 = 16u;
static const uint32 YHC_M6_MENU_CHR_BANKS1 = 128u;

static SFORMAT M5006StateRegs[] = {
    { M5006Regs, 6, "M6RG" },
    { &M5006Locked, 1, "M6LK" },
    { &M5006Status, 1, "M6ST" },
    { 0 }
};

static bool Mapper5006_Decode(uint32* prgBase8, uint32* prgBanks8,
    uint32* chrBase1, uint32* chrBanks1,
    bool* chrRAM, int* wramKiB)
{
    const uint8 pc = M5006Regs[4] >> 4;
    const uint8 cc = M5006Regs[4] & 0x0F;
    const uint8 wc = (M5006Regs[5] >> 3) & 7u;
    if (pc > 5u || cc > 7u || wc > 4u)
        return false;

    const uint32 pb16 = (uint32)M5006Regs[0] |
        ((uint32)(M5006Regs[1] & 0x0F) << 8);
    const uint32 cb8 = (uint32)M5006Regs[2] |
        ((uint32)(M5006Regs[3] & 0x1F) << 8);
    const uint32 pg16 = 2u << pc; // 32..1024KiB, in 16KiB banks
    const uint32 cg8 = 1u << cc; // 8..1024KiB, in 8KiB banks

    // Reject rather than silently align or wrap: the Builder must pack
    // the physical PRG and CHR groups independently.
    if ((pb16 & (pg16 - 1u)) || pb16 > 4096u - pg16 ||
        (cb8 & (cg8 - 1u)) || cb8 > 8192u - cg8)
        return false;
    // The first 128KiB of both ROM regions belongs to the MENU.
    // CHR-RAM-only games do not consume a CHR-ROM segment.
    if (pb16 < 8u || (!(M5006Regs[5] & 0x40u) && cb8 < 16u))
        return false;

    *prgBase8 = pb16 * 2u;
    *prgBanks8 = pg16 * 2u;
    *chrRAM = (M5006Regs[5] & 0x40) != 0;
    *chrBase1 = *chrRAM ? 0u : cb8 * 8u;
    *chrBanks1 = *chrRAM ? 8u : cg8 * 8u;
    static const int wramTable[5] = { 0, 8, 16, 32, 64 };
    *wramKiB = wramTable[wc];
    return true;
}

static void Mapper5006_InstallBoardHandlers(void);

static DECLFR(Mapper5006_ReadReg)
{
    if ((A & 7u) <= 5u)
        return M5006Regs[A & 7u];
    if ((A & 7u) == 6u)
        return (M5006Status & 0x81u) | (M5006Locked ? 0x02u : 0u);
    return X.DB;
}

static DECLFW(Mapper5006_WriteReg)
{
    if (M5006Locked)
        return;
    const uint8 which = (uint8)(A & 7u);
    if (which <= 5u)
    {
        static const uint8 mask[6] = { 0xFF, 0x0F, 0xFF, 0x1F, 0xFF, 0xF8 };
        M5006Regs[which] = V & mask[which];
        return;
    }
    if (which != 6u || !(V & 0x01))
        return;

    uint32 pb = 0, ps = 0, cb = 0, cs = 0;
    int wramKiB = 0;
    bool ram = false;
    if (!Mapper5006_Decode(&pb, &ps, &cb, &cs, &ram, &wramKiB))
    {
        M5006Status = 0x80u;
        return;
    }

    // Both routines validate the chosen memory arrangement; no changes
    // to ASIC mapper state occur on a rejected board configuration.
    if (!ASIC_MMC5_SelectGame(pb, ps, cb, cs, ram))
    {
        M5006Status = 0x80u;
        return;
    }
    // The physical backing was preallocated to 64KiB in Init, and
    // Decode already validated the only five allowed visible sizes.
    if (!ASIC_MMC5_SetWRAMKiB(wramKiB))
    {
        M5006Status = 0x80u;
        return;
    }
    if (V & 0x02) ASIC_MMC5_ClearCHRRAM();
    if (V & 0x04) ASIC_MMC5_ClearWRAM();

    // Hardware reset of MMC5 internals after changing its outer mask
    // clears stale IRQ, expansion sound, CHR/NT/ExRAM control registers.
    // It preserves actual WRAM/CHR-RAM data unless a clear was requested.
    ASIC_MMC5_Reset();
    M5006Status = 1u;
    M5006Locked = (M5006Regs[5] & 0x80u) ? 1u : 0u;
    Mapper5006_InstallBoardHandlers();
}

static void Mapper5006_InstallBoardHandlers(void)
{
    // ASIC_MMC5_Reset() resets the standard MMC5 handlers in
    // $4020-$5BFF; restore our non-conflicting board-register window.
    SetWriteHandler(0x4800, 0x4806, Mapper5006_WriteReg);
    SetReadHandler(0x4800, 0x4806, Mapper5006_ReadReg);
}

static void Mapper5006_ResetBoard(bool power)
{
    memset(M5006Regs, 0, sizeof(M5006Regs));
    M5006Regs[4] = 0x24u; // 128KiB menu PRG, 128KiB menu CHR
    M5006Regs[5] = 0x08u; // default menu visible WRAM: 8KiB
    M5006Locked = 0;
    M5006Status = 1u;
    // The reset vector is fetched only AFTER this CartInfo callback.
    ASIC_MMC5_SelectGame(0u, YHC_M6_MENU_PRG_BANKS8,
        0u, YHC_M6_MENU_CHR_BANKS1, false);
    if (power) ASIC_MMC5_Power();
    else ASIC_MMC5_Reset();
    ASIC_MMC5_SetWRAMKiB(8);
    Mapper5006_InstallBoardHandlers();
}

static void Mapper5006_Power(void) { Mapper5006_ResetBoard(true); }
static void Mapper5006_Reset(void) { Mapper5006_ResetBoard(false); }

static void Mapper5006_Restore(int version)
{
    ASIC_MMC5_Restore(version);
    Mapper5006_InstallBoardHandlers();
}

static void Mapper5006_Init(CartInfo* info)
{
    // A single 64KiB backing buffer serves all MMC5 games; the visible
    // WRAM capacity is selected independently for each one.
    ASIC_MMC5_Config cfg = { 0u, YHC_M6_MENU_PRG_BANKS8,
                            0u, YHC_M6_MENU_CHR_BANKS1,
                            64, (uint8)(info->battery != 0), 0 };
    ASIC_MMC5_Init(info, &cfg);
    info->Power = Mapper5006_Power;
    info->Reset = Mapper5006_Reset;
    GameStateRestore = Mapper5006_Restore;
    AddExState(M5006StateRegs, ~0, 0, NULL);
}


/*
 * Mapper 500.7 Hybrid ASIC controller — VRC compatibility stage (v2.4).
 * Game profiles: 0=UxROM,1=CNROM,2=MMC1,3=TxROM,4=TxSROM,
 * 5=TQROM,6=Mapper74,7=VRC1,8=VRC2,9=VRC3,10=VRC4,
 * 11=VRC6,12=VRC7,13=NROM.
 * Menu cores: 3=MMC3, 11=VRC6, 12=VRC7, one shared 128+128KiB
 * menu region. Submapper 0..6 source and handlers are untouched.
 *
 * Board registers $4800-$4808 (write-only pending config before commit):
 * 0,1 PRG base (16KiB unit, 12-bit); 2,3 CHR base (8KiB, 13-bit)
 * 4 [PPPP CCCC] PRG size: 0..5=32..1024KiB, 6=16KiB;
 *                 CHR size: 0..7=8..1024KiB
 * 5 [L R WWW 00B] lock, RAM, WRAM, bus-conflict; WWW: 0=0K,1=8K,2=2K VRC4 mirror
 * 6 commands: bit0 game COMMIT, bit4 menu-core COMMIT;
 *             bit1 clear CHR-RAM, bit2 clear WRAM (with game COMMIT).
 * 7 [TTTT PPPP] ASIC/profile; 8 [AAAA BBBB] VRC A1/A0 CPU address bits.
 * READ $4806: bit0 valid last commit, bit1 locked, bit2 game running,
 *             bit7 last commit failed. Other registers read pending values.
 *
 * REQUIRES CPU INTERNAL-RAM trampoline for actual game switching.
 * Commit changes PRG mapping immediately; it does NOT reset the CPU PC.
 */
enum {
    YHC_M7_UNROM = 0, YHC_M7_CNROM = 1, YHC_M7_MMC1 = 2,
    YHC_M7_MMC3 = 3, YHC_M7_TXSROM = 4, YHC_M7_TQROM = 5,
    YHC_M7_M74 = 6, YHC_M7_VRC1 = 7, YHC_M7_VRC2 = 8,
    YHC_M7_VRC3 = 9, YHC_M7_VRC4 = 10, YHC_M7_VRC6 = 11,
    YHC_M7_VRC7 = 12, YHC_M7_NROM = 13,
    YHC_M7_CHRRAM_CHIP = 0x12, YHC_M7_WRAM_CHIP = 0x13
};
static uint8 M5007Regs[9], M5007Active, M5007Profile, M5007Menu;
static uint8 M5007Status, M5007Locked, M5007IsGame;
static uint8 M5007Latch, M5007TxSLast;
static uint8 M5007Pins, M5007MenuPins, M5007MenuProfile;
// Runtime-installed ASIC is distinct from the saved active ASIC: on
// SaveState load, saved state is overwritten BEFORE GameStateRestore.
static uint8 M5007Installed = 0xFF;
static uint8 M5007CHRRAM[8192], M5007WRAM[65536];
static uint32 M5007PRG8Base, M5007PRG8Count, M5007CHR1Base, M5007CHR1Count;
static uint8 M5007UseRAM, M5007UseWRAM, M5007BusConflict; // M5007UseWRAM stores visible KiB: 0/2/8
static uint8 M5007Battery; // Configuration, not SaveState; the entire 64KiB WRAM is shared.
static void Mapper5007_Sync(void);

static SFORMAT M5007State[] = {
    {M5007Regs, 9, "H7RG"}, {&M5007Active, 1, "H7AC"},
    {&M5007Profile, 1, "H7PF"}, {&M5007Menu, 1, "H7MN"},
    {&M5007Status, 1, "H7ST"}, {&M5007Locked, 1, "H7LK"},
    {&M5007IsGame, 1, "H7GM"}, {&M5007Latch, 1, "H7LT"},
    {&M5007TxSLast, 1, "H7TS"},
    {&M5007Pins, 1, "H7PN"},
    {&M5007MenuPins, 1, "H7MP"},
    {&M5007MenuProfile, 1, "H7MF"},
    {&M5007PRG8Base, 4 | FCEUSTATE_RLSB, "H7PB"},
    {&M5007PRG8Count, 4 | FCEUSTATE_RLSB, "H7PC"},
    {&M5007CHR1Base, 4 | FCEUSTATE_RLSB, "H7CB"},
    {&M5007CHR1Count, 4 | FCEUSTATE_RLSB, "H7CC"},
    {&M5007UseRAM, 1, "H7CR"}, {&M5007UseWRAM, 1, "H7WR"},
    {&M5007BusConflict, 1, "H7BC"},
    {M5007CHRRAM, 8192, "H7CM"}, {M5007WRAM, 65536, "H7WM"},
    {0}
};

// All game groups are powers of two. Physical addressing is ADD, not
// a global OR: it is valid only after our mandatory alignment checks.
static uint32 Mapper5007_PRG8(uint32 inner) {
    return M5007PRG8Base + (inner & (M5007PRG8Count - 1u));
}
static uint32 Mapper5007_CHR1(uint32 inner) {
    return M5007CHR1Base + (inner & (M5007CHR1Count - 1u));
}
static void Mapper5007_SetCHR1(uint32 slot, uint32 inner) {
    if (M5007UseRAM) setchr1r(YHC_M7_CHRRAM_CHIP, slot * 1024u, inner & 7u);
    else setchr1(slot * 1024u, Mapper5007_CHR1(inner));
}
static void Mapper5007_SyncDiscrete(void) {
    const uint8 type = M5007Active;
    if (type == YHC_M7_UNROM) {
        const uint32 mask = M5007Profile == 1 ? 15u : M5007Profile == 2 ? 31u : 7u;
        const uint32 low = (uint32)M5007Latch & mask;
        setprg16(0x8000, Mapper5007_PRG8(low * 2u) / 2u);
        setprg16(0xC000, Mapper5007_PRG8(mask * 2u) / 2u);
    }
    else if (M5007PRG8Count == 2u) {
        // 16KiB NROM-128/CNROM-128 mirrored into $8000-$FFFF.
        const uint32 b = M5007PRG8Base;
        setprg8(0x8000, b); setprg8(0xA000, b + 1);
        setprg8(0xC000, b); setprg8(0xE000, b + 1);
    }
    else {
        for (uint32 i = 0; i < 4; ++i)
            setprg8(0x8000 + i * 8192u, Mapper5007_PRG8(i));
    }
    // CNROM hardware has only two CHR select bits in the basic profile.
    const uint32 chrbank = type == YHC_M7_CNROM ? (M5007Latch & 3u) : 0u;
    for (uint32 i = 0; i < 8; ++i) Mapper5007_SetCHR1(i, chrbank * 8u + i);
    // $4808 bit0 is the discrete menu/game H/V mirroring selector.
    setmirror((M5007Pins & 1u) ? MI_H : MI_V);
}
static void Mapper5007_SyncMMC1(void) {
    MMC1_syncWRAM(0);
    for (uint32 i = 0; i < 2; ++i) {
        uint32 inner = (uint32)MMC1_getPRGBank((uint8)i) * 2u;
        if (M5007PRG8Count == 64u && M5007UseRAM)
            inner |= (uint32)(MMC1_getCHRBank(0) & 0x10) * 2u; // SUROM A18
        const uint32 b = Mapper5007_PRG8(inner);
        setprg8(0x8000 + i * 0x4000u, b);
        setprg8(0xA000 + i * 0x4000u, b + 1u);
    }
    for (uint32 i = 0; i < 2; ++i) {
        const uint32 inner4 = (uint32)MMC1_getCHRBank((uint8)i);
        for (uint32 j = 0; j < 4; ++j)
            Mapper5007_SetCHR1(i * 4u + j, inner4 * 4u + j);
    }
    MMC1_syncMirror();
}
static void Mapper5007_TxSROMPPU(uint32 A) {
    M5007TxSLast = (uint8)((A & 0x1FFFu) >> 10);
    setmirror((MMC3_getCHRBank(M5007TxSLast) & 0x80) ? MI_1 : MI_0);
}
static void Mapper5007_SyncMMC3(void) {
    for (uint32 i = 0; i < 4; ++i)
        setprg8(0x8000 + i * 8192u, Mapper5007_PRG8((uint32)MMC3_getPRGBank((uint8)i)));
    for (uint32 i = 0; i < 8; ++i) {
        const uint32 bank = (uint32)MMC3_getCHRBank((uint8)i);
        if (M5007Active == YHC_M7_TQROM && (bank & 0x40))
            setchr1r(YHC_M7_CHRRAM_CHIP, i * 1024u, bank & 7u);
        else if (M5007Active == YHC_M7_M74 && (bank == 8 || bank == 9))
            setchr1r(YHC_M7_CHRRAM_CHIP, i * 1024u, bank - 8u);
        else Mapper5007_SetCHR1(i,
            M5007Active == YHC_M7_TXSROM ? bank & 0x7Fu :
            M5007Active == YHC_M7_TQROM ? bank & 0x3Fu : bank);
    }
    if (M5007Active == YHC_M7_TXSROM) {
        PPU_hook = Mapper5007_TxSROMPPU;
        setmirror(MMC3_getCHRBank(M5007TxSLast & 7u) & 0x80 ? MI_1 : MI_0);
    }
    else {
        PPU_hook = NULL;
        MMC3_syncMirror();
    }
    if (M5007UseWRAM) MMC3_syncWRAM(0);
}
static void Mapper5007_SyncVRC(void) {
    switch (M5007Active) {
    case YHC_M7_VRC1:
        VRC1_syncPRG((int)(M5007PRG8Count - 1u), (int)M5007PRG8Base);
        if (M5007UseRAM) for (uint32 i = 0; i < 8; ++i) Mapper5007_SetCHR1(i, i);
        else VRC1_syncCHR((int)(M5007CHR1Count / 4u - 1u), (int)(M5007CHR1Base / 4u));
        VRC1_syncMirror(); break;
    case YHC_M7_VRC2: case YHC_M7_VRC4:
        VRC24_syncPRG((int)(M5007PRG8Count - 1u), (int)M5007PRG8Base);
        if (M5007UseRAM) {
            for (uint32 i = 0; i < 8; ++i) {
                uint32 bank = (uint32)VRC24_getCHRBank((uint8)i);
                if (M5007Active == YHC_M7_VRC2 && M5007Profile == 0) bank >>= 1; // VRC2a missing CHR A0
                Mapper5007_SetCHR1(i, bank);
            }
        }
        else VRC24_syncCHR((int)(M5007CHR1Count - 1u), (int)M5007CHR1Base);
        VRC24_syncMirror(); break;
    case YHC_M7_VRC3:
        VRC3_syncPRG((int)(M5007PRG8Count / 2u - 1u), (int)(M5007PRG8Base / 2u));
        for (uint32 i = 0; i < 8; ++i) Mapper5007_SetCHR1(i, i);
        setmirror(MI_H); break;
    case YHC_M7_VRC6:
        VRC6_syncPRG((int)(M5007PRG8Count - 1u), (int)M5007PRG8Base);
        if (M5007UseRAM) for (uint32 i = 0; i < 8; ++i) Mapper5007_SetCHR1(i, (uint32)VRC6_getCHRBank((uint8)i));
        else VRC6_syncCHR((int)(M5007CHR1Count - 1u), (int)M5007CHR1Base);
        VRC6_syncMirror(); break;
    case YHC_M7_VRC7:
        VRC7_syncPRG((int)(M5007PRG8Count - 1u), (int)M5007PRG8Base);
        if (M5007UseRAM) for (uint32 i = 0; i < 8; ++i) Mapper5007_SetCHR1(i, (uint32)VRC7_getCHRBank((uint8)i));
        else VRC7_syncCHR((int)(M5007CHR1Count - 1u), (int)M5007CHR1Base);
        VRC7_syncMirror(); break;
    }
}
static void Mapper5007_Sync(void) {
    if (M5007Active == YHC_M7_UNROM || M5007Active == YHC_M7_CNROM || M5007Active == YHC_M7_NROM)
        Mapper5007_SyncDiscrete();
    else if (M5007Active == YHC_M7_MMC1) Mapper5007_SyncMMC1();
    else if (M5007Active >= YHC_M7_MMC3 && M5007Active <= YHC_M7_M74) Mapper5007_SyncMMC3();
    else Mapper5007_SyncVRC();
}
static int Mapper5007_VRC2aCHR(uint8 bank) {
    // VRC2a (Mapper 22) does not connect the low CHR address bit.
    return VRC24_getCHRBank(bank) >> 1;
}
static DECLFW(Mapper5007_DiscreteWrite) {
    if (M5007BusConflict) V &= CartBR(A);
    M5007Latch = V;
    Mapper5007_SyncDiscrete();
}
static DECLFR(Mapper5007_OpenBus) { return X.DB; }
static DECLFW(Mapper5007_IgnoreWrite) { (void)A; (void)V; }
static DECLFR(Mapper5007_RAMRead) { return CartBR(A); }
static DECLFW(Mapper5007_RAMWrite) { CartBW(A, V); }

static void Mapper5007_StopASIC(void) {
    if (M5007Installed == YHC_M7_VRC6) VRC6_deactivate();
    if (M5007Installed == YHC_M7_VRC7) VRC7_deactivate();
    M5007Installed = 0xFF;
    // MMC1/VRC CPU clock and MMC3 PPU scanline hooks are exclusive.
    MapIRQHook = NULL;
    GameHBIRQHook = NULL;
    PPU_hook = NULL;
    X6502_IRQEnd(FCEU_IQEXT);
}
static void Mapper5007_InstallBoardHandlers(void);
static void Mapper5007_Activate(uint8 type, uint8 profile, uint8 pins, bool clear) {
    Mapper5007_StopASIC();
    M5007Active = type; M5007Profile = profile; M5007TxSLast = 0;
    const uint8 a0 = pins & 0x0Fu, a1 = pins >> 4;
    const int A0 = a0 < 8 ? (1 << a0) : 0;
    const int A1 = a1 < 8 ? (1 << a1) : 0;
    // VRC2/VRC4 share one ASIC core, but not one memory model.  Explicitly
    // expose the selected game's 0/2/8KiB geometry from the shared backing.
    VRC24_setWRAMBytes((type == YHC_M7_VRC2 || type == YHC_M7_VRC4) ? (int)M5007UseWRAM * 1024 : -1);
    switch (type) {
    case YHC_M7_UNROM: case YHC_M7_CNROM: case YHC_M7_NROM:
        SetReadHandler(0x8000, 0xFFFF, CartBR);
        SetWriteHandler(0x8000, 0xFFFF,
            type == YHC_M7_NROM ? Mapper5007_IgnoreWrite : Mapper5007_DiscreteWrite);
        if (clear) M5007Latch = 0;
        Mapper5007_Sync(); break;
    case YHC_M7_MMC1:
        MMC1_activate(clear ? 1u : 0u, Mapper5007_Sync,
            profile == 1 ? MMC1_TYPE_MMC1A : MMC1_TYPE_MMC1B,
            NULL, NULL, NULL, NULL); break;
    case YHC_M7_MMC3: case YHC_M7_TXSROM: case YHC_M7_TQROM: case YHC_M7_M74:
        MMC3_activate(clear ? 1u : 0u, Mapper5007_Sync,
            profile == 1 ? MMC3_TYPE_NEC : MMC3_TYPE_SHARP,
            NULL, NULL, NULL, NULL); break;
    case YHC_M7_VRC1: VRC1_activate(clear ? 1u : 0u, Mapper5007_Sync); break;
    case YHC_M7_VRC2: VRC2_activate(clear ? 1u : 0u, Mapper5007_Sync, A0, A1, NULL,
        profile == 0 ? Mapper5007_VRC2aCHR : NULL, NULL, NULL); break;
    case YHC_M7_VRC3: VRC3_activate(clear ? 1u : 0u, Mapper5007_Sync); break;
    case YHC_M7_VRC4: VRC4_activate(clear ? 1u : 0u, Mapper5007_Sync, A0, A1,
        profile == 1 ? 0u : 1u, NULL, NULL, NULL, NULL, NULL); break;
    case YHC_M7_VRC6: VRC6_activate(clear ? 1u : 0u, Mapper5007_Sync, A0, A1, NULL, NULL, NULL, NULL); break;
    case YHC_M7_VRC7:
        VRC7_setFMEnabled(profile == 0 ? 1u : 0u);
        VRC7_setPirateMode(profile == 2 ? 1u : 0u);
        VRC7_activate(clear ? 1u : 0u, Mapper5007_Sync, A0); break;
    }
    // Reinstall the shared backing page at $6000; VRC4 2KiB mirroring is enforced by VRC24 handlers.
    // Other ASICs do not uniformly call their syncWRAM() on activation.
    if (M5007UseWRAM) setprg8r(0x10, 0x6000, 0);
    // VRC ASICs re-install $6000 handlers; no-WRAM games should see
    // open bus there rather than accidentally reading the shared SRAM.
    if (!M5007UseWRAM) {
        SetReadHandler(0x6000, 0x7FFF, Mapper5007_OpenBus);
        SetWriteHandler(0x6000, 0x7FFF, Mapper5007_IgnoreWrite);
        if (type == YHC_M7_VRC2) {
            // VRC2 can expose a separate one-bit Microwire register
            // on $6000-$6FFF even when it has no external WRAM.
            SetReadHandler(0x6000, 0x6FFF, VRC2_readMicrowire);
            SetWriteHandler(0x6000, 0x6FFF, VRC2_writeMicrowire);
        }
    }
    else if (type == YHC_M7_UNROM || type == YHC_M7_CNROM || type == YHC_M7_NROM || type == YHC_M7_VRC3 || type == YHC_M7_VRC1) {
        SetReadHandler(0x6000, 0x7FFF, Mapper5007_RAMRead);
        SetWriteHandler(0x6000, 0x7FFF, Mapper5007_RAMWrite);
    }
    M5007Installed = type;
    Mapper5007_InstallBoardHandlers();
}

static bool Mapper5007_Decode(uint32& pb, uint32& ps, uint32& cb, uint32& cs,
    uint8& type, uint8& profile, uint8& useRAM,
    uint8& useWRAM, uint8& conflict) {
    const uint8 pc = M5007Regs[4] >> 4, cc = M5007Regs[4] & 15u;
    type = M5007Regs[7] >> 4; profile = M5007Regs[7] & 15u;
    const uint8 wc = (M5007Regs[5] >> 3) & 7u;
    useRAM = (M5007Regs[5] >> 6) & 1u;
    conflict = M5007Regs[5] & 1u;
    // v2.4: WWW=0 none, 1=8KiB, 2=2KiB mirrored VRC4 RAM.
    // Larger banked WRAM remains reserved for a later stage.
    if (pc > 6u || cc > 7u || type > YHC_M7_NROM || wc > 2u || (M5007Regs[5] & 0x06)) return false;
    const uint32 p16 = (uint32)M5007Regs[0] | ((uint32)(M5007Regs[1] & 15u) << 8);
    const uint32 c8 = (uint32)M5007Regs[2] | ((uint32)(M5007Regs[3] & 31u) << 8);
    const uint32 group16 = pc == 6u ? 1u : (2u << pc);
    const uint32 group8 = 1u << cc;
    if ((p16 & (group16 - 1u)) || p16 < 8u || p16>4096u - group16) return false;
    if (!useRAM || type == YHC_M7_TQROM || type == YHC_M7_M74 || type == YHC_M7_TXSROM) {
        if ((c8 & (group8 - 1u)) || c8 < 16u || c8>8192u - group8) return false;
    }
    pb = p16 * 2u; ps = group16 * 2u;
    cb = c8 * 8u; cs = group8 * 8u;
    // Limit each native ASIC, rather than claiming all 1MiB game sizes
    // are reachable just because the outer multicart has 64MiB.
    if (ps < 4u && type != YHC_M7_NROM && type != YHC_M7_CNROM) return false;
    // UxROM latch exposes exactly 8/16/32 x 16KiB banks per profile.
    if (type == YHC_M7_UNROM && (profile > 2u || ps != (profile == 2u ? 64u : profile == 1u ? 32u : 16u))) return false;
    if (type == YHC_M7_CNROM && (profile != 0u || (!useRAM && cs > 32u))) return false;
    if (type == YHC_M7_NROM && (profile > 1u || (profile == 1u && ps != 2u) || (profile == 0u && ps != 4u) || (!useRAM && cs > 8u))) return false;
    if (type == YHC_M7_MMC1 && (profile > 2u || ps > 64u || (ps == 64u && (!useRAM || profile != 2u)))) return false;
    if (type >= YHC_M7_MMC3 && type <= YHC_M7_M74 && (profile > 1u || (type != YHC_M7_MMC3 && profile != 0u))) return false;
    if (type == YHC_M7_MMC3 && cs > 256u && !useRAM) return false;
    if (type == YHC_M7_TXSROM && (cs > 128u || useRAM)) return false;
    if (type == YHC_M7_TQROM && (cs > 64u || useRAM)) return false;
    if (type == YHC_M7_M74 && (useRAM || cs > 256u)) return false;
    if (type == YHC_M7_VRC1 && (profile != 0u || cs > 128u)) return false;
    if (type == YHC_M7_VRC2 && (profile > 1u || cs > (profile == 0u ? 128u : 256u))) return false;
    if (type == YHC_M7_VRC3 && (profile != 0u || cs > 8u)) return false;
    if (type == YHC_M7_VRC4 && (profile > 1u || cs > 256u)) return false;
    if (type == YHC_M7_VRC6 && (profile != 0u || cs > 256u)) return false;
    if (type == YHC_M7_VRC7 && (profile > 2u || cs > 256u)) return false;
    if (conflict && type != YHC_M7_CNROM && type != YHC_M7_UNROM) return false;
    // Reject a CHR-ROM group beyond the actual loaded CHR allocation;
    // CHRmask1[0] is the loaded 1KiB bank mask in FCEUX.
    const bool needsCHR = (!useRAM || type == YHC_M7_TQROM ||
        type == YHC_M7_M74 || type == YHC_M7_TXSROM);
    if (needsCHR && (!CHRptr[0] || (CHRmask1[0] + 1u) < cb + cs)) return false;
    if (type == YHC_M7_MMC1 && !useRAM && cs > 128u) return false;
    if ((type == YHC_M7_UNROM || type == YHC_M7_VRC3) && !useRAM) return false;
    // VRC2/4/6 need CPU address pin mux; VRC7 needs one input line.
    if (type == YHC_M7_VRC2 || type == YHC_M7_VRC4 || type == YHC_M7_VRC6) {
        const uint8 a = M5007Regs[8] & 15u, b = M5007Regs[8] >> 4;
        if (a > 7u || b > 7u || a == b) return false;
    }
    if (type == YHC_M7_VRC7 && (M5007Regs[8] & 15u) > 7u) return false;
    if (wc == 2u && type != YHC_M7_VRC4) return false;
    useWRAM = wc == 1u ? 8u : wc == 2u ? 2u : 0u;
    return true;
}
static DECLFR(Mapper5007_ReadReg) {
    const uint8 r = (uint8)(A - 0x4800u);
    if (r == 6u) return (uint8)((M5007Status & 0x81u) | (M5007Locked ? 2u : 0u) | (M5007IsGame ? 4u : 0u));
    return r < 9u ? M5007Regs[r] : X.DB;
}
static DECLFW(Mapper5007_WriteReg) {
    if (M5007Locked) return;
    const uint8 r = (uint8)(A - 0x4800u);
    if (r != 6u) {
        if (r < 9u) {
            static const uint8 masks[9] = { 0xFF,0x0F,0xFF,0x1F,0xFF,0xF9,0x00,0xFF,0xFF };
            M5007Regs[r] = V & masks[r];
        }
        return;
    }
    if (V & 0x10u) {
        // Switch only the menu ASIC. No game bank or RAM is consumed.
        const uint8 menu = (uint8)(M5007Regs[7] >> 4);
        if (menu != YHC_M7_MMC3 && menu != YHC_M7_VRC6 && menu != YHC_M7_VRC7) {
            M5007Status = 0x80; return;
        }
        const uint8 prof = (uint8)(M5007Regs[7] & 15u);
        if ((menu == YHC_M7_VRC6 && prof != 0) || (menu == YHC_M7_VRC7 && prof > 1) ||
            (menu == YHC_M7_MMC3 && prof > 1)) {
            M5007Status = 0x80; return;
        }
        const uint8 ma = M5007Regs[8] & 15u, mb = M5007Regs[8] >> 4;
        if ((menu == YHC_M7_VRC6 && (ma > 7u || mb > 7u || ma == mb)) ||
            (menu == YHC_M7_VRC7 && ma > 7u)) {
            M5007Status = 0x80; return;
        }
        M5007Menu = menu; M5007MenuPins = M5007Regs[8]; M5007MenuProfile = prof;
        M5007Pins = M5007MenuPins;
        M5007IsGame = 0; M5007PRG8Base = M5007CHR1Base = 0;
        M5007PRG8Count = 16u; M5007CHR1Count = 128u;
        M5007UseRAM = 0; M5007UseWRAM = 8; M5007BusConflict = 0;
        Mapper5007_Activate(menu, prof, M5007Regs[8], true);
        M5007Status = 1; return;
    }
    if (!(V & 1u))return;
    uint32 pb = 0, ps = 0, cb = 0, cs = 0;
    uint8 type = 0, profile = 0, ram = 0, wram = 0, conflict = 0;
    if (!Mapper5007_Decode(pb, ps, cb, cs, type, profile, ram, wram, conflict)) {
        M5007Status = 0x80; return;
    }
    if (V & 2u)memset(M5007CHRRAM, 0, sizeof(M5007CHRRAM));
    if (V & 4u)memset(M5007WRAM, 0, sizeof(M5007WRAM));
    M5007PRG8Base = pb; M5007PRG8Count = ps;
    M5007CHR1Base = cb; M5007CHR1Count = cs;
    M5007UseRAM = ram; M5007UseWRAM = wram; M5007BusConflict = conflict;
    M5007IsGame = 1; M5007Pins = M5007Regs[8];
    Mapper5007_Activate(type, profile, M5007Pins, true);
    M5007Locked = (M5007Regs[5] & 0x80u) ? 1u : 0u;
    M5007Status = 1;
}
static void Mapper5007_InstallBoardHandlers(void) {
    SetWriteHandler(0x4800, 0x4808, Mapper5007_WriteReg);
    SetReadHandler(0x4800, 0x4808, Mapper5007_ReadReg);
}
static void Mapper5007_ResetBoard(bool power) {
    if (power) {
        // Never destroy an existing battery-backed RAM image at Power.
        if (!M5007Battery) memset(M5007WRAM, 0, sizeof(M5007WRAM));
        memset(M5007CHRRAM, 0, sizeof(M5007CHRRAM));
        M5007Menu = YHC_M7_MMC3; M5007MenuProfile = 0; M5007MenuPins = 0xFF;
    }
    // Soft Reset remembers the selected menu core, not the last game.
    const uint8 menu = M5007Menu;
    memset(M5007Regs, 0, sizeof(M5007Regs));
    M5007Regs[4] = 0x24u; M5007Regs[5] = 0x08u;
    M5007Regs[7] = (uint8)((menu << 4) | M5007MenuProfile);
    M5007Regs[8] = M5007MenuPins; M5007Pins = M5007MenuPins;
    M5007Locked = M5007IsGame = M5007BusConflict = M5007UseRAM = 0;
    M5007Status = 1; M5007UseWRAM = 8;
    M5007PRG8Base = M5007CHR1Base = 0;
    M5007PRG8Count = 16; M5007CHR1Count = 128;
    Mapper5007_Activate(menu, M5007MenuProfile, M5007MenuPins, true);
}
static void Mapper5007_Power(void) { Mapper5007_ResetBoard(true); }
static void Mapper5007_Reset(void) { Mapper5007_ResetBoard(false); }
static void Mapper5007_Restore(int version) {
    (void)version;
    Mapper5007_Activate(M5007Active, M5007Profile, M5007Pins, false);
}
static void Mapper5007_Close(void) { Mapper5007_StopASIC(); VRC24_setWRAMBytes(-1); VRC7_close(); }
static void Mapper5007_Init(CartInfo* info) {
    M5007Installed = 0xFF;
    M5007Battery = (info->battery != 0) ? 1u : 0u;
    // Dedicated RAM chip indices do not overlap 500.3/500.4/500.5/500.6.
    SetupCartCHRMapping(YHC_M7_CHRRAM_CHIP, M5007CHRRAM, sizeof(M5007CHRRAM), 1);
    SetupCartPRGMapping(YHC_M7_WRAM_CHIP, M5007WRAM, sizeof(M5007WRAM), 1);
    // Existing MMC1/MMC3/VRC WRAM sync helpers use chip $10.
    // Register the same backing storage at $10 for this submapper only.
    SetupCartPRGMapping(0x10, M5007WRAM, sizeof(M5007WRAM), 1);
    if (M5007Battery) info->addSaveGameBuf(M5007WRAM, sizeof(M5007WRAM));
    MMC1_addExState(); MMC3_addExState();
    VRC1_addExState(); VRC24_addExState(); VRC3_addExState();
    VRC6_addExState(); VRC7_addExState();
    AddExState(M5007State, ~0, 0, NULL);
    info->Power = Mapper5007_Power; info->Reset = Mapper5007_Reset;
    info->Close = Mapper5007_Close;
    GameStateRestore = Mapper5007_Restore;
}

// One-screen nametable selection.  Always select MI_0 / MI_1 explicitly.
// Do not write (MI_0 + bit) & 1: that yields H/V rather than one-screen.
static void Mapper500_SetOneScreen(uint8 page)
{
    setmirror((page & 1) ? MI_1 : MI_0);
}

static void Mapper500_Sync(void)
{
    // Preserve the original $7000-$7FFF mapping for all three modes.
    setprg4(0x7000, 0);

    switch (submapper)
    {
    case 1: // 500.1 - AxROM; bit 1 adds explicitly grouped 512 KiB games.
    {
        if (reg[1] & 0x02)
        {
            // 512 KiB = 16 x 32 KiB, 32 groups fit into 16 MiB.
            // Bit 1 is new and must be set by new multicart menus.
            const uint32 base32 = ((uint32)(reg[0] & 0x1F) << 4);
            setprg32(0x8000, base32 | (latch.data & 0x0F));
        }
        else
        {
            // Preserve the original address calculation exactly.
            setprg32(0x8000, ((uint32)reg[0] << 2) + (latch.data & 7));
        }
        setchr8(0);
        // $6001 bit2 = 0: use AxROM game latch bit4 (v1.5.2 protocol).
        // $6001 bit2 = 1: use $6001 bit0 (original Mapper501 board
        // registration behavior).  Choose explicitly in the multicart menu.
        // This extra mode does not change game banking or the write lock.
        const uint8 screen = (reg[1] & 0x04)
            ? (reg[1] & 0x01)
            : ((latch.data >> 4) & 0x01);
        Mapper500_SetOneScreen(screen);
        break;
    }

    case 2: // 500.2 - A/B/UxROM, new PRG A25 extension.
    {
        const uint32 mask = (8u << ((reg[1] >> 4) & 3)) - 1u;
        // Old outer range: 0..255 (32 MiB in 128 KiB groups).
        // New reg[1].6 adds outer bit 8, range 0..511 (64 MiB).
        // For old 16/32 MiB ROMs FCEUX PRGmask16/32 wraps bit 8 away.
        const uint32 outer = (uint32)reg[0] |
            ((uint32)(reg[1] & 0x40) << 2);

        if (reg[1] & 6)
        {
            setprg32(0x8000,
                (outer << 2) + (latch.data & (mask >> 1)));
        }
        else
        {
            setprg16(0x8000,
                (outer << 3) + (latch.data & mask));
            setprg16(0xC000,
                (outer << 3) + mask);
        }

        setchr8(0);
        if (reg[1] & 2)
        {
            // AxROM/BxROM one-screen mode: game latch bit4 selects CIRAM
            // page 0/1. Parentheses are essential; do not mask MI_0 itself.
            Mapper500_SetOneScreen((latch.data >> 4) & 1);
        }
        else
        {
            setmirror(reg[1] & 1);
        }
        break;
    }

    case 0: // 500.0 - UNROM / UOROM / extended UxROM 512 KiB
    default:
        if (reg[1] & 0x08)
        {
            // Extended UxROM: 512 KiB = 32 x 16 KiB.
            // This is nonstandard; stock UNROM/UOROM games do not
            // automatically gain a 5-bit inner bank register.
            const uint32 base16 = ((uint32)(reg[0] & 0x3F) << 5);
            setprg16(0x8000, base16 | (latch.data & 0x1F));
            setprg16(0xC000, base16 | 0x1F);
        }
        else if (reg[1] & 0x04)
        {
            // UOROM: 16 KiB bank select uses latch bits 0..3.
            // 128 groups * 256 KiB = 32 MiB; reg[0].7 is ignored.
            const uint32 outer = ((uint32)(reg[0] & 0x7F) << 4);
            setprg16(0x8000, outer | (latch.data & 0x0F));
            setprg16(0xC000, outer | 0x0F);
        }
        else
        {
            // UNROM: preserve all original behavior, including 8-bank fixed
            // region. 256 groups * 128 KiB = 32 MiB.
            const uint32 outer = (uint32)reg[0] << 3;
            setprg16(0x8000, outer | (latch.data & 0x07));
            setprg16(0xC000, outer | 0x07);
        }
        setchr8(0);
        setmirror(reg[1] & 1);
        break;
    }
}

static DECLFW(Mapper500_WriteReg)
{
    if (~reg[1] & 0x80)
    {
        reg[A & 1] = V;
        Mapper500_Sync();
    }
}

static void Mapper500_Power(void)
{
    reg[0] = 0;
    reg[1] = 0;
    LatchPower();
    SetReadHandler(0x7000, 0x7FFF, CartBR);
    SetWriteHandler(0x6000, 0x6FFF, Mapper500_WriteReg);
}

void Mapper500_Init(CartInfo* info)
{
    submapper = (uint8)(info->submapper & 0x0F);
    if (submapper == 3)
    {
        Mapper5003_Init(info);
        return;
    }
    if (submapper == 4)
    {
        // MMC1 serial writes belong to the existing ASIC_MMC1 module.
        Mapper5004_Init(info);
        return;
    }
    if (submapper == 5)
    {
        Mapper5005_Init(info);
        return;
    }
    if (submapper == 6)
    {
        Mapper5006_Init(info);
        return;
    }
    if (submapper == 7)
    {
        Mapper5007_Init(info);
        return;
    }
    // Unassigned submappers still use the old 500.0 fallback.
    Latch_Init(info, Mapper500_Sync, NULL, 0, 0);
    info->Power = Mapper500_Power;
    AddExState(StateRegs, ~0, 0, NULL);
}

// UNIF board names have no NES 2.0 submapper; keep explicit wrappers.
void Mapper500_UNIF0_Init(CartInfo* info)
{
    info->submapper = 0;
    Mapper500_Init(info);
}

void Mapper500_UNIF1_Init(CartInfo* info)
{
    info->submapper = 1;
    Mapper500_Init(info);
}

void Mapper500_UNIF2_Init(CartInfo* info)
{
    info->submapper = 2;
    Mapper500_Init(info);
}

// UNIF-specific wrapper; add its prototype to ines.h and board name to unif.cpp.
void Mapper500_UNIF3_Init(CartInfo* info)
{
    info->submapper = 3;
    Mapper500_Init(info);
}

// Mapper 500.4 UNIF variant (board name is not a NES 2.0 submapper).
void Mapper500_UNIF4_Init(CartInfo* info)
{
    info->submapper = 4;
    Mapper500_Init(info);
}

// MMC3 TxROM UNIF board alias. Declare in ines.h and register in unif.cpp.
void Mapper500_UNIF5_Init(CartInfo* info)
{
    info->submapper = 5;
    Mapper500_Init(info);
}

// MMC5 multicart UNIF alias (requires ines.h declaration and unif.cpp entry).
void Mapper500_UNIF6_Init(CartInfo* info)
{
    info->submapper = 6;
    Mapper500_Init(info);
}

// Multi-ASIC Hybrid Mapper500.7 UNIF wrapper (optional board registration).
void Mapper500_UNIF7_Init(CartInfo* info)
{
    info->submapper = 7;
    Mapper500_Init(info);
}
