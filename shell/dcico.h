//
// dcico.h - .ico (ICONDIR + DIB) decoder for the dcgfx icon atlas.
//
// The CE shell icons ship as real .ico files in \Windows\dcico_*.ico (extracted from the
// H/PC ceshell/explorer resources). This decodes one image out of an .ico into ARGB4444
// pixels for the VRAM atlas - a pure byte parser, no Win32, so it stays host-testable.
//
// Supported image formats: 1/4/8bpp palettized (palette in the DIB), 24bpp BGR, 32bpp BGRA;
// every image carries a 1bpp AND transparency mask (bit set = transparent). The output cell
// is always nDim x nDim; the picked source image is nearest-neighbour scaled to fit.
//
#ifndef DCICO_H
#define DCICO_H

// LE reads - the .ico/DIB formats are little-endian (and the file bytes may sit on any
// alignment), so never cast the buffer to a struct.
static unsigned short IcoU16(const unsigned char *p)
{
	return (unsigned short)(p[0] | (p[1] << 8));
}
static unsigned long IcoU32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
	       ((unsigned long)p[3] << 24);
}

// Decode one image out of an .ico blob into nDim x nDim ARGB4444 (0xAARRGGBB, alpha 0 =
// transparent). Prefers an exact nDim match, then the smallest image >= nDim, then the
// largest available; the source is NEAREST-scaled to the cell. Returns 1 on success.
static int IcoTo4444(const unsigned char *pb, unsigned long cb, int nDim, unsigned short *pOut)
{
	unsigned long nCount, i, nOff, nLen;
	long nBest = -1;
	const unsigned char *pDib, *pXor, *pAnd, *pPal;
	long nW, nH, nBpp, nClr, nPal;
	long nXstride, nAstride;
	int x, y, ok = 0;

	if (!pb || cb < 6 || IcoU16(pb) != 0 || IcoU16(pb + 2) != 1) // reserved + type ICON
		return 0;
	nCount = IcoU16(pb + 4);
	if (!nCount || 6 + nCount * 16 > cb)
		return 0;

	// pick the image: exact size wins, else the smallest that covers nDim, else the biggest
	for (i = 0; i < nCount; i++)
	{
		const unsigned char *e = pb + 6 + i * 16;
		long w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
		long bw;
		if (IcoU32(e + 8) > cb || IcoU32(e + 12) > cb - IcoU32(e + 8)) // len/off must fit
			continue;
		if (w == nDim && h == nDim)
		{
			nBest = (long)i;
			break;
		}
		bw = (nBest >= 0) ? (pb[6 + nBest * 16] ? pb[6 + nBest * 16] : 256) : 0;
		if (nBest < 0 || (w >= nDim && (bw < nDim || w < bw)) || (bw < nDim && w > bw))
			nBest = (long)i;
	}
	if (nBest < 0)
		return 0;
	{
		const unsigned char *e = pb + 6 + nBest * 16;
		nOff = IcoU32(e + 12);
		nLen = IcoU32(e + 8);
	}
	if (nLen > cb || nOff > cb - nLen || nLen < 40)
		return 0;
	pDib = pb + nOff;
	if (IcoU32(pDib) < 40) // BITMAPINFOHEADER
		return 0;
	nW = (long)IcoU32(pDib + 4);
	nH = (long)(IcoU32(pDib + 8) >> 1); // biHeight counts XOR + AND planes
	nBpp = IcoU16(pDib + 14);
	nClr = IcoU32(pDib + 32);
	if (nW <= 0 || nH <= 0)
		return 0;
	nPal = (nBpp <= 8) ? (nClr ? (long)nClr : (1L << nBpp)) : 0;
	pPal = pDib + IcoU32(pDib);
	pXor = pPal + nPal * 4;
	nXstride = ((nW * nBpp + 31) >> 5) << 2;
	nAstride = ((nW + 31) >> 5) << 2;
	if (IcoU32(pDib) + (unsigned long)nPal * 4 + (unsigned long)nXstride * nH +
	        (unsigned long)nAstride * nH >
	    nLen)
		return 0;
	pAnd = pXor + nXstride * nH;

	for (y = 0; y < nDim; y++)
		for (x = 0; x < nDim; x++)
		{
			// nearest-neighbour: cell (x,y) -> source pixel (sx,sy), DIB rows are bottom-up
			long sx = (long)((x + 0.5) * nW / nDim);
			long sy = (long)((y + 0.5) * nH / nDim);
			const unsigned char *pRow;
			unsigned short wPx = 0;
			if (sx >= nW)
				sx = nW - 1;
			if (sy >= nH)
				sy = nH - 1;
			pRow = pAnd + (nH - 1 - sy) * nAstride;
			if ((pRow[sx >> 3] >> (7 - (sx & 7))) & 1)
			{
				pOut[y * nDim + x] = 0; // masked = transparent
				continue;
			}
			pRow = pXor + (nH - 1 - sy) * nXstride;
			if (nBpp == 24 || nBpp == 32)
			{
				const unsigned char *px = pRow + sx * (nBpp >> 3);
				wPx = (unsigned short)(0xF000 | ((px[2] >> 4) << 8) | ((px[1] >> 4) << 4) |
				                       (px[0] >> 4));
			}
			else
			{
				unsigned long idx;
				if (nBpp == 8)
					idx = pRow[sx];
				else if (nBpp == 4)
					idx = (sx & 1) ? (pRow[sx >> 1] & 0x0F) : (pRow[sx >> 1] >> 4);
				else // 1bpp
					idx = (pRow[sx >> 3] >> (7 - (sx & 7))) & 1;
				if (idx < (unsigned long)nPal)
				{
					const unsigned char *pc = pPal + idx * 4; // B,G,R,0
					wPx = (unsigned short)(0xF000 | ((pc[2] >> 4) << 8) |
					                       ((pc[1] >> 4) << 4) | (pc[0] >> 4));
				}
			}
			pOut[y * nDim + x] = wPx;
		}
	ok = 1;
	return ok;
}

#endif // DCICO_H
