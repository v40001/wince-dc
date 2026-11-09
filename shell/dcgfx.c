//
// dcgfx.c - Direct3D / PVR2 hardware compositor for the Dreamcast CE shell.
//
// d3dim.dll is the real DX6 immediate-mode HAL driving the PVR2 tile renderer
// (TA/ISP/TSP) - confirmed by the Step-1 test quad. So the whole UI is rendered as
// hardware quads instead of software rasterization:
//   - GfxFill / GfxBevel  -> flat-colored D3DTLVERTEX quads (no texture)
//   - GfxIcon / GfxText    -> textured quads from ONE ARGB4444 VRAM atlas
//   - GfxPresent           -> BeginScene -> DrawIndexedPrimitive per same-texture run
//                             (emit order = painter's Z) -> EndScene -> Flip
// No CPU framebuffer, no per-frame memcpy, no per-layer Lock drain. Glyphs + icons
// live in PVR VRAM (the atlas), off the SH-4 bus. Constraints: DP1 path only (DP2 is
// a trap stub), NO Z buffer (submit-order Z), color-key -> alpha=0 at atlas upload.
//
#define CINTERFACE // C-style DDraw/D3D COM (dcgfx.h no longer defines this)
#include "dcgfx.h"
#include "dcico.h" // .ico -> ARGB4444 decoder (the real CE icons live in \Windows\dcico_*.ico)
#include <ddraw.h>
#include <d3d.h>
#include <wdm.h> // MmMapIoSpace / PHYSICAL_ADDRESS (page-layer framebuffer map)

// GDI framebuffer info from the display driver (ExtEscape GETGDIINFO), as in the SDK htmlsamp.
typedef struct
{
	int width, height, stride;
	unsigned long physicalAddr;
} GDISurfaceInfo;
#define GETGDIINFO 6500

#define KEY        0xF81F // 565 magenta = transparent key (atlas -> alpha 0)
#define ATLAS_DIM  512
#define GFIRST     32
#define GLAST      126
#define GN         (GLAST - GFIRST + 1)
#define GH         16 // glyph cell height
// Quad storage is DYNAMIC: the scene + desktop-cache arrays grow on demand (PushQuad doubles them)
// and shrink back after the scene stays small, so RAM tracks the busiest recent frame instead of a
// worst-case BSS reservation. Text renders one quad per character, so a couple of text-heavy
// windows can need a few thousand quads; with on-demand growth nothing is ever dropped (no
// taskbar/cursor loss) up to a ceiling no real UI reaches. The WORD index buffer caps a single
// same-texture batch at 16384 quads (index 16384*4 = 65536 would overflow), which is the ceiling.
#define QUAD_INIT 256   // initial capacity (one modest window)
#define QUAD_MAX  16384 // hard ceiling (WORD index limit); far above any real scene

static HWND s_hwnd = NULL;
static LPDIRECTDRAW s_pDd = NULL;
static LPDIRECTDRAWSURFACE s_pPrimary = NULL;
static LPDIRECTDRAWSURFACE s_pBack = NULL;
static BOOL s_bUseFlip = FALSE;
static LPDIRECT3D2 s_pD3d = NULL;
static LPDIRECT3DDEVICE2 s_pDev = NULL;
static LPDIRECT3DVIEWPORT2 s_pVp = NULL;
static BOOL s_bD3dOk = FALSE;

static LPDIRECTDRAWSURFACE s_pAtlasSurf = NULL; // VRAM atlas (kept alive while bound)
static LPDIRECT3DTEXTURE2 s_pAtlasTex = NULL;
static D3DTEXTUREHANDLE s_hAtlas = 0;

// Page layer (browser): GWES framebuffer wrap -> VRAM texture quad. See GfxInitPageLayer.
static LPDIRECTDRAWSURFACE s_pGdiSurf = NULL;  // system-mem surface aliasing the GDI framebuffer
static LPDIRECTDRAWSURFACE s_pPageSurf = NULL; // VRAM texture the page is blitted into
static LPDIRECT3DTEXTURE2 s_pPageTex = NULL;
static D3DTEXTUREHANDLE s_hPage = 0;
#define PAGE_TW 1024 // page texture is pow2 and >= 640x480
#define PAGE_TH 512

// Wallpaper layer (desktop background): a decoded BMP uploaded into a 565 VRAM texture, drawn as
// a quad under the icons. Same pow2 texture geometry as the page layer (1024x512 holds 640x480).
static LPDIRECTDRAWSURFACE s_pWallSurf = NULL; // VRAM 565 texture
static LPDIRECT3DTEXTURE2 s_pWallTex = NULL;
static D3DTEXTUREHANDLE s_hWall = 0;
static int s_nWallW = 0, s_nWallH = 0;     // wallpaper pixel size (clamped to PAGE_TW/TH)
static int s_nWallStyle = GFXWALL_STRETCH; // remembered draw style
static WCHAR s_szWallPath[MAX_PATH] = {0}; // remembered path (re-upload after a surface loss)

// Start-menu banner (the vertical "Windows CE" strip): same BMP->565 VRAM path as the
// wallpaper but on its own pow2 texture (BAN_TW x BAN_TH) so it doesn't share the 1 MB page.
#define BAN_TW 64
#define BAN_TH 512
static LPDIRECTDRAWSURFACE s_pBanSurf = NULL;
static LPDIRECT3DTEXTURE2 s_pBanTex = NULL;
static D3DTEXTUREHANDLE s_hBan = 0;
static int s_nBanW = 0, s_nBanH = 0;
static WCHAR s_szBanPath[MAX_PATH] = {0};

// The scene buffer is backed by a STATIC working buffer (BSS), so the desktop ALWAYS renders: a
// purely heap-allocated buffer black-screened the shell on real hardware (the PVR2's tile-accelerator
// reads vertices by DMA, and the process LocalHeap is apparently not where it can reach / can run
// out, whereas the module's static data always works - Flycast hid it by not DMAing). The heap path
// only kicks in for rare scenes above QUAD_WORK quads; below it nothing is ever allocated per frame.
#define QUAD_WORK 2048 // static scene capacity - covers the desktop + a couple of windows
static D3DTLVERTEX s_aSceneVb[QUAD_WORK * 4];
static WORD s_aSceneIb[QUAD_WORK * 6];
static BYTE s_aSceneQt[QUAD_WORK];

// Retained quad list (the scene), replayed every frame. It starts on the static buffer above and
// only moves to the heap above QUAD_WORK (detected by s_pVb != s_aSceneVb). Indices are base-relative.
static D3DTLVERTEX *s_pVb = s_aSceneVb;
static WORD *s_pIb = s_aSceneIb;
static BYTE *s_pQtex = s_aSceneQt; // tex: 0=solid 1=atlas 2=page
static int s_nCap = QUAD_WORK, s_nQuad = 0;

// Desktop cache: a cached vertex SUB-LIST (not pixels) prepended each frame; also grown on demand.
static D3DTLVERTEX *s_pDvb;
static WORD *s_pDib;
static BYTE *s_pDtex;
static int s_nDcap, s_nDQuad;
static BOOL s_bRecDesk = FALSE;

typedef struct
{
	float u0, v0, u1, v1;
	BYTE adv;
} GlyphUV;
static GlyphUV s_aGlyph[3][GN];
static BOOL s_bGlyphReady = FALSE;
typedef struct
{
	float u0, v0, u1, v1;
} RectUV;
static RectUV s_aIconUV[ICON_COUNT][2]; // [0]=16px, [1]=32px

HFONT g_FontUI = NULL;
HFONT g_FontBold = NULL;
HFONT g_FontTitle = NULL;

static WORD ToRgb565(COLORREF c)
{
	return (WORD)(((GetRValue(c) >> 3) << 11) | ((GetGValue(c) >> 2) << 5) | (GetBValue(c) >> 3));
}
static D3DCOLOR ToArgb(COLORREF c)
{
	return (D3DCOLOR)(0xFF000000u | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) |
	                  GetBValue(c));
}

static HFONT MakeFont(int nHeight, int nWeight)
{
	LOGFONTW lf;
	memset(&lf, 0, sizeof(lf));
	lf.lfHeight = -nHeight;
	lf.lfWeight = nWeight;
	lstrcpyW(lf.lfFaceName, L"Tahoma"); // the stock CE UI face (falls back if the ttf is absent)
	return CreateFontIndirectW(&lf);
}

// Real Windows CE icons: each ICON_* id maps to a .ico in \Windows (extracted from the H/PC
// ceshell/explorer resources and normalized to 16x16+32x32 24bpp+mask). When a file is missing
// the ASCII art below is used instead, so the shell still renders on a bare image.
static const WCHAR *const s_aIcoName[ICON_COUNT] = {
    L"mycomputer", // ICON_COMPUTER
    L"drive",      // ICON_DRIVE
    L"folder",     // ICON_FOLDER
    L"app",        // ICON_APP
    L"clock",      // ICON_CLOCK
    L"file",       // ICON_FILE
    L"mycomputer", // ICON_SWIRL -> "My Handheld PC"-style machine icon (the swirl is the fallback)
    L"cursor",     // ICON_CURSOR
    L"ie",         // ICON_IE
    L"start",      // ICON_START
    L"bin",        // ICON_BIN
    L"net",        // ICON_NET
    L"power",      // ICON_POWER
    L"programs",   // ICON_PROGRAMS
    L"favorites",  // ICON_FAVORITES
    L"documents",  // ICON_DOCUMENTS
    L"settings",   // ICON_SETTINGS
    L"help",       // ICON_HELP
    L"run",        // ICON_RUN
    L"suspend",    // ICON_SUSPEND
    L"controlpanel", // ICON_CPL
    L"taskbar",    // ICON_TASKBAR
    L"explorer",   // ICON_EXPLORER
    L"cmdprompt",  // ICON_CMD
};

