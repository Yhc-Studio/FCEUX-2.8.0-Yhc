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

#include "../types.h"
#include "crc32.h"

#include <zlib.h>
uint32 CalcCRC32(uint32 crc, uint8 *buf, uint32 len)
{
 return(crc32(crc,buf,len));
}

uint32 FCEUI_CRC32(uint32 crc, uint8 *buf, uint32 len)
{
 return(CalcCRC32(crc,buf,len));
}

static uint32 crc32_gf2_matrix_times(const uint32 *mat, uint32 vec)
{
 uint32 sum = 0;
 while(vec)
 {
  if(vec & 1)
   sum ^= *mat;
  vec >>= 1;
  mat++;
 }
 return sum;
}

static void crc32_gf2_matrix_square(uint32 *square, const uint32 *mat)
{
 for(int n = 0; n < 32; n++)
  square[n] = crc32_gf2_matrix_times(mat, mat[n]);
}

/*
 * Combine two standard zlib CRC-32 values without rescanning the second
 * buffer. This is equivalent to crc32(crc1, secondBuffer, len2) when crc2
 * is crc32(0, secondBuffer, len2).
 *
 * FCEUX's bundled Windows zlib is 1.1.4 and predates crc32_combine(), so
 * keep the small GF(2) combine routine here instead of requiring a zlib
 * upgrade just for large-ROM loading.
 */
uint32 CalcCRC32Combine(uint32 crc1, uint32 crc2, uint64 len2)
{
 uint32 even[32];
 uint32 odd[32];

 if(!len2)
  return crc1;

 odd[0] = 0xEDB88320UL;
 uint32 row = 1;
 for(int n = 1; n < 32; n++)
 {
  odd[n] = row;
  row <<= 1;
 }

 crc32_gf2_matrix_square(even, odd);
 crc32_gf2_matrix_square(odd, even);

 do
 {
  crc32_gf2_matrix_square(even, odd);
  if(len2 & 1)
   crc1 = crc32_gf2_matrix_times(even, crc1);
  len2 >>= 1;

  if(!len2)
   break;

  crc32_gf2_matrix_square(odd, even);
  if(len2 & 1)
   crc1 = crc32_gf2_matrix_times(odd, crc1);
  len2 >>= 1;
 }
 while(len2);

 crc1 ^= crc2;
 return crc1;
}
