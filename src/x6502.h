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

#ifndef _X6502H

#include "x6502struct.h"

extern X6502 X;


//the opsize table is used to quickly grab the instruction sizes (in bytes)
extern const uint8 opsize[256];

//the optype table is a quick way to grab the addressing mode for any 6502 opcode
extern const uint8 optype[256];

// the opwrite table aids in predicting the value written for any 6502 opcode
extern const uint8 opwrite[256];

//-----------
//mbg 6/30/06 - some of this was removed to mimic XD
//#ifdef FCEUDEF_DEBUGGER
void X6502_Debug(void (*CPUHook)(X6502 *),
    uint8 (*ReadHook)(X6502 *, unsigned int),
    void (*WriteHook)(X6502 *, unsigned int, uint8));

//extern void (*X6502_Run)(int32 cycles);
//#else
//void X6502_Run(int32 cycles);
//#endif
// Stage 3.6.6b: automatic normal/debug CPU execution routing.
void X6502_RunNormal(int32 cycles);
void X6502_RunDebug(int32 cycles);
void X6502_RunAuto(int32 cycles);
#define X6502_Run(x) X6502_RunAuto(x)
//------------

extern uint32 timestamp;
extern uint32 soundtimestamp;

extern uint8 encryptOpcodes;
extern uint8 encryptOpcodesConfig;
extern uint8 encryptOpcodesPending;
extern uint8 encryptOpcodesPendingMode;

// Debugger/disassembler opcode decryption is gated separately from the CPU
// fetch state. Only OneBus/VTxx boards may enable this guard; ordinary NES
// boards always see raw opcode bytes even if a stale encryption variable were
// ever to survive a frontend/load edge case.
void X6502_SetOpcodeDebugDecryptionEnabled(uint8 enabled);
uint8 X6502_IsOpcodeDebugDecryptionEnabled(void);

// Return the opcode byte as the CPU will execute it under the currently
// active OneBus/VTxx opcode-encryption mode. This helper is side-effect free
// and returns rawOpcode unchanged while the board guard is disabled.
uint8 X6502_DecodeOpcodeForDebug(uint8 rawOpcode);

// VT369 can run the main 6502 at 3x while PPU/APU remain at their base
// clocks. The multiplier is board-controlled ($411C.7) and defaults to 1.
void X6502_SetOneBusClockMultiplier(uint8 multiplier);
uint8 X6502_GetOneBusClockMultiplier(void);
// PPU timing units: 48 units per ordinary CPU cycle, 16 per VT369 3x cycle.
uint32 X6502_GetPPUTimestamp48(void);
extern int scanline;

#define N_FLAG  0x80
#define V_FLAG  0x40
#define U_FLAG  0x20
#define B_FLAG  0x10
#define D_FLAG  0x08
#define I_FLAG  0x04
#define Z_FLAG  0x02
#define C_FLAG  0x01

extern void (*MapIRQHook)(int a);

#define NTSC_CPU (dendy ? 1773447.467 : 1789772.7272727272727272)
#define PAL_CPU  1662607.125

#define FCEU_IQEXT      0x001
#define FCEU_IQEXT2     0x002
/* ... */
#define FCEU_IQRESET    0x020
#define FCEU_IQNMI2  0x040  // Delayed NMI, gets converted to *_IQNMI
#define FCEU_IQNMI  0x080
#define FCEU_IQDPCM     0x100
#define FCEU_IQFCOUNT   0x200
#define FCEU_IQTEMP     0x800

void X6502_Init(void);
void X6502_Reset(void);
void X6502_Power(void);

void TriggerNMI(void);
void TriggerNMI2(void);

uint8 X6502_DMR(uint32 A);
void X6502_DMW(uint32 A, uint8 V);
void X6502_AddCycles(int cycles);

void X6502_IRQBegin(int w);
void X6502_IRQEnd(int w);

int X6502_GetOpcodeCycles( int op );

// FCEUX 2.7-style low-overhead CPU memory hook dispatcher.
// When no hook is registered, x6502's hot path only performs a null-pointer check.
class X6502_MemHook
{
public:
    enum Type { Read = 0, Write, Exec } type;

    static void Add(enum Type type,
        void (*func)(unsigned int address, unsigned int value, void *userData),
        void *userData = nullptr);
    static void Remove(enum Type type,
        void (*func)(unsigned int address, unsigned int value, void *userData),
        void *userData = nullptr);

    inline void call(unsigned int address, unsigned int value)
    {
        // Iterative dispatch avoids recursion when more than one hook is registered.
        X6502_MemHook *hook = this;
        while (hook != nullptr)
        {
            hook->func(address, value, hook->userData);
            hook = hook->next;
        }
    }

private:
    void (*func)(unsigned int address, unsigned int value, void *userData) = nullptr;
    void *userData = nullptr;
    X6502_MemHook *next = nullptr;
    int refCount = 0;
};

#define _X6502H
#endif