// 16x16 icons as ASCII art (' ' = transparent).
static const char *s_aIconArt[ICON_COUNT][16] = {
    {// ICON_COMPUTER
     "", " KKKKKKKKKK", " KssssssssK", " KssssssssK", " KssssssssK", " KssssssssK", " KssssssssK",
     " KKKKKKKKKK", "    KKKK", "   KKKKKK", "  KLLLLLLK", "  KKKKKKKK", "", "", "", ""},
    {// ICON_DRIVE
     "", "     KKKK", "   KKccccKK", "  KcccWWcccK", "  KccWKKWccK", " KccWK  KWccK",
     " KccWK  KWccK", "  KccWKKWccK", "  KcccWWcccK", "   KKccccKK", "     KKKK", "", "", "", "",
     ""},
    {// ICON_FOLDER
     "", "", "  ooo", " oYYYo", "oYYYYYoooooo", "oYYYYYYYYYYo", "oYYYYYYYYYYo", "oYYYYYYYYYYo",
     "oYYYYYYYYYYo", "oYYYYYYYYYYo", "oooooooooooo", "", "", "", "", ""},
    {// ICON_APP
     "", " KKKKKKKKKKK", " KBBBBBBBBBK", " KKKKKKKKKKK", " KWWWWWWWWWK", " KWLLLLLLLWK",
     " KWLLLLLLLWK", " KWLLLLLLLWK", " KWWWWWWWWWK", " KKKKKKKKKKK", "", "", "", "", "", ""},
    {// ICON_CLOCK
     "", "     KKKK", "   KKWWWWKK", "  KWWWWWWWWK", " KWWWKWWWWWK", " KWWWKWWWWWK", " KWWWKKKWWWK",
     " KWWWWWWWWWK", " KWWWWWWWWWK", "  KWWWWWWWWK", "   KKWWWWKK", "     KKKK", "", "", "", ""},
    {// ICON_FILE
     "", "  KKKKKKK", "  KWWWWKKK", "  KWWWWKWK", "  KWWWWKKK", "  KWGGGGWK", "  KWGGGGWK",
     "  KWGGGGWK", "  KWGGGGWK", "  KWGGGGWK", "  KWWWWWWK", "  KKKKKKKK", "", "", "", ""},
    {// ICON_SWIRL
     "", "     OOOOO", "   OOOOOOOOO", "  OOO     OOO", " OOO   OO   OO", " OO  OOOOOO  O",
     " OO OO    OO O", " OO OO    O  O", " OO  OO     OO", " OOO  OOOOOOO", "  OOO      OO",
     "   OOOOOOOOO", "     OOOOO", "", "", ""},
    {// ICON_CURSOR
     "K", "KK", "KWK", "KWWK", "KWWWK", "KWWWWK", "KWWWWWK", "KWWWWWWK", "KWWWWWWWK", "KWWWWWKKK",
     "KWWKWWK", "KWK KWWK", "KK   KWWK", "      KWK", "       K", ""},
    {// ICON_IE - Internet Explorer globe (blue body, white meridian + equator)
     "", "     BBBBB", "   BBBBBBBBB", "  BBBBBWBBBBB", "  BBBBBWBBBBB", " BBBBBBWBBBBBB",
     " WWWWWWWWWWWWW", " BBBBBBWBBBBBB", "  BBBBBWBBBBB", "  BBBBBWBBBBB", "   BBBBBBBBB",
     "     BBBBB", "", "", "", ""},
    {// ICON_START - CE flag (waving 4-colour flag on a pole)
     " K", " KrrgYYbB", " K rrggYYbbBB", " KrrggYYbbBB", " KggYYbbBBrr", " KYYbbBBrrgg",
     " KbbBBrrggYY", " KBBrrggYYbb", " KrrggYYbb", " K", " K", " K", " K", "", "", ""},
    {// ICON_BIN - Recycle Bin (grey bin, green logo)
     "", "  KKKKKKKK", " KWGGGGGGWK", " KgGGGGGGgK", " KgGgGGgGgK", " KgGGgGGgGK",
     " KgGgGGgGgK", " KgGGgGGgGK", " KgGgGGgGgK", " KgGGGGGGgK", "  KKKKKKKK", "", "", "",
     "", ""},
    {// ICON_NET - two linked PCs
     "", " KKK  KKK", " KsK  KsK", " KKKKKKKK", "  K    K", "  K    K", " KKKKKKKK",
     " KsK    KsK", " KKK  KKK", "", "", "", "", "", "", ""},
    {// ICON_POWER - power plug
     "", "      KK", "     KWWK", "    KWWK", "   KWWK", "  KKWK", " KWWK", "KWWK",
     "KWK", "KK", "", "", "", "", "", ""},
    {// ICON_PROGRAMS - window w/ four app tiles
     "", "  oKKKKKKKKKK", " oYKggggggggK", "oYYKgggggggKK", "oYKgggggggKKK",
     "YKg KKKKKKKKKK", "YKg KBrrgbBBbK", "YKg KBrrgbBBbK", "YKg KBrrgbBBbK",
     "YKg KBYYgYBBYK", "YKg KKKKKKKKKK", " YKgggggggggK", "  KKKKKKKKKK", "", "", ""},
    {// ICON_FAVORITES - folder w/ blue star
     "", "", "  ooo", " oYYYo", "oYYYYYoooooo", "oYYYYYYYYYYo", "oYYYYBYYYYYo",
     "oYYBBBBBYYYo", "oYYYYBYYYYYo", "oYYBYYYYBYWo", "oYYYYYYYYYYo", "oooooooooooo",
     "", "", "", ""},
    {// ICON_DOCUMENTS - folder w/ pages
     "", "", "  WWWWW", " oYYWKoWWW", "oYYYYYWKoWWo", "oYYYYYYYYYYo", "oYYYYYYYYYYo",
     "oYYYYYYYYYYo", "oYYYYYYYYYYo", "oYYYYYYYYYYo", "oYYYYYYYYYYo", "oooooooooooo",
     "", "", "", ""},
    {// ICON_SETTINGS - window w/ gear
     "", "  KKKKKKKKK", " KWWWWWWWWK", " KWKKKKKKWK", " KWK KKK KWK",
     " KWKKYYYK KWK", " KWKYYKYYKWK", " KWKKYKYYKWK", " KWKYYKYYKWK", " KWKKYYYKWK",
     " KKKKKKKKKK", "    YYKYY", "", "", "", ""},
    {// ICON_HELP - book w/ ?
     "", "   KKKKKK", "  KrrrrrKK", " KrrrrrrrK", " KrKKKrKKKrK", " KrrrrKrrrK",
     " KrrrKKrrrK", " KrrrrKrrrK", " KrrrrKrrrK", " KrrrrrrrrK", "  KKKKKKKK",
     "   KWWWWK", "", "", "", ""},
    {// ICON_RUN - hourglass + window
     "", " KKKKKKKKKKK", " KBBBBBBBBBK", " KKKKKKKKKKK", " KWWWWWWWWWK",
     "  KKKK  KKK", "   KrK   KrK", "   KrrK KrrK", "   KrK   KrK",
     "  KKKK   KKK", " KWWWWK KWWWK", " KKKKKK KKKK", "", "", "", ""},
    {// ICON_SUSPEND - PC w/ moon screen
     "", "  KKKKKKKKK", " KGGGGGGGGK", " KGKKKKKKKGK", " KGKBBBYKGK",
     " KGKBBBYYGK", " KGKBBBYKGK", " KGKBBBKGGK", " KGKKKKKKKGK", " KGGGGGGGGK",
     "  KKKKKKKKK", "", "", "", "", ""},
    {// ICON_CPL - folder w/ tools
     "", "", "  ooo", " oYYYo", "oYYYYYoooooo", "oYYKKKKKKYYo", "oYYKrKKgKYYo",
     "oYYYKggKKYYo", "oYYKKKKrKYYo", "oYYgKKKKKYYo", "oYYYYYYYYYYo", "oooooooooooo",
     "", "", "", ""},
    {// ICON_TASKBAR - mini taskbar + flag
     "", " K", " KrrgYYbB", " KrrggYYbbBB", " KggYYbbBBrr", " KYYbbBBrrgg",
     " KbbBBrrggYY", " KBBrrggYYbb", " KKKKKKKKKKK", " KLLLLLLLLLK", " KKKBKKKKBKK",
     " KLLLLLLLLLK", " KKKKKKKKKKK", "", "", ""},
    {// ICON_EXPLORER - window + magnifier
     "", "  KKKKKKKK", " KBBBBBBBK", " KKKKKKKKK", " KWWWKKKWWK", " KWWKBBBKWK",
     " KWKBBBBBKKK", " KWKBBBBBKKKK", " KWKKBBBKKKKK", " KWWKKKWKKKK",
     " KKKKKKKKK KKK", "", "", "", "", ""},
    {// ICON_CMD - console window
     "", "  KKKKKKKKKK", " KBBBBBBBBBK", " KKKKKKKKKKK", " KWWWWWWWWWK",
     " KWKWWWWWWWK", " KWWSWWWWWWK", " KWWWWWWWWWK", " KWWWWWWWWWK", " KWWWWWWWWWK",
     " KKKKKKKKKKK", "", "", "", "", ""},
};

static COLORREF PalColor(char c)
{
	switch (c)
	{
		case 'K':
			return RGB(0, 0, 0);
		case 'W':
			return RGB(255, 255, 255);
		case 'D':
			return RGB(96, 96, 96);
		case 'G':
			return RGB(160, 160, 160);
		case 'L':
			return RGB(208, 208, 208);
		case 'Y':
			return RGB(255, 206, 90);
		case 'o':
			return RGB(200, 150, 70);
		case 'B':
			return RGB(40, 80, 200);
		case 'c':
			return RGB(80, 200, 220);
		case 's':
			return RGB(100, 160, 220);
		case 'g':
			return RGB(40, 200, 80);
		case 'r':
			return RGB(210, 60, 60);
		case 'O':
			return RGB(235, 125, 30);
		default:
			return RGB(255, 0, 255); // -> KEY
	}
}

// 565 -> ARGB4444; the color key becomes fully transparent.
static WORD Conv4444(WORD wPx565)
{
	int r5, g6, b5;
	if (wPx565 == KEY)
		return 0x0000;
	r5 = (wPx565 >> 11) & 0x1F;
	g6 = (wPx565 >> 5) & 0x3F;
	b5 = wPx565 & 0x1F;
	return (WORD)(0xF000 | ((r5 >> 1) << 8) | ((g6 >> 2) << 4) | (b5 >> 1));
}

