uint8 *C;
uint8 cc;
uint32 vadr;
#ifdef PPU_VRC5FETCH
uint8 tmpd;
#endif

#ifndef PPUT_MMC5SP
	FCEU_MAYBE_UNUSED uint8 zz;
#else
	uint8 xs, ys;
	xs = X1;
	ys = ((scanline >> 3) + MMC5HackSPScroll) & 0x1F;
	if (ys >= 0x1E) ys -= 0x1E;
#endif

if (X1 >= 2) {
	if (vtrender) {
		const int basepos = (int)(P - Plinef);
		if (vt369mode & 0x01) {
			for (int vp = 0; vp < 8; ++vp) {
				const uint8 pen = vt369shift[XOffset + vp];
				P[vp] = pen;
				if ((unsigned)(basepos + vp) < 256) {
					VTBGVisible[basepos + vp] = pen ? 1 : 0;
					VT369DirectPixel[basepos + vp] = 1;
					VT369RawColor[basepos + vp] = VT369SnapshotRGB555(pen);
				}
			}
		}
		else if (vtpacked) {
			for (int vp = 0; vp < 8; ++vp) {
				const uint8 pen = vtpackedshift[XOffset + vp];
				P[vp] = pen;
				if ((unsigned)(basepos + vp) < 256) VTBGVisible[basepos + vp] = pen ? 1 : 0;
			}
		}
		else {
			const uint8 b0 = (uint8)((pshift[0] >> (8 - XOffset)) & 0xFF);
			const uint8 b1 = (uint8)((pshift[1] >> (8 - XOffset)) & 0xFF);
			const uint8 b2 = (uint8)((pshift[2] >> (8 - XOffset)) & 0xFF);
			const uint8 b3 = (uint8)((pshift[3] >> (8 - XOffset)) & 0xFF);
			const uint32 attrdata = ppulut3[XOffset | (atlatch << 3)];
			for (int vp = 0; vp < 8; ++vp) {
				const uint8 bit = (uint8)(0x80 >> vp);
				uint8 pix = (b0 & bit ? 0x01 : 0x00) | (b1 & bit ? 0x02 : 0x00);
				if (vtmode & 0x02) {
					if (b2 & bit) pix |= 0x20;
					if (b3 & bit) pix |= 0x40;
				}
				uint8 attr = (uint8)((attrdata >> (vp * 4 + 2)) & 3);
				if (vtmode & 0x10) attr = 0;
				const uint8 opaque = (pix & 0x63) ? 1 : 0;
				P[vp] = opaque ? (uint8)(pix | (attr << 2)) : 0;
				if ((unsigned)(basepos + vp) < 256) VTBGVisible[basepos + vp] = opaque;
			}
		}
		P += 8;
	}
	else {
		uint8 *S = PALRAM;
		uint32 pixdata;
		pixdata = ppulut1[(pshift[0] >> (8 - XOffset)) & 0xFF] | ppulut2[(pshift[1] >> (8 - XOffset)) & 0xFF];
		pixdata |= ppulut3[XOffset | (atlatch << 3)];
		P[0] = S[pixdata & 0xF];
		pixdata >>= 4; P[1] = S[pixdata & 0xF];
		pixdata >>= 4; P[2] = S[pixdata & 0xF];
		pixdata >>= 4; P[3] = S[pixdata & 0xF];
		pixdata >>= 4; P[4] = S[pixdata & 0xF];
		pixdata >>= 4; P[5] = S[pixdata & 0xF];
		pixdata >>= 4; P[6] = S[pixdata & 0xF];
		pixdata >>= 4; P[7] = S[pixdata & 0xF];
		P += 8;
	}
}

#ifdef PPUT_MMC5SP
	vadr = (MMC5HackExNTARAMPtr[xs | (ys << 5)] << 4) + (vofs & 7);
#else
	zz = RefreshAddr & 0x1F;
	C = vnapage[(RefreshAddr >> 10) & 3];
#ifdef PPU_VRC5FETCH
	tmpd = QTAINTRAM[((((RefreshAddr >> 10) & 3) >> ((qtaintramreg >> 1)) & 1) << 10) | (RefreshAddr & 0x3FF)];
	vofs = ((tmpd & 0x3F) << 12) | ((RefreshAddr >> 12) & 7);	// recalculate VROM offset 
#endif
	/* VT369 12/09/0F mode stores two-byte tile numbers in 64-byte name-table
	 * rows.  Bit 11 selects the second 30-row screen vertically; horizontal
	 * name-table selection is deliberately ignored by this hardware mode. */
	if ((vt369mode & 0x10) && FFCEUX_PPUReadVT369BG16) {
		const uint16 coarseX = (uint16)(RefreshAddr & 0x1F);
		const uint16 coarseY = (uint16)((RefreshAddr >> 5) & 0x1F);
		const uint16 sourceY = (uint16)((coarseY + ((RefreshAddr & 0x0800) ? 30 : 0)) & 0x1F);
		const uint16 nameAddr = (uint16)(0x2000 + sourceY * 64 + coarseX * 2);
		const uint16 tile16 = (uint16)FFCEUX_PPURead_Default(nameAddr) |
			((uint16)FFCEUX_PPURead_Default((nameAddr + 1) & 0x3FFF) << 8);
		vadr = ((uint32)tile16 << 4) | (vofs & 7); /* hook/debug-safe surrogate */
	}
	else
		vadr = (C[RefreshAddr & 0x3ff] << 4) + vofs;				// Fetch name table byte.
