/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2025 NewRisingSun
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

#ifndef _ASIC_VRC7_H
#define _ASIC_VRC7_H

void VRC7_syncWRAM (int);
void VRC7_syncPRG (int, int);
void VRC7_syncCHR (int, int);
int VRC7_getCHRBank(uint8);
void VRC7_syncMirror ();
DECLFR(VRC7_readWRAM);
DECLFW(VRC7_writeWRAM);
DECLFW(VRC7_writeReg);
void VRC7_cpuCycle (int);
void VRC7_clear ();
void VRC7_setFMEnabled(uint8); // 0 for non-FM VRC7 clones, 1 for VRC7a
void VRC7_setPirateMode(uint8); // optional YOKO VRC7P IRQ threshold 248
void VRC7_deactivate(void);
void VRC7_close(void);
void VRC7_activate (uint8, void (*)(), int);
void VRC7_addExState ();
void VRC7_restore (int);
void VRC7_power ();
void VRC7_init (CartInfo *, void (*)(), int);

#endif