// --- clip rect (compositor-side window clipping) --------------------------------
static float s_fClipX0, s_fClipY0, s_fClipX1, s_fClipY1;
static int s_nClipOn = 0;
void GfxSetClip(int x0, int y0, int x1, int y1)
{
	s_fClipX0 = (float)x0;
	s_fClipY0 = (float)y0;
	s_fClipX1 = (float)x1;
	s_fClipY1 = (float)y1;
	s_nClipOn = 1;
}
void GfxClearClip(void)
{
	s_nClipOn = 0;
}

// --- dynamic quad arrays --------------------------------------------------------
// Grow the three parallel arrays (verts / indices / tex-flags) to hold >= need quads, preserving
// the first `keep`. Doubling from QUAD_INIT -> amortized O(1) across a frame. Returns 0 only at the
// QUAD_MAX ceiling or on OOM (PushQuad then drops, which no real scene reaches).
static int GrowQuads(D3DTLVERTEX **pVb, WORD **pIb, BYTE **pQt, int *pCap, int nNeed, int nKeep)
{
	int nc;
	D3DTLVERTEX *pNvb;
	WORD *pNib;
	BYTE *pNqt;
	if (nNeed <= *pCap)
		return 1;
	if (nNeed > QUAD_MAX)
		return 0;
	nc = *pCap ? *pCap : QUAD_INIT;
	while (nc < nNeed)
		nc <<= 1;
	if (nc > QUAD_MAX)
		nc = QUAD_MAX;
	pNvb = (D3DTLVERTEX *)LocalAlloc(LMEM_FIXED, (DWORD)nc * 4 * sizeof(D3DTLVERTEX));
	pNib = (WORD *)LocalAlloc(LMEM_FIXED, (DWORD)nc * 6 * sizeof(WORD));
	pNqt = (BYTE *)LocalAlloc(LMEM_FIXED, (DWORD)nc);
	if (!pNvb || !pNib || !pNqt)
	{
		if (pNvb)
			LocalFree(pNvb);
		if (pNib)
			LocalFree(pNib);
		if (pNqt)
			LocalFree(pNqt);
		return 0;
	}
	if (nKeep > 0 && *pVb)
	{
		memcpy(pNvb, *pVb, (DWORD)nKeep * 4 * sizeof(D3DTLVERTEX));
		memcpy(pNib, *pIb, (DWORD)nKeep * 6 * sizeof(WORD));
		memcpy(pNqt, *pQt, (DWORD)nKeep);
	}
	// The scene starts on the static working buffer; never LocalFree that (only heap overflow).
	if (*pVb && *pVb != s_aSceneVb)
		LocalFree(*pVb);
	if (*pIb && *pIb != s_aSceneIb)
		LocalFree(*pIb);
	if (*pQt && *pQt != s_aSceneQt)
		LocalFree(*pQt);
	*pVb = pNvb;
	*pIb = pNib;
	*pQt = pNqt;
	*pCap = nc;
	return 1;
}

// Reclaim the live scene after it has stayed small: hand the heap overflow back and return to the
// static working buffer (the static buffer itself is never freed). No content copy - the frame is
// drawn and the next rebuilds from scratch. No-op if already on the static buffer or still oversized.
static void ShrinkScene(int nWant)
{
	if (s_pVb == s_aSceneVb || nWant > QUAD_WORK)
		return;
	LocalFree(s_pVb);
	LocalFree(s_pIb);
	LocalFree(s_pQtex);
	s_pVb = s_aSceneVb;
	s_pIb = s_aSceneIb;
	s_pQtex = s_aSceneQt;
	s_nCap = QUAD_WORK;
}

// --- quad list ------------------------------------------------------------------
static void PushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                     D3DCOLOR dwCol, BYTE bTex)
{
	D3DTLVERTEX *pV;
	WORD *pIx;
	int nBase, nIdx;
	// All quads coplanar at z=0 (we paint back-to-front: desktop -> windows -> taskbar -> cursor).
	// (The per-quad "nearer z" depth trick was reverted - it pushed the first quads to z~1.0, the
	// far-clip plane, and they got culled on real hardware -> black desktop.)
	float fZ = 0.0f;
	if (!GrowQuads(&s_pVb, &s_pIb, &s_pQtex, &s_nCap, s_nQuad + 1, s_nQuad))
		return; // only at the ceiling
	// Software clip to the active clip rect, adjusting UVs so textured quads (glyphs/icons)
	// clip cleanly instead of spilling outside a window's client area.
	if (s_nClipOn)
	{
		float cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
		if (cx0 < s_fClipX0)
			cx0 = s_fClipX0;
		if (cy0 < s_fClipY0)
			cy0 = s_fClipY0;
		if (cx1 > s_fClipX1)
			cx1 = s_fClipX1;
		if (cy1 > s_fClipY1)
			cy1 = s_fClipY1;
		if (cx1 <= cx0 || cy1 <= cy0)
			return;             // fully outside
		if (x1 > x0 && y1 > y0) // re-map UVs to the clipped extent
		{
			float du = (u1 - u0), dv = (v1 - v0);
			float nu0 = u0 + du * (cx0 - x0) / (x1 - x0), nu1 = u0 + du * (cx1 - x0) / (x1 - x0);
			float nv0 = v0 + dv * (cy0 - y0) / (y1 - y0), nv1 = v0 + dv * (cy1 - y0) / (y1 - y0);
			u0 = nu0;
			u1 = nu1;
			v0 = nv0;
			v1 = nv1;
		}
		x0 = cx0;
		y0 = cy0;
		x1 = cx1;
		y1 = cy1;
	}
	nBase = s_nQuad * 4;
	pV = &s_pVb[nBase];
	pV[0].sx = x0;
	pV[0].sy = y0;
	pV[0].sz = fZ;
	pV[0].rhw = 1;
	pV[0].color = dwCol;
	pV[0].specular = 0;
	pV[0].tu = u0;
	pV[0].tv = v0;
	pV[1].sx = x1;
	pV[1].sy = y0;
	pV[1].sz = fZ;
	pV[1].rhw = 1;
	pV[1].color = dwCol;
	pV[1].specular = 0;
	pV[1].tu = u1;
	pV[1].tv = v0;
	pV[2].sx = x0;
	pV[2].sy = y1;
	pV[2].sz = fZ;
	pV[2].rhw = 1;
	pV[2].color = dwCol;
	pV[2].specular = 0;
	pV[2].tu = u0;
	pV[2].tv = v1;
	pV[3].sx = x1;
	pV[3].sy = y1;
	pV[3].sz = fZ;
	pV[3].rhw = 1;
	pV[3].color = dwCol;
	pV[3].specular = 0;
	pV[3].tu = u1;
	pV[3].tv = v1;
	nIdx = s_nQuad * 6;
	pIx = &s_pIb[nIdx];
	pIx[0] = (WORD)nBase;
	pIx[1] = (WORD)(nBase + 1);
	pIx[2] = (WORD)(nBase + 2);
	pIx[3] = (WORD)(nBase + 1);
	pIx[4] = (WORD)(nBase + 3);
	pIx[5] = (WORD)(nBase + 2);
	s_pQtex[s_nQuad] = bTex;
	s_nQuad++;
}

// Load \Windows\dcico_<name>.ico and write the nDim image into the atlas cell for `id`
// (16px row at y=144, 32px row at y=160, column = id * cell width). Returns 0 when the file
// is absent/undecodable - the caller then falls back to the ASCII art.
static int IcoAtlasCell(int id, int nDim, BYTE *pAb, int nAp)
{
	WCHAR szPath[64];
	HANDLE hf;
	BYTE *pb = NULL;
	static WORD awPx[32 * 32]; // one cell, ARGB4444
	DWORD dwSize, dwRead = 0;
	int x, y, cx, cy, ok = 0;

	wsprintfW(szPath, L"\\Windows\\dcico_%s.ico", s_aIcoName[id]);
	hf = CreateFileW(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                 FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE)
		return 0;
	dwSize = GetFileSize(hf, NULL);
	if (dwSize > 6 && dwSize < 64 * 1024)
		pb = (BYTE *)LocalAlloc(LMEM_FIXED, dwSize);
	if (pb && ReadFile(hf, pb, dwSize, &dwRead, NULL) && dwRead == dwSize &&
	    IcoTo4444(pb, dwSize, nDim, awPx))
	{
		cx = (nDim == 16) ? id * 16 : id * 32;
		cy = (nDim == 16) ? 144 : 160;
		for (y = 0; y < nDim; y++)
			for (x = 0; x < nDim; x++)
				*((WORD *)(pAb + (cy + y) * nAp) + (cx + x)) = awPx[y * nDim + x];
		ok = 1;
	}
	if (pb)
		LocalFree(pb);
	CloseHandle(hf);
	return ok;
}

