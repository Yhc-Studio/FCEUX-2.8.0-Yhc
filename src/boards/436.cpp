/* FCE Ultra - NES/Famicom Emulator
 *
 * NES 2.0 Mapper 436: OneBus / 820401/T-217
 *
 * The implementation lives in onebus.cpp so Mapper 436 shares the same
 * OneBus register, encryption, CHR-RAM and state logic as Mapper 256/270/408.
 */

#include "mapinc.h"

extern void Mapper436_OneBus_Init(CartInfo *info);

void Mapper436_Init(CartInfo *info) {
	Mapper436_OneBus_Init(info);
}