#endif

#ifdef PPUT_HOOK
	PPU_hook(0x2000 | (RefreshAddr & 0xfff));
#endif

#ifdef PPUT_MMC5SP
	cc = MMC5HackExNTARAMPtr[0x3c0 + (xs >> 2) + ((ys & 0x1C) << 1)];
	cc = ((cc >> ((xs & 2) + ((ys & 0x2) << 1))) & 3);
#else
	#ifdef PPUT_MMC5CHR1
		cc = (MMC5HackExNTARAMPtr[RefreshAddr & 0x3ff] & 0xC0) >> 6;
	#else
		if (vt369mode & 0x10)
			cc = 0; /* special 16-bit names carry the full tile number; no legacy attribute bits */
		else {
			cc = C[0x3c0 + (zz >> 2) + ((RefreshAddr & 0x380) >> 4)];	// Fetch attribute table byte.
			cc = ((cc >> ((zz & 2) + ((RefreshAddr & 0x40) >> 4))) & 3);
		}
	#endif
#endif

atlatch >>= 2;
atlatch |= cc << 2;

pshift[0] <<= 8;
pshift[1] <<= 8;
pshift[2] <<= 8;
pshift[3] <<= 8;

if (vtrender && (vt369mode & 0x01) && FFCEUX_PPUReadVT369BG) {
	memmove(vt369shift, vt369shift + 8, 8);
	if ((vt369mode & 0x10) && FFCEUX_PPUReadVT369BG16) {
		const uint16 tile16 = (uint16)(vadr >> 4);
		const uint8 fineY = (uint8)(RefreshAddr >> 12) & 7;
		for (int vp = 0; vp < 8; ++vp)
			vt369shift[8 + vp] = FFCEUX_PPUReadVT369BG16(tile16, fineY, (uint8)vp);
	}
	else {
		for (int vp = 0; vp < 8; ++vp)
			vt369shift[8 + vp] = FFCEUX_PPUReadVT369BG(vadr, cc, (uint8)vp);
	}
}
else if (vtrender && vtpacked) {
	memmove(vtpackedshift, vtpackedshift + 8, 8);
	for (int vp = 0; vp < 8; ++vp)
		vtpackedshift[8 + vp] = FFCEUX_PPUReadPackedBG(RefreshAddr, (uint8)vp);
}
/* Packed VT369 data has already been fetched through the board callback.
 * Keep the legacy planar pointer/hook path inside the ordinary $0000-$1FFF
 * range so a 16-bit tile number cannot index VPage out of bounds. */
if (vtrender && (vt369mode & 0x01)) vadr &= 0x1FFF;

#ifdef PPUT_MMC5SP
	C = MMC5HackVROMPTR + vadr;
	C += ((MMC5HackSPPage & 0x3f & MMC5HackVROMMask) << 12);
#else
	#ifdef PPUT_MMC5CHR1
		C = MMC5HackVROMPTR;
		C += (((MMC5HackExNTARAMPtr[RefreshAddr & 0x3ff]) & 0x3f & MMC5HackVROMMask) << 12) + (vadr & 0xfff);
		C += (MMC50x5130 & 0x3) << 18; //11-jun-2009 for kuja_killer
	#elif defined(PPUT_MMC5)
		C = MMC5BGVRAMADR(vadr);
	#else

	#ifdef PPU_VRC5FETCH
	if(tmpd & 0x40)
		C = CHRptr[0] + vadr;
	else
		C = VRAMADR(vadr);
	#else
		C = VRAMADR(vadr);
#endif

	#endif
#endif

#ifdef PPUT_HOOK
	PPU_hook(vadr);
#endif

#ifdef PPU_BGFETCH
	if (RefreshAddr & 1) {
		if(ScreenON)
			RENDER_LOGP(C + 8);
		pshift[0] |= C[8];
		pshift[1] |= C[8];
	} else {
		if(ScreenON)
			RENDER_LOGP(C);
		pshift[0] |= C[0];
		pshift[1] |= C[0];
	}
#else
	#ifdef PPU_VRC5FETCH
	pshift[0] |= C[0];
	if(tmpd & 0x40)
		pshift[1] |= (tmpd & 0x80) ? 0xFF : 0x00;
	else
		pshift[1] |= C[8];
	#else
	if (vtrender && (vt369mode & 0x01)) {
		/* VT369 Stage 2 is packed/non-planar; vt369shift was filled above. */
	}
	else if (vtrender && !vtpacked) {
		pshift[0] |= FFCEUX_PPUReadPatternExt(vadr, cc, 0, 0);
		pshift[1] |= FFCEUX_PPUReadPatternExt(vadr, cc, 0, 1);
		if (vtmode & 0x02) {
			pshift[2] |= FFCEUX_PPUReadPatternExt(vadr, cc, 0, 2);
			pshift[3] |= FFCEUX_PPUReadPatternExt(vadr, cc, 0, 3);
		}
	}
	else {
		if(ScreenON)
			RENDER_LOGP(C);
		pshift[0] |= C[0];
		if(ScreenON)
			RENDER_LOGP(C + 8);
		pshift[1] |= C[8];
	}
	#endif
#endif

if ((RefreshAddr & 0x1f) == 0x1f)
	RefreshAddr ^= 0x41F;
else
	RefreshAddr++;

#ifdef PPUT_HOOK
	PPU_hook(0x2000 | (RefreshAddr & 0xfff));
#endif