// --- atlas build (one-time): glyphs via GDI raster + icons, upload to VRAM -------
static void BuildAtlas(void)
{
	DDSURFACEDESC sd, ld, lt;
	LPDIRECTDRAWSURFACE pStg = NULL, pTmp = NULL, pDst = NULL;
	LPDIRECT3DTEXTURE2 pStgTex = NULL;
	HFONT aFonts[3];
	HDC hdc;
	BYTE *pAb;
	int nAp, id, x, y, sx, sy, scale, f, c, nCols, n;

	s_bGlyphReady = FALSE;
	s_hAtlas = 0;
	if (!s_pDd || !s_pDev)
		return;

	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
	sd.dwWidth = ATLAS_DIM;
	sd.dwHeight = ATLAS_DIM;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0x0F00;
	sd.ddpfPixelFormat.dwGBitMask = 0x00F0;
	sd.ddpfPixelFormat.dwBBitMask = 0x000F;
	sd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xF000;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &pStg, NULL) != DD_OK || !pStg)
	{
		OutputDebugStringW(L"atlas: staging create FAIL\r\n");
		return;
	}

	// icons -> atlas (16px row at y=144, 32px row at y=160)
	memset(&ld, 0, sizeof(ld));
	ld.dwSize = sizeof(ld);
	if (IDirectDrawSurface_Lock(pStg, NULL, &ld, DDLOCK_WAIT, NULL) != DD_OK)
	{
		IDirectDrawSurface_Release(pStg);
		return;
	}
	pAb = (BYTE *)ld.lpSurface;
	nAp = ld.lPitch;
	for (y = 0; y < ATLAS_DIM; y++)
		memset(pAb + y * nAp, 0, ATLAS_DIM * 2);
	for (id = 0; id < ICON_COUNT; id++)
		for (scale = 1; scale <= 2; scale++)
		{
			int dim = 16 * scale, cx = (scale == 1) ? id * 16 : id * 32,
			    cy = (scale == 1) ? 144 : 160;
			RectUV *pU = &s_aIconUV[id][scale - 1];
			pU->u0 = cx / (float)ATLAS_DIM;
			pU->v0 = cy / (float)ATLAS_DIM;
			pU->u1 = (cx + dim) / (float)ATLAS_DIM;
			pU->v1 = (cy + dim) / (float)ATLAS_DIM;
			if (IcoAtlasCell(id, dim, pAb, nAp)) // real CE icon from \Windows\dcico_*.ico
				continue;
			for (y = 0; y < 16; y++)
			{
				const char *pszRow = s_aIconArt[id][y];
				for (n = 0; pszRow[n]; n++)
					;
				for (x = 0; x < 16; x++)
				{
					WORD t = Conv4444(ToRgb565(PalColor((x < n) ? pszRow[x] : ' ')));
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							*((WORD *)(pAb + (cy + y * scale + sy) * nAp) + (cx + x * scale + sx)) =
							    t;
				}
			}
		}
	IDirectDrawSurface_Unlock(pStg, NULL);

	// glyphs: render the 3 fonts to a temp 565 surface, read coverage -> atlas alpha
	nCols = ATLAS_DIM / 16;
	aFonts[0] = g_FontUI;
	aFonts[1] = g_FontBold;
	aFonts[2] = g_FontTitle;
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
	sd.dwWidth = ATLAS_DIM;
	sd.dwHeight = 160;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0xF800;
	sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
	sd.ddpfPixelFormat.dwBBitMask = 0x001F;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &pTmp, NULL) == DD_OK && pTmp)
	{
		memset(&lt, 0, sizeof(lt));
		lt.dwSize = sizeof(lt);
		if (IDirectDrawSurface_Lock(pTmp, NULL, &lt, DDLOCK_WAIT, NULL) == DD_OK)
		{
			for (y = 0; y < 160; y++)
				memset((BYTE *)lt.lpSurface + y * lt.lPitch, 0, ATLAS_DIM * 2);
			IDirectDrawSurface_Unlock(pTmp, NULL);
		}
		if (IDirectDrawSurface_GetDC(pTmp, &hdc) == DD_OK && hdc)
		{
			SetBkColor(hdc, RGB(0, 0, 0));
			SetTextColor(hdc, RGB(255, 255, 255));
			SetBkMode(hdc, OPAQUE);
			for (f = 0; f < 3; f++)
			{
				SelectObject(hdc, aFonts[f]);
				for (c = 0; c < GN; c++)
				{
					int idx = f * GN + c;
					WCHAR ch = (WCHAR)(GFIRST + c);
					RECT rcC;
					rcC.left = (idx % nCols) * 16;
					rcC.top = (idx / nCols) * 16;
					rcC.right = rcC.left + 16;
					rcC.bottom = rcC.top + 16;
					// clip to the cell: glyphs with a negative left bearing ('j',
					// 'A', '/') spill ink into the previous cell otherwise
					ExtTextOutW(hdc, rcC.left, rcC.top, ETO_CLIPPED, &rcC, &ch, 1, NULL);
				}
			}
			IDirectDrawSurface_ReleaseDC(pTmp, hdc);
		}
		memset(&ld, 0, sizeof(ld));
		ld.dwSize = sizeof(ld);
		memset(&lt, 0, sizeof(lt));
		lt.dwSize = sizeof(lt);
		if (IDirectDrawSurface_Lock(pStg, NULL, &ld, DDLOCK_WAIT, NULL) == DD_OK)
		{
			pAb = (BYTE *)ld.lpSurface;
			nAp = ld.lPitch;
			if (IDirectDrawSurface_Lock(pTmp, NULL, &lt, DDLOCK_WAIT, NULL) == DD_OK)
			{
				BYTE *pTb = (BYTE *)lt.lpSurface;
				int nTp = lt.lPitch;
				for (f = 0; f < 3; f++)
					for (c = 0; c < GN; c++)
					{
						int idx = f * GN + c, gx = (idx % nCols) * 16, gy = (idx / nCols) * 16,
						    w = 0;
						GlyphUV *pG = &s_aGlyph[f][c];
						for (y = 0; y < 16; y++)
						{
							WORD *pTr = (WORD *)(pTb + (gy + y) * nTp);
							WORD *pAr = (WORD *)(pAb + (gy + y) * nAp);
							for (x = 0; x < 16; x++)
							{
								int a4 =
								    ((pTr[gx + x] >> 11) & 0x1F) >> 1; // luminance -> 4-bit alpha
								if (a4)
								{
									pAr[gx + x] = (WORD)((a4 << 12) | 0x0FFF);
									if (x + 1 > w)
										w = x + 1;
								}
							}
						}
						pG->u0 = gx / (float)ATLAS_DIM;
						pG->v0 = gy / (float)ATLAS_DIM;
						pG->u1 = (gx + 16) / (float)ATLAS_DIM;
						pG->v1 = (gy + 16) / (float)ATLAS_DIM;
						pG->adv = (BYTE)(w ? w + 1 : 4);
					}
				IDirectDrawSurface_Unlock(pTmp, NULL);
				s_bGlyphReady = TRUE;
			}
			IDirectDrawSurface_Unlock(pStg, NULL);
		}
		IDirectDrawSurface_Release(pTmp);
	}

	// upload staging -> VRAM texture
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ALLOCONLOAD;
	sd.dwWidth = ATLAS_DIM;
	sd.dwHeight = ATLAS_DIM;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0x0F00;
	sd.ddpfPixelFormat.dwGBitMask = 0x00F0;
	sd.ddpfPixelFormat.dwBBitMask = 0x000F;
	sd.ddpfPixelFormat.dwRGBAlphaBitMask = 0xF000;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &pDst, NULL) == DD_OK && pDst)
	{
		if (IDirectDrawSurface_QueryInterface(pStg, &IID_IDirect3DTexture2, (void **)&pStgTex) ==
		        DD_OK &&
		    IDirectDrawSurface_QueryInterface(pDst, &IID_IDirect3DTexture2,
		                                      (void **)&s_pAtlasTex) == DD_OK &&
		    IDirect3DTexture2_Load(s_pAtlasTex, pStgTex) == DD_OK &&
		    IDirect3DTexture2_GetHandle(s_pAtlasTex, s_pDev, &s_hAtlas) == DD_OK)
		{
			s_pAtlasSurf = pDst;
			OutputDebugStringW(L"atlas: VRAM upload OK, handle bound\r\n");
		}
		else
		{
			OutputDebugStringW(L"atlas: QI/Load/GetHandle FAIL\r\n");
			if (s_pAtlasTex)
			{
				IDirect3DTexture2_Release(s_pAtlasTex);
				s_pAtlasTex = NULL;
			}
			IDirectDrawSurface_Release(pDst);
		}
		if (pStgTex)
			IDirect3DTexture2_Release(pStgTex);
	}
	else
		OutputDebugStringW(L"atlas: VRAM dest create FAIL\r\n");
	IDirectDrawSurface_Release(pStg);
}

static void D3DLog(const WCHAR *pszWhat, HRESULT hr)
{
	WCHAR b[96];
	wsprintfW(b, L"D3D: %s hr=%08x\r\n", pszWhat, (unsigned)hr);
	OutputDebugStringW(b);
}

static void InitD3D(void)
{
	HRESULT hr;
	D3DVIEWPORT2 vp;

	s_bD3dOk = FALSE;
	if (!s_pDd || !s_pBack)
	{
		OutputDebugStringW(L"D3D: no flip back buffer\r\n");
		return;
	}

	hr = IDirectDraw_QueryInterface(s_pDd, &IID_IDirect3D2, (void **)&s_pD3d);
	D3DLog(L"QI IDirect3D2", hr);
	if (hr != DD_OK || !s_pD3d)
		return;
	hr = IDirect3D2_CreateDevice(s_pD3d, &IID_IDirect3DHALDevice, s_pBack, &s_pDev);
	D3DLog(L"CreateDevice(HAL)", hr);
	if (hr != DD_OK || !s_pDev)
		return;
	hr = IDirect3D2_CreateViewport(s_pD3d, &s_pVp, NULL);
	D3DLog(L"CreateViewport", hr);
	if (hr != DD_OK || !s_pVp)
		return;

	IDirect3DDevice2_AddViewport(s_pDev, s_pVp);
	memset(&vp, 0, sizeof(vp));
	vp.dwSize = sizeof(vp);
	vp.dwWidth = SCREEN_W;
	vp.dwHeight = SCREEN_H;
	vp.dvClipX = -1.0f;
	vp.dvClipY = 1.0f;
	vp.dvClipWidth = 2.0f;
	vp.dvClipHeight = 2.0f;
	vp.dvMinZ = 0.0f;
	vp.dvMaxZ = 1.0f;
	IDirect3DViewport2_SetViewport2(s_pVp, &vp);
	IDirect3DDevice2_SetCurrentViewport(s_pDev, s_pVp);

	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_ZENABLE, FALSE);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_SHADEMODE, D3DSHADE_FLAT);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_TEXTUREMAPBLEND,
	                                D3DTBLEND_MODULATEALPHA);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_NEAREST);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_NEAREST);
	IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_TEXTUREHANDLE, 0);

	s_bD3dOk = TRUE;
	OutputDebugStringW(L"D3D: device UP - PVR2 HW compositor\r\n");
}

static void DestroyD3D(void)
{
	if (s_pAtlasTex)
	{
		IDirect3DTexture2_Release(s_pAtlasTex);
		s_pAtlasTex = NULL;
	}
	if (s_pAtlasSurf)
	{
		IDirectDrawSurface_Release(s_pAtlasSurf);
		s_pAtlasSurf = NULL;
	}
	s_hAtlas = 0;
	if (s_pDev && s_pVp)
		IDirect3DDevice2_DeleteViewport(s_pDev, s_pVp);
	if (s_pVp)
	{
		IDirect3DViewport2_Release(s_pVp);
		s_pVp = NULL;
	}
	if (s_pDev)
	{
		IDirect3DDevice2_Release(s_pDev);
		s_pDev = NULL;
	}
	if (s_pD3d)
	{
		IDirect3D2_Release(s_pD3d);
		s_pD3d = NULL;
	}
	s_bD3dOk = FALSE;
}

static BOOL CreateSurfaces(void)
{
	DDSURFACEDESC ddsd;
	HRESULT hr = E_FAIL;
	int nTries;

	for (nTries = 0; nTries < 30; nTries++)
	{
		hr = DirectDrawCreate(NULL, &s_pDd, NULL);
		if (hr == DD_OK)
			break;
		s_pDd = NULL;
		Sleep(100);
	}
	if (hr != DD_OK)
		return FALSE;

	IDirectDraw_SetCooperativeLevel(s_pDd, s_hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
	IDirectDraw_SetDisplayMode(s_pDd, SCREEN_W, SCREEN_H, 16);

	memset(&ddsd, 0, sizeof(ddsd));
	ddsd.dwSize = sizeof(ddsd);
	ddsd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
	ddsd.ddsCaps.dwCaps =
	    DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
	ddsd.dwBackBufferCount = 1;
	hr = IDirectDraw_CreateSurface(s_pDd, &ddsd, &s_pPrimary, NULL);
	if (hr == DD_OK)
	{
		DDSCAPS caps;
		memset(&caps, 0, sizeof(caps));
		caps.dwCaps = DDSCAPS_BACKBUFFER;
		if (IDirectDrawSurface_GetAttachedSurface(s_pPrimary, &caps, &s_pBack) == DD_OK)
			s_bUseFlip = TRUE;
	}
	if (!s_bUseFlip)
	{
		WCHAR b[64];
		wsprintfW(b, L"GfxInit: flip+3DDEVICE primary FAIL hr=%08x\r\n", (unsigned)hr);
		OutputDebugStringW(b);
		return FALSE;
	}
	return TRUE;
}

static void DestroySurfaces(void)
{
	s_hPage = 0; // page layer (browser only; NULL elsewhere)
	if (s_pPageTex)
	{
		IDirect3DTexture2_Release(s_pPageTex);
		s_pPageTex = NULL;
	}
	if (s_pPageSurf)
	{
		IDirectDrawSurface_Release(s_pPageSurf);
		s_pPageSurf = NULL;
	}
	s_hWall = 0; // wallpaper texture (re-uploaded by GfxReloadWallpaper after CreateSurfaces)
	if (s_pWallTex)
	{
		IDirect3DTexture2_Release(s_pWallTex);
		s_pWallTex = NULL;
	}
	if (s_pWallSurf)
	{
		IDirectDrawSurface_Release(s_pWallSurf);
		s_pWallSurf = NULL;
	}
	s_hBan = 0; // banner texture (re-uploaded by GfxReloadBanner after CreateSurfaces)
	if (s_pBanTex)
	{
		IDirect3DTexture2_Release(s_pBanTex);
		s_pBanTex = NULL;
	}
	if (s_pBanSurf)
	{
		IDirectDrawSurface_Release(s_pBanSurf);
		s_pBanSurf = NULL;
	}
	if (s_pGdiSurf)
	{
		IDirectDrawSurface_Release(s_pGdiSurf);
		s_pGdiSurf = NULL;
	}
	s_pBack = NULL;
	s_bUseFlip = FALSE;
	if (s_pPrimary)
	{
		IDirectDrawSurface_Release(s_pPrimary);
		s_pPrimary = NULL;
	}
	if (s_pDd)
	{
		IDirectDraw_RestoreDisplayMode(s_pDd);
		IDirectDraw_Release(s_pDd);
		s_pDd = NULL;
	}
}

BOOL GfxInit(HWND hwnd)
{
	s_hwnd = hwnd;
	if (!CreateSurfaces())
		return FALSE;
	InitD3D();
	if (!s_bD3dOk)
		return FALSE; // hard requirement now (no CPU fallback path)
	AddFontResourceW(L"\\Windows\\tahoma.ttf"); // stock CE UI face (no-op if already registered)
	g_FontUI = MakeFont(12, FW_NORMAL);
	g_FontBold = MakeFont(12, FW_BOLD);
	g_FontTitle = MakeFont(14, FW_BOLD);
	__try
	{
		BuildAtlas();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_bGlyphReady = FALSE;
		OutputDebugStringW(L"GfxInit: BuildAtlas faulted\r\n");
	}
	return TRUE;
}

// Hand back the heap quad buffers while the shell isn't drawing (e.g. a fullscreen app owns the
// screen). The scene returns to its static working buffer (never freed - so the desktop always
// redraws); only the heap overflow + the whole desktop cache (which regrows on demand) are released.
void GfxFreeQuads(void)
{
	if (s_pVb != s_aSceneVb)
	{
		LocalFree(s_pVb);
		LocalFree(s_pIb);
		LocalFree(s_pQtex);
	}
	s_pVb = s_aSceneVb;
	s_pIb = s_aSceneIb;
	s_pQtex = s_aSceneQt;
	s_nCap = QUAD_WORK;
	s_nQuad = 0;
	if (s_pDvb)
		LocalFree(s_pDvb);
	if (s_pDib)
		LocalFree(s_pDib);
	if (s_pDtex)
		LocalFree(s_pDtex);
	s_pDvb = NULL;
	s_pDib = NULL;
	s_pDtex = NULL;
	s_nDcap = s_nDQuad = 0;
}

void GfxShutdown(void)
{
	DestroyD3D();
	DestroySurfaces();
	GfxFreeQuads();
}

// --- public draw API: enqueue quads (no pixels touched) -------------------------
void GfxFill(int left, int top, int right, int bottom, COLORREF color)
{
	if (right <= left || bottom <= top)
		return;
	PushQuad((float)left, (float)top, (float)right, (float)bottom, 0, 0, 0, 0, ToArgb(color), 0);
}

void GfxBevel(const RECT *rc, BOOL raised)
{
	// DrawEdge EDGE_RAISED/EDGE_SUNKEN: two bands, outer (TL hilight/BR dkshadow)
	// then inner (TL 3dlight/BR 3dshadow); the stock CE control look
	COLORREF tl1 = raised ? RGB(255, 255, 255) : RGB(128, 128, 128);
	COLORREF br1 = raised ? RGB(0, 0, 0) : RGB(255, 255, 255);
	COLORREF tl2 = raised ? RGB(223, 223, 223) : RGB(0, 0, 0);
	COLORREF br2 = raised ? RGB(128, 128, 128) : RGB(223, 223, 223);
	GfxFill(rc->left, rc->top, rc->right, rc->top + 1, tl1);
	GfxFill(rc->left, rc->top, rc->left + 1, rc->bottom, tl1);
	GfxFill(rc->left, rc->bottom - 1, rc->right, rc->bottom, br1);
	GfxFill(rc->right - 1, rc->top, rc->right, rc->bottom, br1);
	GfxFill(rc->left + 1, rc->top + 1, rc->right - 1, rc->top + 2, tl2);
	GfxFill(rc->left + 1, rc->top + 1, rc->left + 2, rc->bottom - 1, tl2);
	GfxFill(rc->left + 1, rc->bottom - 2, rc->right - 1, rc->bottom - 1, br2);
	GfxFill(rc->right - 2, rc->top + 1, rc->right - 1, rc->bottom - 1, br2);
}

void GfxIcon(int id, int x, int y)
{
	RectUV *pU;
	if (id < 0 || id >= ICON_COUNT)
		return;
	pU = &s_aIconUV[id][0];
	PushQuad((float)x, (float)y, (float)(x + 16), (float)(y + 16), pU->u0, pU->v0, pU->u1, pU->v1,
	         0xFFFFFFFF, 1);
}

void GfxIconBig(int id, int x, int y)
{
	RectUV *pU;
	if (id < 0 || id >= ICON_COUNT)
		return;
	pU = &s_aIconUV[id][1];
	PushQuad((float)x, (float)y, (float)(x + 32), (float)(y + 32), pU->u0, pU->v0, pU->u1, pU->v1,
	         0xFFFFFFFF, 1);
}

HDC GfxLockDC(void)
{
	return (HDC)1;
} // text targets the quad list now
void GfxUnlockDC(HDC hdc)
{
	(void)hdc;
}

void GfxText(HDC hdc, int x, int y, COLORREF fg, COLORREF bg, HFONT font, const WCHAR *text)
{
	int nFi = (font == g_FontBold) ? 1 : (font == g_FontTitle) ? 2 : 0;
	D3DCOLOR dwFgc = ToArgb(fg), dwBgc = ToArgb(bg);
	const WCHAR *p;
	int nRunW = 0, nCx;

	(void)hdc;
	if (!s_bGlyphReady)
		return;
	for (p = text; *p; p++)
	{
		WCHAR ch = (*p < GFIRST || *p > GLAST) ? '?' : *p;
		nRunW += s_aGlyph[nFi][ch - GFIRST].adv;
	}
	// One opaque bg quad behind the run - UNLESS the caller asked for a transparent background
	// (GFX_TRANSPARENT), e.g. desktop icon labels over the wallpaper. Glyphs alpha-blend either
	// way.
	if (bg != GFX_TRANSPARENT && nRunW > 0)
		PushQuad((float)x, (float)y, (float)(x + nRunW), (float)(y + GH), 0, 0, 0, 0, dwBgc, 0);
	nCx = x;
	for (p = text; *p; p++)
	{
		WCHAR ch = (*p < GFIRST || *p > GLAST) ? '?' : *p;
		GlyphUV *pG = &s_aGlyph[nFi][ch - GFIRST];
		PushQuad((float)nCx, (float)y, (float)(nCx + 16), (float)(y + GH), pG->u0, pG->v0, pG->u1,
		         pG->v1, dwFgc, 1);
		nCx += pG->adv;
	}
}

int GfxTextWidth(HFONT font, const WCHAR *text)
{
	int nFi = (font == g_FontBold) ? 1 : (font == g_FontTitle) ? 2 : 0, nW = 0;
	const WCHAR *p;
	if (!s_bGlyphReady)
		return 0;
	for (p = text; *p; p++)
	{
		WCHAR ch = (*p < GFIRST || *p > GLAST) ? '?' : *p;
		nW += s_aGlyph[nFi][ch - GFIRST].adv;
	}
	return nW;
}

// --- desktop cache as a vertex sub-list -----------------------------------------
void GfxBeginDesktopCache(void)
{
	s_bRecDesk = TRUE;
	s_nQuad = 0; // record desktop quads from index 0
}

void GfxEndDesktopCache(void)
{
	s_nDQuad = s_nQuad;
	if (!GrowQuads(&s_pDvb, &s_pDib, &s_pDtex, &s_nDcap, s_nDQuad, 0))
		s_nDQuad = s_nDcap; // fit the desktop
	if (s_nDQuad > 0)
	{
		memcpy(s_pDvb, s_pVb, s_nDQuad * 4 * sizeof(D3DTLVERTEX));
		memcpy(s_pDib, s_pIb, s_nDQuad * 6 * sizeof(WORD));
		memcpy(s_pDtex, s_pQtex, s_nDQuad);
	}
	s_bRecDesk = FALSE;
	s_nQuad = 0;
}

void GfxBlitDesktopCache(void)
{
	if (s_nDQuad <= 0)
	{
		s_nQuad = 0;
		return;
	}
	if (!GrowQuads(&s_pVb, &s_pIb, &s_pQtex, &s_nCap, s_nDQuad, 0))
	{
		s_nQuad = 0;
		return;
	} // room for it
	memcpy(s_pVb, s_pDvb, s_nDQuad * 4 * sizeof(D3DTLVERTEX));
	memcpy(s_pIb, s_pDib,
	       s_nDQuad * 6 * sizeof(WORD)); // base-relative indices stay valid at slot 0
	memcpy(s_pQtex, s_pDtex, s_nDQuad);
	s_nQuad = s_nDQuad;
}

// --- present: the only D3D-talking path -----------------------------------------
static int s_nGhostIcon = -1;
void GfxSetDragGhost(int iconId)
{
	s_nGhostIcon = iconId;
}

BOOL GfxPresent(int cursorX, int cursorY, BOOL showCursor)
{
	int i;
	if (!s_bD3dOk || !s_pDev || !s_pPrimary)
		return FALSE;

	if (s_nGhostIcon >= 0 &&
	    s_nGhostIcon < ICON_COUNT) // drag ghost: translucent 32x32 under cursor
	{
		RectUV *pU = &s_aIconUV[s_nGhostIcon][1];
		int gx = cursorX - 6, gy = cursorY - 4; // carried just below-right of the pointer tip
		PushQuad((float)gx, (float)gy, (float)(gx + 32), (float)(gy + 32), pU->u0, pU->v0, pU->u1,
		         pU->v1, 0xB0FFFFFF, 1); // ~0.69 alpha (blend is SRCALPHA)
	}

	if (showCursor) // cursor = ICON_CURSOR quad, drawn last (top)
	{
		RectUV *pU = &s_aIconUV[ICON_CURSOR][1]; // the stock CE arrow is 32x32
		float x1, y1;
		// Clamp the hotspot on-screen and clip the 32x32 sprite to the screen so the pointer
		// never draws past the edges (the arrow's tip is the top-left hotspot, so it can sit
		// right at an edge; we just don't extend the quad beyond the framebuffer).
		if (cursorX < 0)
			cursorX = 0;
		else if (cursorX > SCREEN_W - 1)
			cursorX = SCREEN_W - 1;
		if (cursorY < 0)
			cursorY = 0;
		else if (cursorY > SCREEN_H - 1)
			cursorY = SCREEN_H - 1;
		x1 = (float)(cursorX + 32);
		if (x1 > SCREEN_W)
			x1 = SCREEN_W;
		y1 = (float)(cursorY + 32);
		if (y1 > SCREEN_H)
			y1 = SCREEN_H;
		PushQuad((float)cursorX, (float)cursorY, x1, y1, pU->u0, pU->v0,
		         pU->u0 +
		             (pU->u1 - pU->u0) * (x1 - cursorX) / 32.0f, // scale UV to the clipped extent
		         pU->v0 + (pU->v1 - pU->v0) * (y1 - cursorY) / 32.0f, 0xFFFFFFFF, 1);
	}

	if (IDirect3DDevice2_BeginScene(s_pDev) == D3D_OK)
	{
		i = 0;
		while (i < s_nQuad) // emit-order batches: run per texture state
		{
			BYTE bTex = s_pQtex[i];
			int j = i;
			D3DTEXTUREHANDLE th;
			while (j < s_nQuad && s_pQtex[j] == bTex)
				j++;
			th = (bTex == 4)   ? s_hBan
			     : (bTex == 3) ? s_hWall
			     : (bTex == 2) ? s_hPage
			     : (bTex == 1) ? s_hAtlas
			                   : 0; // 4 = banner, 3 = wallpaper, 2 = page, 1 = atlas, 0 = solid
			IDirect3DDevice2_SetRenderState(s_pDev, D3DRENDERSTATE_TEXTUREHANDLE, th);
			// Pass ONLY this run's vertices (&s_pVb[i*4], (j-i)*4) with the base-relative
			// index template s_pIb[0..]. Passing the whole array each batch made the TA
			// re-transform every vertex per batch -> O(verts x batches) -> the 56ms spike.
			IDirect3DDevice2_DrawIndexedPrimitive(s_pDev, D3DPT_TRIANGLELIST, D3DVT_TLVERTEX,
			                                      &s_pVb[i * 4], (j - i) * 4, s_pIb, (j - i) * 6,
			                                      D3DDP_DONOTCLIP | D3DDP_DONOTLIGHT);
			i = j;
		}
		IDirect3DDevice2_EndScene(s_pDev);
	}

	{ // reclaim RAM when the scene has stayed well under capacity (hysteresis: a ~120-frame window
		// so a brief burst of windows doesn't thrash grow<->shrink). want = recent peak + ~50%
		// slack.
		static int nPeak, nFrames;
		if (s_nQuad > nPeak)
			nPeak = s_nQuad;
		if (++nFrames >= 120)
		{
			int nWant = nPeak + (nPeak >> 1) + 32;
			if (s_pVb != s_aSceneVb && nWant <= QUAD_WORK)
				ShrinkScene(nWant); // drop heap overflow, return to the static working buffer
			nPeak = 0;
			nFrames = 0;
		}
	}
	s_nQuad = 0;

	if (IDirectDrawSurface_Flip(s_pPrimary, NULL, DDFLIP_WAIT) == DDERR_SURFACELOST)
	{
		IDirectDrawSurface_Restore(s_pPrimary);
		return TRUE;
	}
	return FALSE;
}

// Block until the PVR vertical blank (~60Hz). Used to pace the shell loop instead
// of Sleep (which rounds up to the ~50ms CE system tick = the 20fps cap).
HRESULT GfxWaitVBlank(void)
{
	if (!s_pDd)
		return E_FAIL;
	return IDirectDraw_WaitForVerticalBlank(s_pDd, DDWAITVB_BLOCKBEGIN, NULL);
}

//
// Page layer: alias the GWES GDI framebuffer as a system-mem DDraw surface, and create a VRAM
// texture the page region is BltFast'd into each frame. Mirrors the SDK htmlsamp draw.cpp trick
// (GETGDIINFO -> MmMapIoSpace -> IDirectDrawSurface3::SetSurfaceDesc(lpSurface=...)).
//
BOOL GfxInitPageLayer(void)
{
	GDISurfaceInfo info;
	DDSURFACEDESC sd;
	LPDIRECTDRAWSURFACE pSys = NULL;
	LPDIRECTDRAWSURFACE3 pSys3 = NULL;
	PHYSICAL_ADDRESS pa;
	void *pvBits;
	HDC dc;

	if (!s_pDd || !s_pDev)
		return FALSE;

	dc = GetDC(NULL);
	memset(&info, 0, sizeof(info));
	if (!dc || ExtEscape(dc, GETGDIINFO, 0, 0, sizeof(info), (LPSTR)&info) <= 0)
	{
		if (dc)
			ReleaseDC(NULL, dc);
		OutputDebugStringW(L"page: GETGDIINFO FAIL\r\n");
		return FALSE;
	}
	ReleaseDC(NULL, dc);

	{
		WCHAR b[128];
		wsprintfW(b, L"page: GETGDIINFO w=%d h=%d stride=%d phys=%08x\r\n", info.width, info.height,
		          info.stride, (unsigned)info.physicalAddr);
		OutputDebugStringW(b);
	}

	pa.HighPart = 0;
	pa.LowPart = info.physicalAddr;
	pvBits = MmMapIoSpace(pa, (ULONG)(info.height * info.stride), TRUE);
	if (!pvBits)
	{
		OutputDebugStringW(L"page: MmMapIoSpace FAIL\r\n");
		return FALSE;
	}

	// system-mem surface, then re-point it at the GDI framebuffer via IDirectDrawSurface3.
	// Pin the format to 565 so BltFast to the 565 page texture is a straight copy (a format
	// mismatch shows up as vertical RGB striping over the recognizable image).
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH;
	sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
	sd.dwWidth = info.width;
	sd.dwHeight = info.height;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &pSys, NULL) != DD_OK || !pSys)
	{
		OutputDebugStringW(L"page: gdi surface create FAIL\r\n");
		return FALSE;
	}
	if (IDirectDrawSurface_QueryInterface(pSys, &IID_IDirectDrawSurface3, (void **)&pSys3) ==
	        DD_OK &&
	    pSys3)
	{
		memset(&sd, 0, sizeof(sd));
		sd.dwSize = sizeof(sd);
		sd.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_LPSURFACE | DDSD_PITCH | DDSD_PIXELFORMAT;
		sd.dwWidth = info.width;
		sd.dwHeight = info.height;
		sd.lPitch = info.stride;
		sd.lpSurface = pvBits;
		sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
		sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
		sd.ddpfPixelFormat.dwRGBBitCount = 16;
		sd.ddpfPixelFormat.dwRBitMask = 0xF800;
		sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
		sd.ddpfPixelFormat.dwBBitMask = 0x001F;
		if (IDirectDrawSurface3_SetSurfaceDesc(pSys3, &sd, 0) != DD_OK)
			OutputDebugStringW(L"page: SetSurfaceDesc FAIL\r\n");
		IDirectDrawSurface3_Release(pSys3);
	}
	s_pGdiSurf = pSys;

	// VRAM texture (565) the page is blitted into; bound as a normal compositor texture
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
	sd.dwWidth = PAGE_TW;
	sd.dwHeight = PAGE_TH;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0xF800;
	sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
	sd.ddpfPixelFormat.dwBBitMask = 0x001F;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &s_pPageSurf, NULL) != DD_OK || !s_pPageSurf)
	{
		OutputDebugStringW(L"page: vram texture create FAIL\r\n");
		return FALSE;
	}
	if (IDirectDrawSurface_QueryInterface(s_pPageSurf, &IID_IDirect3DTexture2,
	                                      (void **)&s_pPageTex) != DD_OK ||
	    IDirect3DTexture2_GetHandle(s_pPageTex, s_pDev, &s_hPage) != DD_OK)
	{
		OutputDebugStringW(L"page: texture handle FAIL\r\n");
		return FALSE;
	}

	OutputDebugStringW(L"page: layer ready\r\n");
	return TRUE;
}

// Blit a GDI-framebuffer rect into the page texture and queue it as a compositor quad.
void GfxBlitPage(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh)
{
	RECT src;
	if (!s_pGdiSurf || !s_pPageSurf || !s_hPage)
		return;
	if (sw > PAGE_TW)
		sw = PAGE_TW;
	if (sh > PAGE_TH)
		sh = PAGE_TH;
	src.left = sx;
	src.top = sy;
	src.right = sx + sw;
	src.bottom = sy + sh;
	IDirectDrawSurface_BltFast(s_pPageSurf, 0, 0, s_pGdiSurf, &src, DDBLTFAST_WAIT);
	PushQuad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), 0.0f, 0.0f,
	         (float)sw / PAGE_TW, (float)sh / PAGE_TH, 0xFFFFFFFF, 2);
}

// --- wallpaper -------------------------------------------------------------------
// Create the VRAM 565 wallpaper texture (once; recreated after a surface loss).
static BOOL EnsureWallTexture(void)
{
	DDSURFACEDESC sd;
	if (s_pWallSurf)
	{
		if (!s_hWall && s_pWallTex) // handle was cleared (e.g. by a "(None)") - re-fetch it
			IDirect3DTexture2_GetHandle(s_pWallTex, s_pDev, &s_hWall);
		return TRUE;
	}
	if (!s_pDd || !s_pDev)
		return FALSE;
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
	sd.dwWidth = PAGE_TW;
	sd.dwHeight = PAGE_TH;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0xF800;
	sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
	sd.ddpfPixelFormat.dwBBitMask = 0x001F;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &s_pWallSurf, NULL) != DD_OK || !s_pWallSurf)
		return FALSE;
	if (IDirectDrawSurface_QueryInterface(s_pWallSurf, &IID_IDirect3DTexture2,
	                                      (void **)&s_pWallTex) != DD_OK ||
	    IDirect3DTexture2_GetHandle(s_pWallTex, s_pDev, &s_hWall) != DD_OK)
		return FALSE;
	return TRUE;
}

// Load a 24-bit BI_RGB BMP from disc, convert to 565, upload into the wallpaper texture. style is
// GFXWALL_STRETCH (fill 640x480) or GFXWALL_CENTER (1:1, centred). The path + style are remembered
// so GfxReloadWallpaper can re-upload after the surfaces are torn down for a fullscreen app.
BOOL GfxSetWallpaper(const WCHAR *pszPath, int nStyle)
{
	HANDLE hf;
	DWORD dwSize, dwRead = 0;
	BYTE *pbFile = NULL;
	BITMAPFILEHEADER *pbf;
	BITMAPINFOHEADER *pbi;
	LPDIRECTDRAWSURFACE pStage = NULL;
	DDSURFACEDESC sd;
	BOOL bOk = FALSE;

	if (pszPath != s_szWallPath) // remember (skip the self-copy on a reload)
	{
		int n = 0;
		if (pszPath)
			for (; pszPath[n] && n < MAX_PATH - 1; n++)
				s_szWallPath[n] = pszPath[n];
		s_szWallPath[n] = 0;
	}
	s_nWallStyle = nStyle;
	if (!s_szWallPath[0]) // cleared -> no wallpaper (keep s_hWall valid; s_nWallW=0 stops drawing)
	{
		s_nWallW = s_nWallH = 0;
		return FALSE;
	}

	hf = CreateFileW(s_szWallPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                 FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE)
		return FALSE;
	dwSize = GetFileSize(hf, NULL);
	if (dwSize > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) && dwSize < 4u * 1024 * 1024)
		pbFile = (BYTE *)LocalAlloc(LMEM_FIXED, dwSize);
	if (pbFile && ReadFile(hf, pbFile, dwSize, &dwRead, NULL) && dwRead == dwSize)
	{
		pbf = (BITMAPFILEHEADER *)pbFile;
		pbi = (BITMAPINFOHEADER *)(pbFile + sizeof(BITMAPFILEHEADER));
		if (pbf->bfType == 0x4D42 && pbi->biBitCount == 24 && pbi->biCompression == BI_RGB &&
		    pbf->bfOffBits < dwSize)
		{
			int w = pbi->biWidth;
			int bTopDown = pbi->biHeight < 0;
			int h = bTopDown ? -pbi->biHeight : pbi->biHeight;
			int srcRow = ((w * 3) + 3) & ~3; // BMP rows are DWORD-aligned
			int tw = w > PAGE_TW ? PAGE_TW : w;
			int th = h > PAGE_TH ? PAGE_TH : h;
			BYTE *pbBits = pbFile + pbf->bfOffBits;
			if (w > 0 && h > 0 && pbf->bfOffBits + (DWORD)srcRow * h <= dwSize)
			{
				memset(&sd, 0, sizeof(sd)); // system-mem 565 staging surface
				sd.dwSize = sizeof(sd);
				sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
				sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
				sd.dwWidth = tw;
				sd.dwHeight = th;
				sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
				sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
				sd.ddpfPixelFormat.dwRGBBitCount = 16;
				sd.ddpfPixelFormat.dwRBitMask = 0xF800;
				sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
				sd.ddpfPixelFormat.dwBBitMask = 0x001F;
				if (s_pDd && IDirectDraw_CreateSurface(s_pDd, &sd, &pStage, NULL) == DD_OK &&
				    pStage)
				{
					DDSURFACEDESC lk;
					memset(&lk, 0, sizeof(lk));
					lk.dwSize = sizeof(lk);
					if (IDirectDrawSurface_Lock(pStage, NULL, &lk, DDLOCK_WAIT, NULL) == DD_OK)
					{
						// 4x4 ordered (Bayer) dither: spreads the 24->16-bit (565) quantization
						// error so gradients band far less on the DC's 16-bit display.
						static const int aBayer[16] = {0, 8,  2, 10, 12, 4, 14, 6,
						                               3, 11, 1, 9,  15, 7, 13, 5};
						int x, y;
						for (y = 0; y < th; y++)
						{
							int srcY = bTopDown ? y : (h - 1 - y); // BMP is bottom-up
							const BYTE *s = pbBits + srcY * srcRow;
							WORD *d = (WORD *)((BYTE *)lk.lpSurface + y * lk.lPitch);
							for (x = 0; x < tw; x++, s += 3)
							{
								int e = aBayer[((y & 3) << 2) | (x & 3)] - 8; // -8..7
								int B = s[0] + (e >> 1), G = s[1] + (e >> 2), R = s[2] + (e >> 1);
								if (R < 0)
									R = 0;
								else if (R > 255)
									R = 255;
								if (G < 0)
									G = 0;
								else if (G > 255)
									G = 255;
								if (B < 0)
									B = 0;
								else if (B > 255)
									B = 255;
								d[x] = (WORD)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
							}
						}
						IDirectDrawSurface_Unlock(pStage, NULL);
						if (EnsureWallTexture())
						{
							RECT src;
							src.left = 0;
							src.top = 0;
							src.right = tw;
							src.bottom = th;
							IDirectDrawSurface_BltFast(s_pWallSurf, 0, 0, pStage, &src,
							                           DDBLTFAST_WAIT);
							s_nWallW = tw;
							s_nWallH = th;
							bOk = TRUE;
						}
					}
				}
			}
		}
	}
	if (pStage)
		IDirectDrawSurface_Release(pStage);
	if (pbFile)
		LocalFree(pbFile);
	CloseHandle(hf);
	if (!bOk)
		s_nWallW = s_nWallH = 0; // load failed -> draw nothing (keep s_hWall valid for next time)
	return bOk;
}

// Re-upload the remembered wallpaper after the surfaces were recreated (post fullscreen app).
void GfxReloadWallpaper(void)
{
	if (s_szWallPath[0])
		GfxSetWallpaper(s_szWallPath, s_nWallStyle);
}

// Queue the wallpaper quad (call inside the desktop-cache recording, before the icons).
void GfxBlitWallpaper(void)
{
	float uu, vv;
	if (!s_hWall || s_nWallW <= 0 || s_nWallH <= 0)
		return;
	uu = (float)s_nWallW / PAGE_TW;
	vv = (float)s_nWallH / PAGE_TH;
	if (s_nWallStyle == GFXWALL_CENTER)
	{
		int dx = (SCREEN_W - s_nWallW) / 2, dy = (SCREEN_H - s_nWallH) / 2;
		if (dx < 0)
			dx = 0;
		if (dy < 0)
			dy = 0;
		PushQuad((float)dx, (float)dy, (float)(dx + s_nWallW), (float)(dy + s_nWallH), 0.0f, 0.0f,
		         uu, vv, 0xFFFFFFFF, 3);
	}
	else // GFXWALL_STRETCH: fill the screen (the taskbar layer overpaints the bottom strip)
		PushQuad(0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0.0f, 0.0f, uu, vv, 0xFFFFFFFF, 3);
}

// Draw the current wallpaper scaled into an arbitrary rect (the Display Properties monitor
// preview). Returns FALSE if no wallpaper is set so the caller can fill the rect itself.
BOOL GfxDrawWallpaperRect(int dx, int dy, int dw, int dh)
{
	if (!s_hWall || s_nWallW <= 0 || s_nWallH <= 0)
		return FALSE;
	PushQuad((float)dx, (float)dy, (float)(dx + dw), (float)(dy + dh), 0.0f, 0.0f,
	         (float)s_nWallW / PAGE_TW, (float)s_nWallH / PAGE_TH, 0xFFFFFFFF, 3);
	return TRUE;
}

// --- Start-menu banner -----------------------------------------------------------
// Create the VRAM 565 banner texture (once; recreated after a surface loss).
static BOOL EnsureBanTexture(void)
{
	DDSURFACEDESC sd;
	if (s_pBanSurf)
	{
		if (!s_hBan && s_pBanTex)
			IDirect3DTexture2_GetHandle(s_pBanTex, s_pDev, &s_hBan);
		return TRUE;
	}
	if (!s_pDd || !s_pDev)
		return FALSE;
	memset(&sd, 0, sizeof(sd));
	sd.dwSize = sizeof(sd);
	sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
	sd.dwWidth = BAN_TW;
	sd.dwHeight = BAN_TH;
	sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
	sd.ddpfPixelFormat.dwRGBBitCount = 16;
	sd.ddpfPixelFormat.dwRBitMask = 0xF800;
	sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
	sd.ddpfPixelFormat.dwBBitMask = 0x001F;
	if (IDirectDraw_CreateSurface(s_pDd, &sd, &s_pBanSurf, NULL) != DD_OK || !s_pBanSurf)
		return FALSE;
	if (IDirectDrawSurface_QueryInterface(s_pBanSurf, &IID_IDirect3DTexture2,
	                                      (void **)&s_pBanTex) != DD_OK ||
	    IDirect3DTexture2_GetHandle(s_pBanTex, s_pDev, &s_hBan) != DD_OK)
		return FALSE;
	return TRUE;
}

// Load a 24-bit BMP into the banner texture. Same decode as GfxSetWallpaper (Bayer-dithered
// 565 staging -> BltFast); the bitmap is clipped at BAN_TW x BAN_TH.
BOOL GfxSetBanner(const WCHAR *pszPath)
{
	HANDLE hf;
	DWORD dwSize, dwRead = 0;
	BYTE *pbFile = NULL;
	BITMAPFILEHEADER *pbf;
	BITMAPINFOHEADER *pbi;
	LPDIRECTDRAWSURFACE pStage = NULL;
	DDSURFACEDESC sd;
	BOOL bOk = FALSE;

	if (pszPath != s_szBanPath)
	{
		int n = 0;
		if (pszPath)
			for (; pszPath[n] && n < MAX_PATH - 1; n++)
				s_szBanPath[n] = pszPath[n];
		s_szBanPath[n] = 0;
	}
	if (!s_szBanPath[0])
	{
		s_nBanW = s_nBanH = 0;
		return FALSE;
	}

	hf = CreateFileW(s_szBanPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                 FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf == INVALID_HANDLE_VALUE)
		return FALSE;
	dwSize = GetFileSize(hf, NULL);
	if (dwSize > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) && dwSize < 4u * 1024 * 1024)
		pbFile = (BYTE *)LocalAlloc(LMEM_FIXED, dwSize);
	if (pbFile && ReadFile(hf, pbFile, dwSize, &dwRead, NULL) && dwRead == dwSize)
	{
		pbf = (BITMAPFILEHEADER *)pbFile;
		pbi = (BITMAPINFOHEADER *)(pbFile + sizeof(BITMAPFILEHEADER));
		if (pbf->bfType == 0x4D42 && pbi->biBitCount == 24 && pbi->biCompression == BI_RGB &&
		    pbf->bfOffBits < dwSize)
		{
			int w = pbi->biWidth;
			int bTopDown = pbi->biHeight < 0;
			int h = bTopDown ? -pbi->biHeight : pbi->biHeight;
			int srcRow = ((w * 3) + 3) & ~3;
			int tw = w > BAN_TW ? BAN_TW : w;
			int th = h > BAN_TH ? BAN_TH : h;
			BYTE *pbBits = pbFile + pbf->bfOffBits;
			if (w > 0 && h > 0 && pbf->bfOffBits + (DWORD)srcRow * h <= dwSize)
			{
				memset(&sd, 0, sizeof(sd));
				sd.dwSize = sizeof(sd);
				sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
				sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
				sd.dwWidth = tw;
				sd.dwHeight = th;
				sd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
				sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
				sd.ddpfPixelFormat.dwRGBBitCount = 16;
				sd.ddpfPixelFormat.dwRBitMask = 0xF800;
				sd.ddpfPixelFormat.dwGBitMask = 0x07E0;
				sd.ddpfPixelFormat.dwBBitMask = 0x001F;
				if (s_pDd && IDirectDraw_CreateSurface(s_pDd, &sd, &pStage, NULL) == DD_OK &&
				    pStage)
				{
					DDSURFACEDESC lk;
					memset(&lk, 0, sizeof(lk));
					lk.dwSize = sizeof(lk);
					if (IDirectDrawSurface_Lock(pStage, NULL, &lk, DDLOCK_WAIT, NULL) == DD_OK)
					{
						// same 4x4 ordered dither as the wallpaper (24->565)
						static const int aBayer[16] = {0, 8,  2, 10, 12, 4, 14, 6,
						                               3, 11, 1, 9,  15, 7, 13, 5};
						int x, y;
						for (y = 0; y < th; y++)
						{
							int srcY = bTopDown ? y : (h - 1 - y);
							const BYTE *s = pbBits + srcY * srcRow;
							WORD *d = (WORD *)((BYTE *)lk.lpSurface + y * lk.lPitch);
							for (x = 0; x < tw; x++, s += 3)
							{
								int e = aBayer[((y & 3) << 2) | (x & 3)] - 8;
								int B = s[0] + (e >> 1), G = s[1] + (e >> 2), R = s[2] + (e >> 1);
								if (R < 0)
									R = 0;
								else if (R > 255)
									R = 255;
								if (G < 0)
									G = 0;
								else if (G > 255)
									G = 255;
								if (B < 0)
									B = 0;
								else if (B > 255)
									B = 255;
								d[x] = (WORD)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
							}
						}
						IDirectDrawSurface_Unlock(pStage, NULL);
						if (EnsureBanTexture())
						{
							RECT src;
							src.left = 0;
							src.top = 0;
							src.right = tw;
							src.bottom = th;
							IDirectDrawSurface_BltFast(s_pBanSurf, 0, 0, pStage, &src,
							                           DDBLTFAST_WAIT);
							s_nBanW = tw;
							s_nBanH = th;
							bOk = TRUE;
						}
					}
				}
			}
		}
	}
	if (pStage)
		IDirectDrawSurface_Release(pStage);
	if (pbFile)
		LocalFree(pbFile);
	CloseHandle(hf);
	if (!bOk)
		s_nBanW = s_nBanH = 0;
	return bOk;
}

// Re-upload the remembered banner after the surfaces were recreated (post fullscreen app).
void GfxReloadBanner(void)
{
	if (s_szBanPath[0])
		GfxSetBanner(s_szBanPath);
}

void GfxBannerSize(int *w, int *h)
{
	if (w)
		*w = s_nBanW;
	if (h)
		*h = s_nBanH;
}

// Queue the banner bitmap like the stock shell's BitBlt: 1:1 at its native size, anchored
// to the BOTTOM of the dst strip. A menu shorter than the bitmap clips the bitmap's top
// rows; a taller one leaves strip background above (the caller FillRects the strip first).
void GfxBlitBanner(int dx, int dy, int dw, int dh)
{
	int h;
	float v0;
	if (!s_hBan || s_nBanW <= 0 || s_nBanH <= 0 || dw <= 0 || dh <= 0)
		return;
	h = dh < s_nBanH ? dh : s_nBanH;
	v0 = (float)(s_nBanH - h) / BAN_TH;
	PushQuad((float)dx, (float)(dy + dh - h), (float)(dx + dw), (float)(dy + dh),
	         0.0f, v0, (float)s_nBanW / BAN_TW, (float)s_nBanH / BAN_TH, 0xFFFFFFFF, 4);
}

static DWORD s_dwLastExit = 0; // exit code of the last GfxLaunch'd app (crash code if it faulted)
DWORD GfxLastExitCode(void)
{
	return s_dwLastExit;
}

int GfxLaunch(const WCHAR *path, GFXPOLLFN pfnPoll)
{
	PROCESS_INFORMATION pi;
	int nKill = 0;

	DestroyD3D();
	DestroySurfaces();
	GfxFreeQuads(); // the shell isn't compositing while the app runs -> hand its ~0.5 MB back
	if (CreateProcessW(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
	{
		if (pfnPoll)
		{
			// Cooperative app (e.g. our browser): the shell kept its keyboard + controller
			// acquired, so poll the panic combos every 30 ms (ALT+F4 / START+A kill;
			// CTRL+ALT+DEL kills + opens the task manager). Nonzero return -> TerminateProcess.
			while (WaitForSingleObject(pi.hProcess, 30) == WAIT_TIMEOUT)
			{
				if ((nKill = pfnPoll()) != 0)
				{
					TerminateProcess(pi.hProcess, 0);
					WaitForSingleObject(pi.hProcess, 3000); // let it unwind / drop the display
					break;
				}
			}
		}
		else
		{
			// Retail game (exclusive Maple/DInput + raw Maple DMA): the shell dropped ALL
			// input and must NOT touch the bus, or the game faults to the BIOS. Just wait.
			WaitForSingleObject(pi.hProcess, INFINITE);
		}
		s_dwLastExit = 0;
		GetExitCodeProcess(pi.hProcess, &s_dwLastExit); // crash code if it faulted unhandled
		if (nKill)
			s_dwLastExit = 0; // we killed it -> not a crash
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	if (CreateSurfaces())
	{
		InitD3D();
		BuildAtlas();
		GfxReloadWallpaper(); // surfaces were torn down for the app -> re-upload the wallpaper
		GfxReloadBanner();    // same for the Start-menu banner texture
	}
	return nKill;
}
