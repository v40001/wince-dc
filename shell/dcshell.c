//
// dcshell.c - Dreamcast CE hybrid desktop shell (DCWin compositor).
//
// A desktop (teal background, shortcut icons, taskbar, Start menu). Everything
// else is a separate windowed process drawn by the compositor: Explorer
// (dcwexp.exe), Calculator, Clock. Desktop/Start shortcuts with a path launch the
// Explorer at that path; shortcuts with an exe launch that app.
//
// Drawing: the whole frame is composited into a back buffer (dcgfx), presented to
// the volatile DC primary each loop. Fills/bevels are DirectDraw COLORFILL, text
// is GDI Arial, icons are color-keyed surfaces. Each layer (desktop -> windows
// back-to-front -> taskbar/Start) does its fills then a GetDC text pass, so
// overlapping windows clip correctly.
//
#include "dcgfx.h"
#include "dcwin.h"
#include "dcinput.h"
#include "vmustore.h"

#define TASK_H     26
#define TASK_Y     (SCREEN_H - TASK_H)

// Start-menu metrics lifted 1:1 from the stock ceshell item layout (HPC2000.BIN module,
// menu draw @rva 0xb94e / measure @rva 0xb812): rows are SM_CYSMICON+6 = 22 px, separator
// rows are 7 px with the groove drawn inset, icons are 16x16 at (item.left+4, item.top+3),
// text starts at item.left+24 and is vertically centered with an 8 px right inset for the
// submenu glyph zone. The root menu prepends the 26-px banner strip, and the banner bitmap
// is BitBlt'd 1:1 anchored to the strip bottom (top clipped when the menu is shorter).
#define ROW_H      22
#define SEP_H      7
#define STRIP_W    26
#define MENU_PAD   2

// Stock CE caption: a 22px navy bar (measured off the CEPC build's Run dialog), with
// ~14px-wide bevelled close/min/max boxes tucked to its right edge.
#define CAPT_H     22

// Win32 OEM VKs live in dcwin.h now (shared with the client apps).

#define CL_DESKTOP RGB(0, 128, 128)
#define CL_FACE    RGB(192, 192, 192)
#define CL_TITLE   RGB(0, 0, 128)
#define CL_SEL     RGB(0, 0, 128)
#define CL_TEXT    RGB(0, 0, 0)
#define CL_WHITE   RGB(255, 255, 255)

// Shell-internal commands for Start-menu items that aren't a plain launch/open.
#define SCMD_NONE       0
#define SCMD_HELP       1 // Help -> browse \Windows (peghelp has no help data on this disc)
#define SCMD_RUN        2 // Run... -> the Run dialog
#define SCMD_SUSPEND    3 // Suspend -> power down (exit the shell back to the DCWin loader)
#define SCMD_TASKBAROPT 4 // Settings -> Taskbar -> Taskbar Properties

typedef struct
{
	const WCHAR *label;
	const WCHAR *path; // open Explorer here, or NULL
	const WCHAR *exe;  // launch this app, or NULL
	int icon;          // ICON_*
	int sep;           // draw an etched separator groove at the top of this row (Start menu)
	int sub;           // SUB_* cascade id, or -1
	int cmd;           // SCMD_*
	int dis;           // greyed out (the stock "(Empty)" placeholder row)
} SHORTCUT;

// Built-in desktop shortcuts. The live desktop is a mutable copy (s_aDesk) so the user can
// drag items off the Start menu to add their own; user-added entries are persisted to the
// VMU. String pointers always point at const tables (defaults here / s_start below), so the
// live array stores no allocations.
// The stock CEPC desktop set (QEMU capture): Recycle Bin / Inbox / Microsoft
// Pocket Word on the top row, My Computer / My Documents / Internet Explorer
// below - each with the shortcut-arrow overlay the real shell draws.
static const SHORTCUT s_deskDefault[] = {
    // the stock CE desktop leads with these (CEPC capture): My Computer, Recycle
    // Bin, My Documents (+ Internet Explorer, appended conditionally below) -
    // then our DC-specific extras
    {L"Recycle Bin", L"\\Recycled", NULL, ICON_BIN, 0, -1, SCMD_NONE, 0},
    {L"Inbox", NULL, L"dcwmail.exe", ICON_INBOX, 0, -1, SCMD_NONE, 0},
    {L"Microsoft Pocket Word", NULL, L"dcwpword.exe", ICON_PWORD, 0, -1, SCMD_NONE, 0},
    {L"My Computer", L"\\", NULL, ICON_COMPUTER, 0, -1, SCMD_NONE, 0},
    {L"My Documents", L"\\My Documents", NULL, ICON_DOCUMENTS, 0, -1, SCMD_NONE, 0},
    {L"Internet Explorer", NULL, L"iexplore.exe", ICON_IE, 0, -1, SCMD_NONE, 0},
};
#define DEFAULT_N (sizeof(s_deskDefault) / sizeof(s_deskDefault[0]))
#define MAXDESK   24
static SHORTCUT s_aDesk[MAXDESK]; // live desktop shortcuts
static int s_anDeskUser[MAXDESK]; // for user-added: the s_start[] index it came from; -1 = built-in
static int s_cDesk = 0;

// The stock ceshell Start menu: four cascades + Help + Run, separator, Suspend (the ROM
// strings are &Programs/&Favorites/&Documents/&Settings/&Help/&Run.../S&uspend). The real
// shell enumerates \Windows\Programs for the first cascade and \Windows\Favorites /
// \Windows\Recent for the next two; ours ships a fixed app list + the "(Empty)" placeholders
// the stock shell shows for empty folders.
#define SUB_PROGRAMS  0
#define SUB_FAVORITES 1
#define SUB_DOCUMENTS 2
#define SUB_SETTINGS  3
#define SUB_COMM      4 // nested cascade inside Programs
#define SUB_COUNT     5

// The stock Programs cascade (CEPC capture): a nested "Communication" submenu,
// then Command Prompt / Inbox / Internet Explorer / Microsoft Pocket Word /
// Windows Explorer. Entries without a DC port stay listed but map to the closest
// client app; our own extras follow after them.
// Communication cascade (QEMU capture): Remote Networking, Terminal. Remote
// Networking is the dial-up connections folder - dcwnet is the DC-side RasDial UI.
static const SHORTCUT s_subComm[] = {
    {L"Remote Networking", NULL, L"dcwnet.exe", ICON_NET, 0, -1, SCMD_NONE, 0},
    {L"Terminal", NULL, L"dcwterm.exe", ICON_TERM, 0, -1, SCMD_NONE, 0},
    {L"Network Diagnostics", NULL, L"dcwnet.exe", ICON_NET, 0, -1, SCMD_NONE, 0},
    {L"System Log", NULL, L"dcwlog.exe", ICON_FILE, 0, -1, SCMD_NONE, 0},
};
static const SHORTCUT s_subPrograms[] = {
    {L"Communication", NULL, NULL, ICON_COMM, 0, SUB_COMM, SCMD_NONE, 0},
    {L"Command Prompt", NULL, L"dcwcmd.exe", ICON_CMD, 0, -1, SCMD_NONE, 0},
    {L"Inbox", NULL, L"dcwmail.exe", ICON_INBOX, 0, -1, SCMD_NONE, 0},
    {L"Internet Explorer", NULL, L"iexplore.exe", ICON_IE, 0, -1, SCMD_NONE, 0},
    {L"Microsoft Pocket Word", NULL, L"dcwpword.exe", ICON_PWORD, 0, -1, SCMD_NONE, 0},
    {L"Windows Explorer", L"\\", NULL, ICON_EXPLORER, 0, -1, SCMD_NONE, 0},
    {NULL, NULL, NULL, 0, 1, -1, 0, 0}, // separator before the DC extras
    {L"Calculator", NULL, L"dcwcalc.exe", ICON_APP, 0, -1, SCMD_NONE, 0},
    {L"Clock", NULL, L"dcwclock.exe", ICON_CLOCK, 0, -1, SCMD_NONE, 0},
    {L"Task Manager", NULL, L"dcwtask.exe", ICON_APP, 0, -1, SCMD_NONE, 0},
    {L"System", NULL, L"dcwmem.exe", ICON_CPL, 0, -1, SCMD_NONE, 0},
    {L"Music Player", NULL, L"dcwplay.exe", ICON_APP, 0, -1, SCMD_NONE, 0},
    {L"Crash Test", NULL, L"dcwtask.exe crash", ICON_APP, 0, -1, SCMD_NONE, 0},
};
static const SHORTCUT s_subFavorites[] = {
    {L"(Empty)", NULL, NULL, 0, 0, -1, SCMD_NONE, 1},
};
static const SHORTCUT s_subDocuments[] = {
    {L"(Empty)", NULL, NULL, 0, 0, -1, SCMD_NONE, 1},
};
static const SHORTCUT s_subSettings[] = {
    {L"Control Panel", NULL, L"dcwcpl.exe", ICON_CPL, 0, -1, SCMD_NONE, 0},
    {L"Taskbar", NULL, NULL, ICON_TASKBAR, 0, -1, SCMD_TASKBAROPT, 0},
};

static const SHORTCUT s_start[] = {
    {L"Programs", NULL, NULL, ICON_PROGRAMS, 0, SUB_PROGRAMS, SCMD_NONE, 0},
    {L"Favorites", NULL, NULL, ICON_FAVORITES, 0, SUB_FAVORITES, SCMD_NONE, 0},
    {L"Documents", NULL, NULL, ICON_DOCUMENTS, 0, SUB_DOCUMENTS, SCMD_NONE, 0},
    {L"Settings", NULL, NULL, ICON_SETTINGS, 0, SUB_SETTINGS, SCMD_NONE, 0},
    {L"Help", NULL, NULL, ICON_HELP, 0, -1, SCMD_HELP, 0},
    {L"Run...", NULL, NULL, ICON_RUN, 0, -1, SCMD_RUN, 0},
    {NULL, NULL, NULL, 0, 1, -1, 0, 0}, // 7px etched separator row, like the stock menu
    {L"Suspend", NULL, NULL, ICON_SUSPEND, 0, -1, SCMD_SUSPEND, 0},
};
#define START_N (sizeof(s_start) / sizeof(s_start[0]))

// Cascade submenu state: s_nSubRoot = s_start[] index whose cascade is open (-1 = closed),
// s_nSubSel = highlighted row inside it. s_nHoverSub tracks a cursor parked on a cascade
// item so the submenu auto-opens after the stock ~400ms dwell. Declared here because the
// geometry helpers below already reference them.
static int s_nSubRoot = -1;
static int s_nSubSel = -1;
static int s_nHoverSub = -1;
static DWORD s_dwHoverAt = 0;
static int s_nSub2Root = -1; // nested cascade (e.g. Communication inside Programs):
static int s_nSub2Sel = -1;  // s_nSub2Root = item index in the level-1 table
static int s_nHoverSub2 = -1;
static DWORD s_dwHoverAt2 = 0;
static int s_bDirty = 1; // declared early - the cascade helpers below set it

// Cascade item lists by SUB_* id (the stock shell fills these from folders).
static const SHORTCUT *SubItems(int nSub, int *pn)
{
	switch (nSub)
	{
		case SUB_PROGRAMS:
			*pn = (int)(sizeof(s_subPrograms) / sizeof(s_subPrograms[0]));
			return s_subPrograms;
		case SUB_FAVORITES:
			*pn = (int)(sizeof(s_subFavorites) / sizeof(s_subFavorites[0]));
			return s_subFavorites;
		case SUB_DOCUMENTS:
			*pn = (int)(sizeof(s_subDocuments) / sizeof(s_subDocuments[0]));
			return s_subDocuments;
		case SUB_SETTINGS:
			*pn = (int)(sizeof(s_subSettings) / sizeof(s_subSettings[0]));
			return s_subSettings;
		case SUB_COMM:
			*pn = (int)(sizeof(s_subComm) / sizeof(s_subComm[0]));
			return s_subComm;
	}
	*pn = 0;
	return NULL;
}

// Width of the vertical "Windows CE" brand strip along the left edge of the open Start
// menu (the stock ceshell bitmap, loaded as \Windows\startban.bmp). The stock shell gives
// the strip the bitmap's native width (26) - it is BitBlt'd 1:1, never stretched.
// 0 when the bitmap isn't staged -> the menu falls back to the plain look.
static int StartMenuStrip(void)
{
	int nBanW = 0, nBanH = 0;
	GfxBannerSize(&nBanW, &nBanH);
	if (nBanW <= 0 || nBanH <= 0)
		return 0;
	return nBanW;
}

// Total height of a menu's item area: SM_CYSMICON+6 rows plus SEP_H separator rows
// (the stock layout adds a thin border around this). Works on the root s_start[] table
// and on cascade lists alike - submenus share the stock row metrics, minus the banner.
static int MenuContentH(const SHORTCUT *aItems, int nItems)
{
	int i, h = 0;
	for (i = 0; i < nItems; i++)
		h += aItems[i].sep ? SEP_H : ROW_H;
	return h;
}
static int StartMenuContentH(void)
{
	return MenuContentH(s_start, (int)START_N);
}

// Width of the item area = SM_CXSMICON(16) + max text width (clamped to 200, the stock
// layout's 0xC8) + 16. The strip goes on the left of this (root menu only).
static int MenuItemW(const SHORTCUT *aItems, int nItems)
{
	int i, w = 0;
	for (i = 0; i < nItems; i++)
	{
		int t;
		if (!aItems[i].label)
			continue;
		t = GfxTextWidth(g_FontUI, aItems[i].label);
		if (t > 200)
			t = 200;
		if (t > w)
			w = t;
	}
	return w + 32;
}
static int StartMenuItemW(void)
{
	return MenuItemW(s_start, (int)START_N);
}

// Content-relative y -> item index, -1 for separators/outside (rows are variable-height).
static int MenuItemAtY(const SHORTCUT *aItems, int nItems, int y)
{
	int i, yy = 0;
	for (i = 0; i < nItems; i++)
	{
		int ih = aItems[i].sep ? SEP_H : ROW_H;
		if (y >= yy && y < yy + ih)
			return aItems[i].sep ? -1 : i;
		yy += ih;
	}
	return -1;
}
static int StartItemAtY(int y)
{
	return MenuItemAtY(s_start, (int)START_N, y);
}

// y of the top of item nIdx inside the content area (for submenu placement).
static int MenuItemTop(const SHORTCUT *aItems, int nIdx)
{
	int i, y = 0;
	for (i = 0; i < nIdx; i++)
		y += aItems[i].sep ? SEP_H : ROW_H;
	return y;
}

// --- cascade submenu geometry/state ---------------------------------------------
// The open cascade hangs off its parent row on the root menu's right, overlapping the
// frame by MENU_PAD (stock submenu windows cover the parent menu's right border, and
// their top edge aligns with the parent item's row). No brand strip on submenus.
static const SHORTCUT *SubOpenItems(int *pn)
{
	if (s_nSubRoot < 0 || s_nSubRoot >= (int)START_N || s_start[s_nSubRoot].sub < 0)
	{
		*pn = 0;
		return NULL;
	}
	return SubItems(s_start[s_nSubRoot].sub, pn);
}

static void SubRect(RECT *prc)
{
	int n = 0, nStrip = StartMenuStrip(), nItemW = StartMenuItemW();
	const SHORTCUT *a = SubOpenItems(&n);
	int rr = 2 + MENU_PAD * 2 + nStrip + nItemW; // root menu's outer right edge
	int ry = TASK_Y - StartMenuContentH() - MENU_PAD * 2;
	int nTop = ry + MENU_PAD + MenuItemTop(s_start, s_nSubRoot) - MENU_PAD;
	int w = a ? MenuItemW(a, n) + MENU_PAD * 2 : MENU_PAD * 2 + 8;
	int h = a ? MenuContentH(a, n) + MENU_PAD * 2 : MENU_PAD * 2;
	if (nTop + h > TASK_Y - 2) // keep it above the taskbar like a real popup
		nTop = TASK_Y - 2 - h;
	if (nTop < 2)
		nTop = 2;
	SetRect(prc, rr - MENU_PAD, nTop, rr - MENU_PAD + w, nTop + h);
}

static void OpenSub(int nRootIdx)
{
	s_nSubRoot = nRootIdx;
	s_nSubSel = -1; // nothing highlighted until the cursor/keys pick a row
	s_nSub2Root = -1;
	s_nSub2Sel = -1;
	s_bDirty = 1;
}
static void CloseSub(void)
{
	s_nSubRoot = -1;
	s_nSubSel = -1;
	s_nHoverSub = -1;
	s_nSub2Root = -1;
	s_nSub2Sel = -1;
	s_nHoverSub2 = -1;
	s_bDirty = 1;
}

// --- nested cascade (level 2) ---------------------------------------------------
// Same geometry rule one level deeper: hangs off the level-1 popup's right edge,
// top aligned with the parent item's row inside that popup.
static const SHORTCUT *Sub2OpenItems(int *pn)
{
	int n1 = 0;
	const SHORTCUT *a1 = SubOpenItems(&n1);
	if (!a1 || s_nSub2Root < 0 || s_nSub2Root >= n1 || a1[s_nSub2Root].sub < 0)
	{
		*pn = 0;
		return NULL;
	}
	return SubItems(a1[s_nSub2Root].sub, pn);
}

static void Sub2Rect(RECT *prc)
{
	RECT r1;
	int n1 = 0, n = 0;
	const SHORTCUT *a1 = SubOpenItems(&n1);
	const SHORTCUT *a2 = Sub2OpenItems(&n);
	int nTop, w, h;
	SubRect(&r1);
	nTop = r1.top + MENU_PAD + (a1 ? MenuItemTop(a1, s_nSub2Root) : 0) - MENU_PAD;
	w = a2 ? MenuItemW(a2, n) + MENU_PAD * 2 : MENU_PAD * 2 + 8;
	h = a2 ? MenuContentH(a2, n) + MENU_PAD * 2 : MENU_PAD * 2;
	if (nTop + h > TASK_Y - 2)
		nTop = TASK_Y - 2 - h;
	if (nTop < 2)
		nTop = 2;
	SetRect(prc, r1.right - MENU_PAD, nTop, r1.right - MENU_PAD + w, nTop + h);
}

static void OpenSub2(int nIdx)
{
	s_nSub2Root = nIdx;
	s_nSub2Sel = -1;
	s_bDirty = 1;
}
static void CloseSub2(void)
{
	s_nSub2Root = -1;
	s_nSub2Sel = -1;
	s_nHoverSub2 = -1;
	s_bDirty = 1;
}

// --- right-click context menu (LT+A on the pad, or the DC mouse's right button) ---
enum
{
	CTXC_OPEN = 1,
	CTXC_DELETE,
	CTXC_ARRANGE,
	CTXC_REFRESH,
	CTXC_DISPLAY,
	CTXC_TASKMGR,
	CTXC_TASKPROP,
	CTXC_PROP
};
typedef struct
{
	const WCHAR *pszLabel;
	int nCmd;
} CtxItem;
static const CtxItem s_aCtxIconUser[] = {
    {L"Open", CTXC_OPEN}, {L"Delete", CTXC_DELETE}, {L"Properties", CTXC_PROP}};
static const CtxItem s_aCtxIconBuiltin[] = {{L"Open", CTXC_OPEN}, {L"Properties", CTXC_PROP}};
static const CtxItem s_aCtxDesktop[] = {
    {L"Arrange Icons", CTXC_ARRANGE}, {L"Refresh", CTXC_REFRESH}, {L"Properties", CTXC_DISPLAY}};
// the stock ceshell MENU/109: right-click on the taskbar itself
static const CtxItem s_aCtxTaskbar[] = {
    {L"Task Manager...", CTXC_TASKMGR}, {L"Properties", CTXC_TASKPROP}};
#define CTX_ROW_H 18
#define CTX_W     128
static const CtxItem *s_pCtxItems = NULL; // NULL = context menu closed
static int s_nCtxCount = 0;
static int s_nCtxSel = 0;
static int s_nCtxX = 0, s_nCtxY = 0; // popup top-left
static int s_nCtxIcon = -1;          // desktop-icon index this menu acts on (-1 = empty desktop)

static int s_nDeskSel = 0;
static int s_bMenuOpen = 0;
static int s_nMenuSel = 0;
static void CloseStartMenu(void); // Start menu + its cascade are one popup tree
static void OpenRunDialog(void);    // "Run..." -> stock Run dialog
static void OpenTaskbarDialog(void);// Settings -> Taskbar -> Taskbar Properties
// the two Start-menu dialogs (implemented below, dispatched by s_nDlgKind)
static void RunDlgOnKey(WPARAM wParam);
static void RunDlgClick(int x, int y);
static void RenderRunFills(void);
static void RenderRunText(HDC hdc);
static void TbDlgOnKey(WPARAM wParam);
static void TbDlgClick(int x, int y);
static void RenderTbFills(void);
static void RenderTbText(HDC hdc);
static int s_bDeskDirty = 1;   // static desktop-layer cache needs a (re)build
static int s_nCachedSel = -1;  // s_nDeskSel baked into the desktop cache
static int s_nCachedMenu = -1; // s_bMenuOpen baked into the desktop cache
static int s_nFocus = -1;      // focused window index, or -1 = desktop
static int s_abWasInUse[DCWIN_MAXWIN];
static DWORD s_adwLastGen[DCWIN_MAXWIN]; // last composited gen per window
static DWORD s_dwLastExec = 0;           // last processed exec request
static HWND s_hwnd = NULL;
static BOOL s_bDiKbd = FALSE;  // DI keyboard acquired (else fall back to WM_KEYDOWN)
static int s_bWmMouseSeen = 0; // logged once when GWES mouse messages arrive
static int s_nCx = SCREEN_W / 2, s_nCy = SCREEN_H / 2; // pointer position
static int s_bBsod = 0;        // a client crashed -> blue-screen overlay is up
static DcCrash s_crash;        // the crash being shown
static DWORD s_dwCrashSeq = 0; // last DcShared.crash.seq we raised a BSOD for
static DWORD s_dwBsodAt = 0;   // tick the BSOD was raised (arm delay before it can be dismissed)

// Desktop icon cell positions (top-left of the 96x54 clickable cell). Mutable so icons can
// be dragged. The stock CEPC layout is ROW-major: three shortcuts across the top
// (Recycle Bin / Inbox / Pocket Word), three on the second row (My Computer /
// My Documents / Internet Explorer); user-added icons keep their dropped pos.
static POINT s_aDeskPos[MAXDESK];
static int s_bDeskPosInit = 0;
static int s_nDeskCols = 3; // built-in icons per row (the stock set is a 3x2 grid)
#define ICELL_W 96
#define ICELL_H 54
#define DESK_X0 14
#define DESK_Y0 8
#define DESK_DX 85 // column pitch (QEMU: ~85px between the real shortcut columns)
#define DESK_DY 80 // row pitch
static void DeskPosInit(void)
{
	int i, nSlot = 0;
	for (i = 0; i < s_cDesk; i++)
	{
		if (s_anDeskUser[i] >= 0)
			continue; // user icons keep their saved / dropped pos
		s_aDeskPos[i].x = DESK_X0 + (nSlot % s_nDeskCols) * DESK_DX;
		s_aDeskPos[i].y = DESK_Y0 + (nSlot / s_nDeskCols) * DESK_DY;
		nSlot++;
	}
	s_bDeskPosInit = 1;
}

// --- dynamic shortcuts + VMU persistence --------------------------------------------
// Persistence blob (one VMU block): 'DCW1' magic, a count, then count records of the
// originating s_start[] index + the icon's desktop position.
#define SHC_MAGIC 0x31574344u // 'D','C','W','1'
typedef struct
{
	short idx, x, y;
} ShcEntry;

static void SaveShortcuts(void)
{
	BYTE abBuf[512];
	DWORD *pdwHdr = (DWORD *)abBuf;
	ShcEntry *pEntry = (ShcEntry *)(abBuf + 8);
	int i, cEntry = 0, cCap = (int)((sizeof(abBuf) - 8) / sizeof(ShcEntry));
	memset(abBuf, 0, sizeof(abBuf));
	for (i = 0; i < s_cDesk && cEntry < cCap; i++)
	{
		if (s_anDeskUser[i] < 0)
			continue;
		pEntry[cEntry].idx = (short)s_anDeskUser[i];
		pEntry[cEntry].x = (short)s_aDeskPos[i].x;
		pEntry[cEntry].y = (short)s_aDeskPos[i].y;
		cEntry++;
	}
	pdwHdr[0] = SHC_MAGIC;
	pdwHdr[1] = (DWORD)cEntry;
	VmuSave(abBuf, 8 + cEntry * (int)sizeof(ShcEntry));
}

static void LoadShortcuts(void)
{
	BYTE abBuf[512];
	DWORD *pdwHdr = (DWORD *)abBuf;
	ShcEntry *pEntry = (ShcEntry *)(abBuf + 8);
	int nGot = 0, i, cEntry;
	if (!VmuLoad(abBuf, sizeof(abBuf), &nGot) || nGot < 8 || pdwHdr[0] != SHC_MAGIC)
		return;
	cEntry = (int)pdwHdr[1];
	for (i = 0; i < cEntry && s_cDesk < MAXDESK; i++)
	{
		int nIdx = pEntry[i].idx;
		if (nIdx < 0 || nIdx >= (int)START_N)
			continue;
		if (!s_start[nIdx].exe && !s_start[nIdx].path)
			continue;
		s_aDesk[s_cDesk] = s_start[nIdx];
		s_anDeskUser[s_cDesk] = nIdx;
		s_aDeskPos[s_cDesk].x = pEntry[i].x;
		s_aDeskPos[s_cDesk].y = pEntry[i].y;
		s_cDesk++;
	}
}

// Seed the live desktop from the built-ins, append persisted user shortcuts, lay out.
static void DeskInit(void)
{
	int i;
	for (i = 0; i < (int)DEFAULT_N; i++)
	{
		s_aDesk[i] = s_deskDefault[i];
		s_anDeskUser[i] = -1;
	}
	s_cDesk = (int)DEFAULT_N;
	// Internet Explorer exists only in the IMGHTML image (chainloaded via dcwboot's boot menu); the
	// lean image has no iexplore.exe in ROM. Show its built-in icon only when it's actually present,
	// so the normal desktop stays clean. It launches via the cooperative-fullscreen path (ShellLaunch
	// -> IsPollSafeFullscreen -> GfxLaunch), like any iexplore.exe shortcut.
	if (GetFileAttributesW(L"\\Windows\\iexplore.exe") != 0xFFFFFFFF && s_cDesk < MAXDESK)
	{
		static const SHORTCUT s_ie = {L"Internet Explorer", NULL, L"iexplore.exe", ICON_IE};
		s_aDesk[s_cDesk] = s_ie;
		s_anDeskUser[s_cDesk] = -1;
		s_cDesk++;
	}
	LoadShortcuts();
	DeskPosInit();
}

// Add a Start-menu item to the desktop at (x,y) and persist. Returns 1 if added.
static int AddDesktopShortcut(int nStartIdx, int x, int y)
{
	if (s_cDesk >= MAXDESK || nStartIdx < 0 || nStartIdx >= (int)START_N)
		return 0;
	if (!s_start[nStartIdx].exe && !s_start[nStartIdx].path)
		return 0; // e.g. "Shut Down..."
	x -= ICELL_W / 2;
	y -= 16;
	if (x < 0)
		x = 0;
	if (x > SCREEN_W - ICELL_W)
		x = SCREEN_W - ICELL_W;
	if (y < 0)
		y = 0;
	if (y > TASK_Y - ICELL_H)
		y = TASK_Y - ICELL_H;
	s_aDesk[s_cDesk] = s_start[nStartIdx];
	s_anDeskUser[s_cDesk] = nStartIdx;
	s_aDeskPos[s_cDesk].x = x;
	s_aDeskPos[s_cDesk].y = y;
	s_nDeskSel = s_cDesk;
	s_cDesk++;
	SaveShortcuts();
	s_bDeskDirty = 1;
	return 1;
}

// Remove a user-added shortcut (built-ins can't be removed) and persist.
static void RemoveShortcut(int i)
{
	int k;
	if (i < 0 || i >= s_cDesk || s_anDeskUser[i] < 0)
		return;
	for (k = i; k < s_cDesk - 1; k++)
	{
		s_aDesk[k] = s_aDesk[k + 1];
		s_anDeskUser[k] = s_anDeskUser[k + 1];
		s_aDeskPos[k] = s_aDeskPos[k + 1];
	}
	s_cDesk--;
	if (s_nDeskSel >= s_cDesk)
		s_nDeskSel = s_cDesk - 1;
	if (s_nDeskSel < 0)
		s_nDeskSel = 0;
	SaveShortcuts();
	s_bDeskDirty = 1;
}

// Pointer drag state machine (mouse-L or controller-A held). Lets the user move/resize
// windows and move desktop shortcuts; a press with no drag is treated as a click on release.
#define DRAG_NONE      0
#define DRAG_WMOVE     1 // moving a window (grabbed its title bar)
#define DRAG_WSIZE     2 // resizing a window (grabbed its bottom-right corner)
#define DRAG_ICON      3 // moving a desktop shortcut
#define DRAG_STARTITEM 4 // dragging a Start-menu item out onto the desktop (creates a shortcut)
#define DRAG_THRESH    3 // px of motion before a press becomes a drag (vs a click)
#define WIN_MINW       80
#define WIN_MINH       40
static int s_nDragKind = DRAG_NONE, s_nDragTarget = -1;
static int s_nDragOffX = 0, s_nDragOffY = 0; // cursor - target origin, at grab time
static int s_bDragMoved = 0, s_bPtrWas = 0, s_nDownX = 0, s_nDownY = 0;

// Window maximize/restore + minimize (shell-side, keyed by window slot). s_awinRestore =
// {x,y,w,h} saved before maximizing. Both reset when a slot frees so a reused slot is normal.
static int s_abWinMax[DCWIN_MAXWIN];
static int s_awinRestore[DCWIN_MAXWIN][4];
static int s_abWinMin[DCWIN_MAXWIN]; // window minimized (hidden; only its taskbar button shows)

static DcShared *s_pShared = NULL;
static HANDLE s_hSharedMap = NULL;

// seqlock snapshot of each window's command list (avoids reading a half-written frame)
static DcCmd s_aSnap[DCWIN_MAXWIN][DCWIN_MAXCMD];
static int s_anSnapN[DCWIN_MAXWIN];

static void DbgStr(const WCHAR *psz)
{
	OutputDebugStringW(psz);
}

// Raise the blue screen for the crash already filled into s_crash. Arms a short delay and drops
// the input that caused the launch/crash so it can't dismiss the screen the same instant.
static void RaiseBsod(void)
{
	DWORD dwVk;
	s_bBsod = 1;
	s_bDirty = 1;
	s_dwBsodAt = GetTickCount();
	DbgStr(L"DCSHELL: BSOD raised\r\n");
	while (DInNextKey(&dwVk))
		;
	DInTookActivate();
	DInTookContext();
	DInTookClick();
}

// A fullscreen app (no dcwlib SEH wrapper) exited. We can only blue-screen it if the exit code is
// a real NTSTATUS fault. CONFIRMED on hardware: CE 2.12 reports a plain exit code 1 for an SH-4
// fault (the kernel logs the real exception to the debug port, but GetExitCodeProcess just sees
// 1), so we CANNOT tell a third-party game crash from a normal exit - those go uncaught here. This
// only fires if some app actually exits with a 0xCxxxxxxx status; normal exits never do.
static void CheckFullscreenCrash(const WCHAR *pszExe, DWORD dwExit)
{
	const WCHAR *pszBase = pszExe, *p;
	int i;
	if ((dwExit & 0xF0000000u) != 0xC0000000u)
		return; // plain exit code (incl. CE's "1" for a fault) -> can't distinguish a crash
	for (p = pszExe; *p; p++)
		if (*p == L'\\' || *p == L'/')
			pszBase = p + 1;
	memset(&s_crash, 0, sizeof(s_crash));
	s_crash.code = dwExit;
	s_crash.access = 0xFFFFFFFF; // no SEH record for a fullscreen app -> no r/w address
	for (i = 0; i < 31 && pszBase[i]; i++)
		s_crash.proc[i] = pszBase[i];
	s_crash.proc[i] = 0;
	RaiseBsod();
}

//
// Compositor shared section + launching
//
static void InitShared(void)
{
	s_hSharedMap =
	    CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(DcShared), DCWIN_SECTION);
	if (!s_hSharedMap)
	{
		DbgStr(L"DCSHELL: shared section create FAILED\r\n");
		return;
	}
	s_pShared = (DcShared *)MapViewOfFile(s_hSharedMap, FILE_MAP_WRITE, 0, 0, sizeof(DcShared));
	if (!s_pShared)
	{
		DbgStr(L"DCSHELL: shared section map FAILED\r\n");
		return;
	}
	memset(s_pShared, 0, sizeof(DcShared));
	s_pShared->magic = DCWIN_MAGIC;
	DbgStr(L"DCSHELL: compositor ready\r\n");
}

static void LaunchApp(const WCHAR *pszExe, const WCHAR *pszArgs)
{
	PROCESS_INFORMATION pi;
	WCHAR szCl[MAX_PATH];
	if (!s_pShared || !pszExe)
		return;
	if (pszArgs)
		lstrcpyW(szCl, pszArgs);
	if (CreateProcessW(pszExe, pszArgs ? szCl : NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess); // detached; we poll the shared section each frame
		DbgStr(L"DCSHELL: launched app\r\n");
	}
	else
		DbgStr(L"DCSHELL: CreateProcess(app) FAILED\r\n");
}

// dcw*.exe are windowed DCWin apps (composited); anything else gets the display.
static BOOL IsDcwApp(const WCHAR *pszPath)
{
	const WCHAR *pszBase = pszPath, *p;
	for (p = pszPath; *p; p++)
		if (*p == L'\\')
			pszBase = p + 1;
	return (pszBase[0] | 32) == 'd' && (pszBase[1] | 32) == 'c' && (pszBase[2] | 32) == 'w';
}

static void ShellLaunch(const WCHAR *pszCmd)
{
	WCHAR szExe[MAX_PATH];
	const WCHAR *pszArgs = NULL;
	int i;
	for (i = 0; pszCmd[i] && pszCmd[i] != L' ' && i < MAX_PATH - 1; i++)
		szExe[i] = pszCmd[i]; // split exe arg
	szExe[i] = 0;
	if (pszCmd[i] == L' ')
		pszArgs = pszCmd + i + 1; // e.g. "dcwplay.exe \CD-ROM\song.mp3"
	if (IsDcwApp(szExe))
		LaunchApp(szExe, pszArgs); // windowed, composited (+ optional file argument)
	else
	{
		// Fullscreen app - our browser OR a retail game. Hand over ALL input + the display and do
		// NOT poll while it runs. On the DC the shell and the app cannot BOTH hold DirectInput: any
		// acquire the shell keeps starves the app (the browser came up with dead input), and a
		// retail game drives the Maple bus raw and faults to the BIOS if we so much as read it. So
		// we fully release; every fullscreen app manages its own exit (the browser via its own
		// chrome). Desktop panic combos on WINDOWED apps are unaffected - this is only the hand-off.
		DbgStr(L"DCSHELL: fullscreen launch (full input hand-off)\r\n");
		DInRelease();
		GfxLaunch(szExe, NULL); // release display -> run (blocks, INFINITE wait) -> reclaim
		DInReacquire();
		CheckFullscreenCrash(szExe, GfxLastExitCode());
		s_bDirty = 1;
		s_bDeskDirty = 1; // surfaces/icons rebuilt -> recache desktop
	}
}

//
// Display Properties dialog (shell-internal modal), NT 4.0 "Background" tab styling: a monitor
// graphic previewing the wallpaper, a dropdown to pick it, a Style dropdown, OK/Cancel/Apply.
// Choosing a wallpaper live-applies it (the monitor preview + desktop update); Cancel reverts.
//
#define DLG_W        400
#define DLG_H        400
#define DLG_ROW      16
#define WALL_MAX     16
#define DLG_CTL_N    5           // wallpaper combo, style combo, OK, Cancel, Apply
#define DISP_MAGIC   0x50534944u // 'D','I','S','P'
#define DISP_VMUFILE "DCWINDSP.BIN"
typedef struct
{
	DWORD dwMagic;
	DWORD dwStyle;    // GFXWALL_*
	WCHAR szWall[64]; // wallpaper file name (\Windows ROM or \CD-ROM\Wallpaper; empty = none)
} DispCfg;

static WCHAR s_aWall[WALL_MAX][64]; // [0] = "(None)", [1] = the built-in CE logo, then *.bmp on disc
static int s_abWallRom[WALL_MAX];   // entry lives in \Windows (ROM) rather than \CD-ROM\Wallpaper
static int s_cWall = 0;             // entry count (incl. "(None)")
static int s_nCurWall = 0;          // committed wallpaper index + style (survive Cancel)
static int s_nCurStyle = GFXWALL_STRETCH;
static int s_bDlgOpen = 0; // the modal is up
// Which modal owns s_bDlgOpen: the wallpaper dialog (stock "Display Properties"-style)
// or the Start-menu dialogs.
#define DLGK_DISPLAY 0
#define DLGK_RUN     1
#define DLGK_TASKBAR 2
static int s_nDlgKind = DLGK_DISPLAY;
static int s_nDlgSel = 0;  // focused control (0..DLG_CTL_N-1)
static int s_nDlgWall = 0; // chosen wallpaper while the dialog is open
static int s_nDlgStyle = GFXWALL_STRETCH;
static int s_nDdActive = -1; // open dropdown: -1 none, 0 = wallpaper, 1 = style
static int s_nDdSel = 0;     // highlighted item in the open dropdown
static const WCHAR *const s_aStyleName[2] = {L"Stretch", L"Center"};

static void ScanWallpapers(void)
{
	WIN32_FIND_DATAW fd;
	HANDLE h;
	wsprintfW(s_aWall[0], L"(None)");
	s_abWallRom[0] = 0;
	s_cWall = 1;
	// the stock CE desktop brand: wince.bmp baked into the ROM (\Windows), so it exists even
	// on a bare image with no disc data
	if (s_cWall < WALL_MAX &&
	    GetFileAttributesW(L"\\Windows\\wince.bmp") != 0xFFFFFFFF)
	{
		wsprintfW(s_aWall[s_cWall], L"wince.bmp");
		s_abWallRom[s_cWall] = 1;
		s_cWall++;
	}
	h = FindFirstFileW(L"\\CD-ROM\\Wallpaper\\*.bmp", &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (s_cWall < WALL_MAX)
			{
				wsprintfW(s_aWall[s_cWall], L"%s", fd.cFileName);
				s_abWallRom[s_cWall] = 0;
				s_cWall++;
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
}

static void WallPath(int idx, WCHAR *pszOut) // full path for entry idx ("" = none)
{
	if (idx <= 0 || idx >= s_cWall)
		pszOut[0] = 0;
	else
		wsprintfW(pszOut, L"%s%s", s_abWallRom[idx] ? L"\\Windows\\" : L"\\CD-ROM\\Wallpaper\\",
		          s_aWall[idx]);
}

static void ApplyWall(int idx, int nStyle) // push the choice to the live desktop
{
	WCHAR szPath[MAX_PATH];
	WallPath(idx, szPath);
	GfxSetWallpaper(szPath[0] ? szPath : NULL, nStyle);
	s_bDeskDirty = 1;
}

static void PersistDisplay(void) // save the committed choice to its own VMU block
{
	DispCfg c;
	memset(&c, 0, sizeof(c));
	c.dwMagic = DISP_MAGIC;
	c.dwStyle = (DWORD)s_nCurStyle;
	if (s_nCurWall > 0 && s_nCurWall < s_cWall)
		wsprintfW(c.szWall, L"%s", s_aWall[s_nCurWall]);
	VmuSaveAs(DISP_VMUFILE, &c, sizeof(c));
}

// Rect of dialog control idx (0 wallpaper combo, 1 style combo, 2 OK, 3 Cancel, 4 Apply).
static void DlgCtlRect(int idx, RECT *pr)
{
	int x0 = (SCREEN_W - DLG_W) / 2, y0 = (SCREEN_H - DLG_H) / 2;
	switch (idx)
	{
		case 0:
			SetRect(pr, x0 + 50, y0 + 232, x0 + 350, y0 + 252);
			break;
		case 1:
			SetRect(pr, x0 + 50, y0 + 274, x0 + 210, y0 + 294);
			break;
		case 2:
			SetRect(pr, x0 + 70, y0 + 350, x0 + 140, y0 + 376);
			break;
		case 3:
			SetRect(pr, x0 + 160, y0 + 350, x0 + 230, y0 + 376);
			break;
		default:
			SetRect(pr, x0 + 250, y0 + 350, x0 + 320, y0 + 376);
			break;
	}
}

static int DdCount(void) // items in the open dropdown
{
	return s_nDdActive == 0 ? s_cWall : (s_nDdActive == 1 ? 2 : 0);
}
static const WCHAR *DdText(int i)
{
	return s_nDdActive == 0 ? s_aWall[i] : s_aStyleName[i];
}
static void DdListRect(RECT *pr) // the open dropdown's list box (below its combo)
{
	RECT cb;
	DlgCtlRect(s_nDdActive == 1 ? 1 : 0, &cb);
	SetRect(pr, cb.left, cb.bottom, cb.right, cb.bottom + DdCount() * DLG_ROW + 4);
}

static void CloseDisplayDialog(int bApply)
{
	if (!bApply) // Cancel -> revert the live desktop to the committed choice
		ApplyWall(s_nCurWall, s_nCurStyle);
	s_bDlgOpen = 0;
	s_nDlgKind = DLGK_DISPLAY;
	s_nDdActive = -1;
	s_bDirty = 1;
	s_bDeskDirty = 1;
}

static void DlgCommit(void) // OK / Apply: commit the live choice + persist
{
	s_nCurWall = s_nDlgWall;
	s_nCurStyle = s_nDlgStyle;
	PersistDisplay();
}

static void DdSelect(int i) // pick item i in the open dropdown, live-apply, close it
{
	if (s_nDdActive == 0 && i >= 0 && i < s_cWall)
		s_nDlgWall = i;
	else if (s_nDdActive == 1 && i >= 0 && i < 2)
		s_nDlgStyle = i;
	s_nDdActive = -1;
	ApplyWall(s_nDlgWall, s_nDlgStyle); // live preview: monitor + desktop
	s_bDirty = 1;
}

static void DlgActivate(void) // A / click on the focused control
{
	switch (s_nDlgSel)
	{
		case 0: // wallpaper combo -> open its dropdown
			s_nDdActive = 0;
			s_nDdSel = s_nDlgWall;
			break;
		case 1: // style combo -> open its dropdown
			s_nDdActive = 1;
			s_nDdSel = s_nDlgStyle;
			break;
		case 2: // OK
			DlgCommit();
			CloseDisplayDialog(1);
			break;
		case 3: // Cancel
			CloseDisplayDialog(0);
			break;
		default: // Apply
			DlgCommit();
			break;
	}
	s_bDirty = 1;
}

static void OpenDisplayDialog(void)
{
	ScanWallpapers();
	s_nDlgWall = (s_nCurWall >= 0 && s_nCurWall < s_cWall) ? s_nCurWall : 0;
	s_nDlgStyle = s_nCurStyle;
	s_nDlgSel = 0;
	s_nDlgKind = DLGK_DISPLAY;
	s_nDdActive = -1;
	s_bDlgOpen = 1;
	s_bDirty = 1;
}

static void DlgOnKey(WPARAM wParam)
{
	if (s_nDlgKind == DLGK_RUN)
	{
		RunDlgOnKey(wParam);
		return;
	}
	if (s_nDlgKind == DLGK_TASKBAR)
	{
		TbDlgOnKey(wParam);
		return;
	}
	if (s_nDdActive >= 0) // a dropdown is open: navigate its items
	{
		int n = DdCount();
		if (wParam == VK_UP && s_nDdSel > 0)
			s_nDdSel--;
		else if (wParam == VK_DOWN && s_nDdSel < n - 1)
			s_nDdSel++;
		else if (wParam == VK_RETURN)
			DdSelect(s_nDdSel);
		else if (wParam == VK_ESCAPE)
			s_nDdActive = -1; // close, no change
		s_bDirty = 1;
		return;
	}
	if (wParam == VK_UP && s_nDlgSel > 0)
		s_nDlgSel--;
	else if (wParam == VK_DOWN && s_nDlgSel < DLG_CTL_N - 1)
		s_nDlgSel++;
	else if (wParam == VK_RETURN)
		DlgActivate();
	else if (wParam == VK_ESCAPE)
		CloseDisplayDialog(0);
	s_bDirty = 1;
}

static void DlgClick(int x, int y)
{
	int i;
	if (s_nDlgKind == DLGK_RUN)
	{
		RunDlgClick(x, y);
		return;
	}
	if (s_nDlgKind == DLGK_TASKBAR)
	{
		TbDlgClick(x, y);
		return;
	}
	if (s_nDdActive >= 0) // dropdown open: a row picks, anywhere else closes it
	{
		RECT lr;
		DdListRect(&lr);
		if (x >= lr.left && x < lr.right && y >= lr.top + 2 && y < lr.top + 2 + DdCount() * DLG_ROW)
			DdSelect((y - (lr.top + 2)) / DLG_ROW);
		else
			s_nDdActive = -1;
		s_bDirty = 1;
		return;
	}
	for (i = 0; i < DLG_CTL_N; i++)
	{
		RECT cr;
		DlgCtlRect(i, &cr);
		if (x >= cr.left && x < cr.right && y >= cr.top && y < cr.bottom)
		{
			s_nDlgSel = i;
			DlgActivate();
			return;
		}
	}
	s_bDirty = 1; // elsewhere in the modal: ignore
}

// --- dialog render ---------------------------------------------------------------
static void DlgComboFill(int idx) // sunken white field + a raised drop-arrow button
{
	RECT cr, ar;
	DlgCtlRect(idx, &cr);
	GfxFill(cr.left, cr.top, cr.right, cr.bottom, CL_WHITE);
	GfxBevel(&cr, FALSE);
	SetRect(&ar, cr.right - 16, cr.top + 1, cr.right - 1, cr.bottom - 1);
	GfxFill(ar.left, ar.top, ar.right, ar.bottom, (idx == s_nDlgSel) ? CL_SEL : CL_FACE);
	GfxBevel(&ar, TRUE); // drop arrow (blue when focused)
}

static void RenderDialogFills(void)
{
	RECT rc;
	int x0, y0, mx, my, b;
	if (!s_bDlgOpen)
		return;
	if (s_nDlgKind == DLGK_RUN)
	{
		RenderRunFills();
		return;
	}
	if (s_nDlgKind == DLGK_TASKBAR)
	{
		RenderTbFills();
		return;
	}
	x0 = (SCREEN_W - DLG_W) / 2;
	y0 = (SCREEN_H - DLG_H) / 2;
	SetRect(&rc, x0, y0, x0 + DLG_W, y0 + DLG_H);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	GfxFill(x0 + 2, y0 + 2, x0 + DLG_W - 2, y0 + 20, CL_TITLE); // title bar
	SetRect(&rc, x0 + 8, y0 + 26, x0 + 96, y0 + 44);            // Background tab
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	// monitor graphic: bezel + screen (wallpaper preview) + stand
	mx = x0 + (DLG_W - 180) / 2;
	my = y0 + 50;
	SetRect(&rc, mx, my, mx + 180, my + 150);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	SetRect(&rc, mx + 14, my + 12, mx + 166, my + 126);
	GfxBevel(&rc, FALSE); // sunken screen frame
	if (!GfxDrawWallpaperRect(mx + 15, my + 13, 150, 112))
		GfxFill(mx + 15, my + 13, mx + 165, my + 125, CL_DESKTOP); // (None) -> teal
	GfxFill(mx + 80, my + 150, mx + 100, my + 160, CL_FACE);       // stand neck
	SetRect(&rc, mx + 62, my + 160, mx + 118, my + 168);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE); // base
	GfxBevel(&rc, TRUE);
	DlgComboFill(0); // wallpaper + style combos
	DlgComboFill(1);
	for (b = 2; b < DLG_CTL_N; b++) // OK / Cancel / Apply
	{
		DlgCtlRect(b, &rc);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, (b == s_nDlgSel) ? CL_SEL : CL_FACE);
		GfxBevel(&rc, TRUE);
	}
	if (s_nDdActive >= 0) // open dropdown list box
	{
		DdListRect(&rc);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_WHITE);
		GfxBevel(&rc, TRUE);
		if (s_nDdSel >= 0 && s_nDdSel < DdCount())
			GfxFill(rc.left + 2, rc.top + 2 + s_nDdSel * DLG_ROW, rc.right - 2,
			        rc.top + 2 + (s_nDdSel + 1) * DLG_ROW, CL_SEL);
	}
}

static void RenderDialogText(HDC hdc)
{
	int x0, y0, i;
	RECT cr;
	if (!s_bDlgOpen)
		return;
	if (s_nDlgKind == DLGK_RUN)
	{
		RenderRunText(hdc);
		return;
	}
	if (s_nDlgKind == DLGK_TASKBAR)
	{
		RenderTbText(hdc);
		return;
	}
	x0 = (SCREEN_W - DLG_W) / 2;
	y0 = (SCREEN_H - DLG_H) / 2;
	GfxText(hdc, x0 + 8, y0 + 4, CL_WHITE, CL_TITLE, g_FontBold, L"Display Properties");
	GfxText(hdc, x0 + 16, y0 + 29, CL_TEXT, CL_FACE, g_FontUI, L"Background");
	GfxText(hdc, x0 + 50, y0 + 216, CL_TEXT, CL_FACE, g_FontUI, L"Wallpaper:");
	DlgCtlRect(0, &cr);
	GfxText(hdc, cr.left + 4, cr.top + 3, CL_TEXT, CL_WHITE, g_FontUI, s_aWall[s_nDlgWall]);
	GfxText(hdc, x0 + 50, y0 + 258, CL_TEXT, CL_FACE, g_FontUI, L"Display:");
	DlgCtlRect(1, &cr);
	GfxText(hdc, cr.left + 4, cr.top + 3, CL_TEXT, CL_WHITE, g_FontUI, s_aStyleName[s_nDlgStyle]);
	DlgCtlRect(2, &cr);
	GfxText(hdc, cr.left + 24, cr.top + 6, (s_nDlgSel == 2) ? CL_WHITE : CL_TEXT,
	        (s_nDlgSel == 2) ? CL_SEL : CL_FACE, g_FontUI, L"OK");
	DlgCtlRect(3, &cr);
	GfxText(hdc, cr.left + 14, cr.top + 6, (s_nDlgSel == 3) ? CL_WHITE : CL_TEXT,
	        (s_nDlgSel == 3) ? CL_SEL : CL_FACE, g_FontUI, L"Cancel");
	DlgCtlRect(4, &cr);
	GfxText(hdc, cr.left + 18, cr.top + 6, (s_nDlgSel == 4) ? CL_WHITE : CL_TEXT,
	        (s_nDlgSel == 4) ? CL_SEL : CL_FACE, g_FontUI, L"Apply");
	GfxText(hdc, x0 + 16, y0 + DLG_H - 18, CL_TEXT, CL_FACE, g_FontUI,
	        L"640 x 480    True Color    60 Hz");
	if (s_nDdActive >= 0) // dropdown list text
	{
		RECT lr;
		int n = DdCount();
		DdListRect(&lr);
		for (i = 0; i < n; i++)
			GfxText(hdc, lr.left + 4, lr.top + 3 + i * DLG_ROW,
			        (i == s_nDdSel) ? CL_WHITE : CL_TEXT, (i == s_nDdSel) ? CL_SEL : CL_WHITE,
			        g_FontUI, DdText(i)); // list box is white
	}
}

//
// "Run" dialog (stock DLG1003): icon + "Type the name of a program, folder, or
// document, and Windows will open it for you." + an edit field + OK / Cancel /
// Browse... laid out like the CE 2.x dialog (buttons stacked at the right).
//
// Stock Run dialog (DLG1003) laid out off the CEPC capture: navy caption with the
// run icon + "Run" + a close box, 32px icon at left, the two-line instruction text,
// an "Open:" label with a sunken combo (edit + dropdown button), and OK/Cancel/
// Browse... as a horizontal row along the bottom. OK starts disabled until the
// field is non-empty (greyed caption, like stock).
#define RUN_W     430
#define RUN_H     170
#define RUN_CTL_N 5 // edit field, OK, Cancel, Browse..., caption close

static WCHAR s_szRun[128];
static int s_nRunLen = 0;

static void OpenRunDialog(void)
{
	s_nDlgKind = DLGK_RUN;
	s_nRunLen = 0;
	s_szRun[0] = 0;
	s_nDlgSel = 0; // focus lands in the edit field
	s_nDdActive = -1;
	s_bDlgOpen = 1;
	s_bDirty = 1;
}

static void RunCtlRect(int i, RECT *pr)
{
	int x0 = (SCREEN_W - RUN_W) / 2, y0 = (SCREEN_H - RUN_H) / 2;
	switch (i)
	{
		case 0:
			SetRect(pr, x0 + 60, y0 + 80, x0 + RUN_W - 44, y0 + 100);
			break; // edit (combo field, dropdown sits right of it)
		case 1:
			SetRect(pr, x0 + 72, y0 + 128, x0 + 162, y0 + 152);
			break; // OK
		case 2:
			SetRect(pr, x0 + 170, y0 + 128, x0 + 260, y0 + 152);
			break; // Cancel
		case 3:
			SetRect(pr, x0 + 268, y0 + 128, x0 + 358, y0 + 152);
			break; // Browse...
		default:
			SetRect(pr, x0 + RUN_W - 17, y0 + 4, x0 + RUN_W - 3, y0 + 21);
			break; // caption close (X)
	}
}

// VK -> edit-field character, unshifted US layout; 0 for non-text keys.
static int RunChar(WPARAM wParam)
{
	if (wParam >= 'A' && wParam <= 'Z')
		return (int)wParam + 32;
	if (wParam >= '0' && wParam <= '9')
		return (int)wParam;
	switch (wParam)
	{
		case VK_SPACE:
			return ' ';
		case VK_OEM_MINUS:
			return '-';
		case VK_OEM_PLUS:
			return '=';
		case VK_OEM_4:
			return '[';
		case VK_OEM_6:
			return ']';
		case VK_OEM_5:
			return '\\';
		case VK_OEM_1:
			return ';';
		case VK_OEM_7:
			return '\'';
		case VK_OEM_3:
			return '`';
		case VK_OEM_COMMA:
			return ',';
		case VK_OEM_PERIOD:
			return '.';
		case VK_OEM_2:
			return '/';
	}
	return 0;
}

static void RunActivate(void)
{
	switch (s_nDlgSel)
	{
		case 1: // OK -> run the typed command line (disabled while the field is empty)
			if (s_nRunLen > 0)
			{
				s_bDlgOpen = 0;
				ShellLaunch(s_szRun);
			}
			break;
		case 3: // Browse... -> the file explorer, like the real browse dialog
			s_bDlgOpen = 0;
			LaunchApp(L"dcwexp.exe", L"\\Windows");
			break;
		default: // Cancel or the caption close box
			s_bDlgOpen = 0;
			break;
	}
	s_bDirty = 1;
}

static void RunDlgOnKey(WPARAM wParam)
{
	if (s_nDlgSel == 0) // edit focused: text input first
	{
		int ch = RunChar(wParam);
		if (ch && s_nRunLen < 126)
		{
			s_szRun[s_nRunLen++] = (WCHAR)ch;
			s_szRun[s_nRunLen] = 0;
		}
		else if (wParam == VK_BACK && s_nRunLen > 0)
			s_szRun[--s_nRunLen] = 0;
		else if (wParam == VK_DELETE)
			s_nRunLen = 0, s_szRun[0] = 0;
		else if (wParam == VK_RETURN)
			RunActivate();
		else if (wParam == VK_ESCAPE)
			s_bDlgOpen = 0;
		else if (wParam == VK_UP || wParam == VK_DOWN)
			s_nDlgSel = 1;
		else if (wParam == VK_TAB) // stock tab order: edit -> OK -> Cancel -> Browse -> edit
			s_nDlgSel = 1;
		s_bDirty = 1;
		return;
	}
	if (wParam == VK_LEFT && s_nDlgSel > 1)
		s_nDlgSel--;
	else if (wParam == VK_RIGHT && s_nDlgSel < 3)
		s_nDlgSel++;
	else if (wParam == VK_UP || wParam == VK_DOWN) // back to the edit field
		s_nDlgSel = 0;
	else if (wParam == VK_RETURN)
		RunActivate();
	else if (wParam == VK_ESCAPE)
		s_bDlgOpen = 0;
	else if (wParam == VK_TAB)
		s_nDlgSel = (s_nDlgSel < 3) ? s_nDlgSel + 1 : 0;
	else if (RunChar(wParam) || wParam == VK_BACK) // typing jumps back into the field
	{
		s_nDlgSel = 0;
		RunDlgOnKey(wParam);
		return;
	}
	s_bDirty = 1;
}

static void RunDlgClick(int x, int y)
{
	int i;
	for (i = 0; i < RUN_CTL_N; i++)
	{
		RECT cr;
		RunCtlRect(i, &cr);
		if (x >= cr.left && x < cr.right && y >= cr.top && y < cr.bottom)
		{
			s_nDlgSel = i;
			if (i > 0)
				RunActivate();
			s_bDirty = 1;
			return;
		}
	}
	// the "Open:" label + combo row also focus the edit field
	{
		RECT cr;
		SetRect(&cr, (SCREEN_W - RUN_W) / 2, (SCREEN_H - RUN_H) / 2 + 70,
		        (SCREEN_W + RUN_W) / 2, (SCREEN_H - RUN_H) / 2 + 104);
		if (x >= cr.left && x < cr.right && y >= cr.top && y < cr.bottom)
		{
			s_nDlgSel = 0;
			s_bDirty = 1;
		}
	}
}

static void RenderRunFills(void)
{
	RECT rc;
	int x0 = (SCREEN_W - RUN_W) / 2, y0 = (SCREEN_H - RUN_H) / 2, i;
	SetRect(&rc, x0, y0, x0 + RUN_W, y0 + RUN_H);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	GfxFill(x0 + 2, y0 + 2, x0 + RUN_W - 2, y0 + CAPT_H + 2, CL_TITLE); // navy caption
	GfxIcon(ICON_RUN, x0 + 5, y0 + 5);                                // caption icon
	GfxIconBig(ICON_RUN, x0 + 16, y0 + 38);                           // 32px icon at left
	for (i = 1; i < RUN_CTL_N; i++)
	{
		RunCtlRect(i, &rc);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
		GfxBevel(&rc, TRUE);
		if (i == s_nDlgSel && s_nDlgSel != 0)
		{ // focus = a second dark edge inside the bevel (stock focused-button look)
			GfxFill(rc.left + 2, rc.top + 2, rc.right - 2, rc.top + 3, RGB(96, 96, 96));
			GfxFill(rc.left + 2, rc.bottom - 3, rc.right - 2, rc.bottom - 2,
			        RGB(96, 96, 96));
			GfxFill(rc.left + 2, rc.top + 2, rc.left + 3, rc.bottom - 2, RGB(96, 96, 96));
			GfxFill(rc.right - 3, rc.top + 2, rc.right - 2, rc.bottom - 2,
			        RGB(96, 96, 96));
		}
	}
	RunCtlRect(0, &rc); // sunken white edit field
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_WHITE);
	GfxBevel(&rc, FALSE);
	{ // combo dropdown button right of the field (stock combo look)
		RECT dd;
		SetRect(&dd, rc.right + 2, rc.top, rc.right + 26, rc.bottom);
		GfxFill(dd.left, dd.top, dd.right, dd.bottom, CL_FACE);
		GfxBevel(&dd, TRUE);
		{ // black down-arrow glyph
			int dx = dd.left + (dd.right - dd.left - 9) / 2, dy = dd.top + 6, r;
			for (r = 0; r < 5; r++)
				GfxFill(dx + r, dy + r, dx + 9 - r, dy + r + 1, CL_TEXT);
		}
	}
	if (s_nDlgSel == 0) // block caret at the end of the text (fills pass - no fills in GetDC)
		GfxFill(rc.left + 4 + GfxTextWidth(g_FontUI, s_szRun), rc.top + 4,
		        rc.left + 10 + GfxTextWidth(g_FontUI, s_szRun), rc.bottom - 4, CL_TEXT);
	{ // caption close box: X glyph on a bevelled button
		RECT cc;
		static const char *s_aXArt[9] = {"XX.....XX", "XXX...XXX", ".XXX.XXX.", "..XXXXX..",
		                                 "...XXX...", "..XXXXX..", ".XXX.XXX.", "XXX...XXX",
		                                 "XX.....XX"};
		int gx, gy;
		RunCtlRect(4, &cc);
		for (gy = 0; gy < 9; gy++)
			for (gx = 0; gx < 9; gx++)
				if (s_aXArt[gy][gx] == 'X')
					GfxFill(cc.left + 3 + gx, cc.top + 4 + gy, cc.left + 4 + gx,
					        cc.top + 5 + gy, CL_TEXT);
	}
}

static void RenderRunText(HDC hdc)
{
	RECT cr;
	int x0 = (SCREEN_W - RUN_W) / 2, y0 = (SCREEN_H - RUN_H) / 2;
	GfxText(hdc, x0 + 24, y0 + 5, CL_WHITE, CL_TITLE, g_FontBold, L"Run");
	GfxText(hdc, x0 + 56, y0 + 40, CL_TEXT, CL_FACE, g_FontUI,
	        L"Type the name of a program, folder, or");
	GfxText(hdc, x0 + 56, y0 + 54, CL_TEXT, CL_FACE, g_FontUI,
	        L"document, and Windows will open it for you.");
	GfxText(hdc, x0 + 16, y0 + 84, CL_TEXT, CL_FACE, g_FontUI, L"Open:");
	RunCtlRect(0, &cr);
	GfxText(hdc, cr.left + 4, cr.top + 4, CL_TEXT, CL_WHITE, g_FontUI, s_szRun);
	RunCtlRect(1, &cr); // OK greys out while the field is empty, like stock
	GfxText(hdc, cr.left + 37, cr.top + 7,
	        (s_nRunLen > 0) ? CL_TEXT : RGB(128, 128, 128), CL_FACE, g_FontUI, L"OK");
	RunCtlRect(2, &cr);
	GfxText(hdc, cr.left + 28, cr.top + 7, CL_TEXT, CL_FACE, g_FontUI, L"Cancel");
	RunCtlRect(3, &cr);
	GfxText(hdc, cr.left + 21, cr.top + 7, CL_TEXT, CL_FACE, g_FontUI, L"Browse...");
}

//
// "Taskbar Properties" (stock DLG108 / "Taskbar Options" tab): Always on top /
// Auto hide / Show Clock checkboxes + OK / Cancel. The options are live: Auto hide
// drops the taskbar to a 2px sliver until the cursor touches the bottom edge (or a
// menu/dialog opens), Show Clock removes the tray clock.
//
#define TB_W      230
#define TB_H      150
#define TB_CTL_N  5 // Always on top, Auto hide, Show Clock, OK, Cancel
#define TB_CHK_Y0 32
#define TB_CHK_DY 22

static int s_abTbDlg[3];                // working copies while the dialog is open
static int s_bTbAlwaysTop = 1, s_bTbAutoHide = 0, s_bTbClock = 1; // committed options

static void TbCtlRect(int i, RECT *pr)
{
	int x0 = (SCREEN_W - TB_W) / 2, y0 = (SCREEN_H - TB_H) / 2;
	if (i < 3)
		SetRect(pr, x0 + 12, y0 + TB_CHK_Y0 + i * TB_CHK_DY, x0 + TB_W - 12,
		        y0 + TB_CHK_Y0 + i * TB_CHK_DY + 16);
	else
		SetRect(pr, x0 + (i == 3 ? 50 : 130), y0 + TB_H - 30, x0 + (i == 3 ? 100 : 180),
		        y0 + TB_H - 10);
}

static void OpenTaskbarDialog(void)
{
	s_nDlgKind = DLGK_TASKBAR;
	s_abTbDlg[0] = s_bTbAlwaysTop;
	s_abTbDlg[1] = s_bTbAutoHide;
	s_abTbDlg[2] = s_bTbClock;
	s_nDlgSel = 0;
	s_nDdActive = -1;
	s_bDlgOpen = 1;
	s_bDirty = 1;
}

static void TbActivate(void)
{
	if (s_nDlgSel < 3) // checkboxes toggle in place
		s_abTbDlg[s_nDlgSel] = !s_abTbDlg[s_nDlgSel];
	else if (s_nDlgSel == 3) // OK -> commit
	{
		s_bTbAlwaysTop = s_abTbDlg[0];
		s_bTbAutoHide = s_abTbDlg[1];
		s_bTbClock = s_abTbDlg[2];
		s_bDlgOpen = 0;
	}
	else // Cancel
		s_bDlgOpen = 0;
	s_bDirty = 1;
}

static void TbDlgOnKey(WPARAM wParam)
{
	if (wParam == VK_UP && s_nDlgSel > 0)
		s_nDlgSel--;
	else if (wParam == VK_DOWN && s_nDlgSel < TB_CTL_N - 1)
		s_nDlgSel++;
	else if (wParam == VK_RETURN || wParam == VK_SPACE)
		TbActivate();
	else if (wParam == VK_ESCAPE)
		s_bDlgOpen = 0;
	s_bDirty = 1;
}

static void TbDlgClick(int x, int y)
{
	int i;
	for (i = 0; i < TB_CTL_N; i++)
	{
		RECT cr;
		TbCtlRect(i, &cr);
		if (x >= cr.left && x < cr.right && y >= cr.top && y < cr.bottom)
		{
			s_nDlgSel = i;
			TbActivate();
			return;
		}
	}
}

static void RenderTbFills(void)
{
	RECT rc;
	int x0 = (SCREEN_W - TB_W) / 2, y0 = (SCREEN_H - TB_H) / 2, i;
	SetRect(&rc, x0, y0, x0 + TB_W, y0 + TB_H);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	GfxFill(x0 + 2, y0 + 2, x0 + TB_W - 2, y0 + 20, CL_TITLE);
	for (i = 0; i < 3; i++) // checkbox frame (sunken white box)
	{
		TbCtlRect(i, &rc);
		SetRect(&rc, rc.left + 2, rc.top + 2, rc.left + 14, rc.top + 14);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_WHITE);
		GfxBevel(&rc, FALSE);
		if (s_abTbDlg[i]) // check mark
		{
			GfxFill(rc.left + 3, rc.top + 6, rc.left + 6, rc.top + 10, CL_TEXT);
			GfxFill(rc.left + 6, rc.top + 3, rc.left + 11, rc.top + 7, CL_TEXT);
		}
	}
	for (i = 3; i < TB_CTL_N; i++)
	{
		TbCtlRect(i, &rc);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, (i == s_nDlgSel) ? CL_SEL : CL_FACE);
		GfxBevel(&rc, TRUE);
	}
}

static void RenderTbText(HDC hdc)
{
	RECT cr;
	static const WCHAR *const aOpts[3] = {L"Always on top", L"Auto hide", L"Show Clock"};
	int x0 = (SCREEN_W - TB_W) / 2, y0 = (SCREEN_H - TB_H) / 2, i;
	GfxText(hdc, x0 + 8, y0 + 4, CL_WHITE, CL_TITLE, g_FontBold, L"Taskbar Properties");
	for (i = 0; i < 3; i++)
	{
		TbCtlRect(i, &cr);
		GfxText(hdc, cr.left + 18, cr.top + 2, CL_TEXT, CL_FACE, g_FontUI, aOpts[i]);
	}
	TbCtlRect(3, &cr);
	GfxText(hdc, cr.left + 16, cr.top + 4, (s_nDlgSel == 3) ? CL_WHITE : CL_TEXT,
	        (s_nDlgSel == 3) ? CL_SEL : CL_FACE, g_FontUI, L"OK");
	TbCtlRect(4, &cr);
	GfxText(hdc, cr.left + 7, cr.top + 4, (s_nDlgSel == 4) ? CL_WHITE : CL_TEXT,
	        (s_nDlgSel == 4) ? CL_SEL : CL_FACE, g_FontUI, L"Cancel");
}

// The taskbar is up unless Auto hide is on and nothing needs it: parked cursor at the
// bottom edge, an open menu, a modal, or a context menu all reveal it (stock behavior).
static int TaskbarUp(void)
{
	if (!s_bTbAutoHide)
		return 1;
	if (s_bMenuOpen || s_bDlgOpen || s_pCtxItems)
		return 1;
	return s_nCy >= SCREEN_H - 3;
}

//
// Right-click context menu
//
static int DeskIconAt(int x, int y) // desktop-icon index under (x,y), or -1
{
	int k;
	for (k = 0; k < s_cDesk; k++)
	{
		int ix = s_aDeskPos[k].x, iy = s_aDeskPos[k].y;
		if (x >= ix && x < ix + ICELL_W && y >= iy && y < iy + ICELL_H)
			return k;
	}
	return -1;
}

static int WindowAt(int x, int y) // topmost open window at (x,y) (focused first), or -1
{
	int k;
	if (!s_pShared)
		return -1;
	if (s_nFocus >= 0 && s_pShared->win[s_nFocus].inUse && !s_abWinMin[s_nFocus])
	{
		DcWindow *w = &s_pShared->win[s_nFocus];
		if (x >= w->x - 2 && x < w->x + w->w + 2 && y >= w->y - (CAPT_H + 2) &&
		    y < w->y + w->h + 2)
			return s_nFocus;
	}
	for (k = 0; k < DCWIN_MAXWIN; k++)
	{
		DcWindow *w = &s_pShared->win[k];
		if (!w->inUse || s_abWinMin[k] || k == s_nFocus)
			continue;
		if (x >= w->x - 2 && x < w->x + w->w + 2 && y >= w->y - (CAPT_H + 2) &&
		    y < w->y + w->h + 2)
			return k;
	}
	return -1;
}

static int PointOverWindow(int x, int y) // 1 if (x,y) is inside any open window (incl. its title)
{
	return WindowAt(x, y) >= 0;
}

static void CloseContextMenu(void)
{
	s_pCtxItems = NULL;
	s_bDirty = 1;
}

// Open the context menu at (x,y): an icon menu if an icon is under the cursor (Delete only for
// user-added shortcuts), else the empty-desktop menu (Arrange / Refresh / Properties).
static void OpenContextMenu(int x, int y)
{
	int k = DeskIconAt(x, y), h;
	CloseStartMenu(); // close the Start menu if it was open
	if (y >= TASK_Y) // stock ceshell MENU/109: right-click on the taskbar itself
	{
		s_nCtxIcon = -1;
		s_pCtxItems = s_aCtxTaskbar;
		s_nCtxCount = sizeof(s_aCtxTaskbar) / sizeof(s_aCtxTaskbar[0]);
	}
	else if (k >= 0)
	{
		s_nDeskSel = k;
		s_nCtxIcon = k;
		if (s_anDeskUser[k] >= 0)
		{
			s_pCtxItems = s_aCtxIconUser;
			s_nCtxCount = sizeof(s_aCtxIconUser) / sizeof(s_aCtxIconUser[0]);
		}
		else
		{
			s_pCtxItems = s_aCtxIconBuiltin;
			s_nCtxCount = sizeof(s_aCtxIconBuiltin) / sizeof(s_aCtxIconBuiltin[0]);
		}
	}
	else
	{
		s_nCtxIcon = -1;
		s_pCtxItems = s_aCtxDesktop;
		s_nCtxCount = sizeof(s_aCtxDesktop) / sizeof(s_aCtxDesktop[0]);
	}
	s_nCtxSel = 0;
	h = s_nCtxCount * CTX_ROW_H + 6;
	if (x > SCREEN_W - CTX_W) // keep the popup fully on-screen
		x = SCREEN_W - CTX_W;
	if (y > TASK_Y - h)
		y = TASK_Y - h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	s_nCtxX = x;
	s_nCtxY = y;
	s_bDirty = 1;
}

static void ContextActivate(void)
{
	int nCmd = (s_pCtxItems && s_nCtxSel >= 0 && s_nCtxSel < s_nCtxCount)
	               ? s_pCtxItems[s_nCtxSel].nCmd
	               : 0;
	int nIcon = s_nCtxIcon;
	CloseContextMenu();
	switch (nCmd)
	{
		case CTXC_OPEN:
			if (nIcon >= 0 && nIcon < s_cDesk)
			{
				if (s_aDesk[nIcon].path)
					LaunchApp(L"dcwexp.exe", s_aDesk[nIcon].path);
				else
					ShellLaunch(s_aDesk[nIcon].exe);
			}
			break;
		case CTXC_DELETE:
			if (nIcon >= 0)
				RemoveShortcut(nIcon); // built-ins are refused inside RemoveShortcut
			s_bDeskDirty = 1;
			break;
		case CTXC_ARRANGE:
			DeskPosInit(); // re-flow built-in icons into the column grid
			s_bDeskDirty = 1;
			break;
		case CTXC_REFRESH:
			s_bDeskDirty = 1;
			break;
		case CTXC_DISPLAY:
			OpenDisplayDialog(); // wallpaper picker (the screensaver tab is a later phase)
			break;
		case CTXC_TASKMGR:
			LaunchApp(L"dcwtask.exe", NULL); // MENU/109 -> Task Manager...
			break;
		case CTXC_TASKPROP:
			OpenTaskbarDialog(); // MENU/109 -> Taskbar Properties
			break;
		case CTXC_PROP:
			// stock behavior: My Computer's Properties opens the System CPL; a file
			// object's opens Explorer on it. (No separate property sheet port yet.)
			if (nIcon >= 0 && nIcon < s_cDesk &&
			    s_aDesk[nIcon].icon == ICON_COMPUTER)
				LaunchApp(L"dcwcpl.exe", NULL);
			else if (nIcon >= 0 && nIcon < s_cDesk && s_aDesk[nIcon].path)
				LaunchApp(L"dcwexp.exe", s_aDesk[nIcon].path);
			break;
	}
	s_bDirty = 1;
}

static void RenderContextFills(void)
{
	RECT rc;
	if (!s_pCtxItems)
		return;
	SetRect(&rc, s_nCtxX, s_nCtxY, s_nCtxX + CTX_W, s_nCtxY + s_nCtxCount * CTX_ROW_H + 6);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	if (s_nCtxSel >= 0 && s_nCtxSel < s_nCtxCount)
		GfxFill(s_nCtxX + 3, s_nCtxY + 3 + s_nCtxSel * CTX_ROW_H, s_nCtxX + CTX_W - 3,
		        s_nCtxY + 3 + (s_nCtxSel + 1) * CTX_ROW_H, CL_SEL);
}

static void RenderContextText(HDC hdc)
{
	int i;
	if (!s_pCtxItems)
		return;
	for (i = 0; i < s_nCtxCount; i++)
	{
		COLORREF fg = (i == s_nCtxSel) ? CL_WHITE : CL_TEXT;
		COLORREF bg = (i == s_nCtxSel) ? CL_SEL : CL_FACE;
		GfxText(hdc, s_nCtxX + 10, s_nCtxY + 4 + i * CTX_ROW_H, fg, bg, g_FontUI,
		        s_pCtxItems[i].pszLabel);
	}
}

//
// Window focus management
//
static int CountWindows(void)
{
	int i, n = 0;
	if (!s_pShared)
		return 0;
	for (i = 0; i < DCWIN_MAXWIN; i++)
		if (s_pShared->win[i].inUse)
			n++;
	return n;
}

// Reap windows whose owner process died without DCWinClose (e.g. a Task Manager
// "end task", or a crash) - otherwise the slot stays inUse and ghosts forever.
// Liveness is an OpenProcess(ownerPid) probe; s_dwReapAccess is the access flag that
// worked on our own (live) pid at startup, or 0 if OpenProcess is unusable here.
static DWORD s_dwReapAccess = 0;

static void ProbeReap(void)
{
	static const DWORD adwTryAccess[] = {0x0400 /*PROCESS_QUERY_INFORMATION*/, PROCESS_ALL_ACCESS,
	                                     0};
	int i;
	s_dwReapAccess = 0;
	__try
	{
		for (i = 0; i < (int)(sizeof(adwTryAccess) / sizeof(adwTryAccess[0])); i++)
		{
			HANDLE h = OpenProcess(adwTryAccess[i], FALSE, GetCurrentProcessId());
			if (h)
			{
				CloseHandle(h);
				s_dwReapAccess = adwTryAccess[i];
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		s_dwReapAccess = 0;
	}
	DbgStr(s_dwReapAccess ? L"DCSHELL: dead-window reaper active\r\n"
	                      : L"DCSHELL: OpenProcess unusable, reaper off\r\n");
}

static void ReapDeadWindows(void)
{
	int i;
	if (!s_pShared || !s_dwReapAccess)
		return;
	for (i = 0; i < DCWIN_MAXWIN; i++)
	{
		HANDLE h;
		if (!s_pShared->win[i].inUse || !s_pShared->win[i].ownerPid)
			continue;
		h = OpenProcess(s_dwReapAccess, FALSE, s_pShared->win[i].ownerPid);
		if (h)
		{
			CloseHandle(h);
			continue;
		} // owner alive
		s_pShared->win[i].inUse = 0; // owner gone -> free the ghost slot
		s_bDirty = 1;
	}
}

// auto-focus newly-appeared windows; drop focus when the focused one closes
static void FixupFocus(void)
{
	int i;
	if (!s_pShared)
		return;
	if (s_nFocus >= 0 && !s_pShared->win[s_nFocus].inUse)
		s_nFocus = -1;
	for (i = 0; i < DCWIN_MAXWIN; i++)
	{
		int bNow = s_pShared->win[i].inUse ? 1 : 0;
		if (bNow != s_abWasInUse[i])
			s_bDirty = 1; // a window opened/closed -> recompose
		if (bNow && !s_abWasInUse[i])
			s_nFocus = i;
		s_abWasInUse[i] = bNow;
	}
}

// cycle focus: current -> next in-use window -> ... -> desktop (-1) -> wrap
static void CycleFocus(void)
{
	int i, nCur = s_nFocus;
	if (!s_pShared)
		return;
	for (i = 0; i < DCWIN_MAXWIN; i++)
	{
		nCur++;
		if (nCur >= DCWIN_MAXWIN)
		{
			s_nFocus = -1;
			return;
		}
		if (s_pShared->win[nCur].inUse)
		{
			s_nFocus = nCur;
			return;
		}
	}
	s_nFocus = -1;
}

//
// Rendering. Each layer: COLORFILL/icon fills (DC unlocked) then a GetDC text pass.
//
static void RenderDesktopFills(void)
{
	int i, x, y;

	GfxFill(0, 0, SCREEN_W, TASK_Y, CL_DESKTOP);
	GfxBlitWallpaper(); // wallpaper quad over the teal fill, under the icons (no-op if unset)
	for (i = 0; i < s_cDesk; i++)
	{
		int nLabelW = GfxTextWidth(g_FontUI, s_aDesk[i].label), nLabelX;
		x = s_aDeskPos[i].x;
		y = s_aDeskPos[i].y;
		nLabelX = x + ICELL_W / 2 - nLabelW / 2; // label centred under the icon
		if (i == s_nDeskSel && !s_bMenuOpen)
			GfxFill(nLabelX - 3, y + 36, nLabelX + nLabelW + 3, y + 52,
			        CL_TITLE);                  // selection box hugs the label
		GfxIconBig(s_aDesk[i].icon, x + 32, y); // 32x32 icon (centred in the 96px cell)
	}
}

static void RenderDesktopText(HDC hdc)
{
	int i, x, y;
	for (i = 0; i < s_cDesk; i++)
	{
		int nLabelW = GfxTextWidth(g_FontUI, s_aDesk[i].label), nLabelX;
		x = s_aDeskPos[i].x;
		y = s_aDeskPos[i].y;
		nLabelX = x + ICELL_W / 2 - nLabelW / 2;
		// Transparent label over the wallpaper: a dark drop shadow for legibility, then white text.
		// The selection highlight (blue box) is drawn separately in RenderDesktopFills.
		GfxText(hdc, nLabelX + 1, y + 38, RGB(0, 0, 0), GFX_TRANSPARENT, g_FontUI,
		        s_aDesk[i].label);
		GfxText(hdc, nLabelX, y + 37, CL_WHITE, GFX_TRANSPARENT, g_FontUI, s_aDesk[i].label);
	}
}

static void RenderTaskbarFills(void)
{
	RECT rc;

	if (!TaskbarUp()) // Auto hide: a 2px sliver stays on the bottom edge (stock look)
	{
		GfxFill(0, SCREEN_H - 2, SCREEN_W, SCREEN_H, CL_FACE);
		return;
	}
	GfxFill(0, TASK_Y, SCREEN_W, SCREEN_H, CL_FACE);
	SetRect(&rc, 0, TASK_Y, SCREEN_W, SCREEN_H);
	GfxBevel(&rc, TRUE);

	// Start button (stock: x2..53, flag icon at +2, "Start" text after it). While the
	// menu is up it draws checked: sunken bevel + a 50% stippled face.
	SetRect(&rc, 2, TASK_Y + 3, 53, SCREEN_H - 3);
	if (s_bMenuOpen)
	{
		int dx, dy;
		for (dy = rc.top; dy < rc.bottom; dy++)
			for (dx = rc.left + ((dy - rc.top) & 1); dx < rc.right; dx += 2)
				GfxFill(dx, dy, dx + 1, dy + 1, RGB(160, 160, 160));
	}
	else
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, s_bMenuOpen ? FALSE : TRUE);
	GfxIcon(ICON_START, 4, TASK_Y + 5); // the CE flag, like the H/PC Start button

	if (s_pShared) // task buttons after Start: icon + text, sunken when focused
	{
		int bx = 56, k;
		for (k = 0; k < DCWIN_MAXWIN; k++)
		{
			int w;
			if (!s_pShared->win[k].inUse)
				continue;
			w = GfxTextWidth(g_FontUI, s_pShared->win[k].title) + 30;
			if (w < 56)
				w = 56;
			if (w > 128)
				w = 128;
			if (bx + w > SCREEN_W - 96) // keep clear of the tray
				break;
			SetRect(&rc, bx, TASK_Y + 3, bx + w, SCREEN_H - 3);
			GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
			GfxBevel(&rc, (k == s_nFocus) ? FALSE : TRUE);
			GfxIcon((int)s_pShared->win[k].icon, bx + 4, TASK_Y + 6);
			bx += w + 2;
		}
	}

	if (s_bTbClock) // Show Clock: sunken tray = AC-plug icon + "h:mm AM/PM"
	{
		SetRect(&rc, SCREEN_W - 94, TASK_Y + 3, SCREEN_W - 26, SCREEN_H - 3);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
		GfxBevel(&rc, FALSE);
		GfxIcon(ICON_POWER, rc.left + 4, TASK_Y + 6); // stock AC-line indicator
	}

	// input-panel button (stock pencil-on-page glyph, far right)
	SetRect(&rc, SCREEN_W - 25, TASK_Y + 3, SCREEN_W - 3, SCREEN_H - 3);
	GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
	GfxBevel(&rc, TRUE);
	GfxIcon(ICON_SIP, rc.left + 3, TASK_Y + 6);

	if (s_bMenuOpen)
	{
		int nStrip = StartMenuStrip();
		int nItemW = StartMenuItemW();
		int nContH = StartMenuContentH();
		int my = TASK_Y - nContH - MENU_PAD * 2; // menu top (2px frame around the items)
		int ix = 2 + MENU_PAD + nStrip, ir = 2 + MENU_PAD + nStrip + nItemW;
		int i, y;
		SetRect(&rc, 2, my, ir + MENU_PAD, TASK_Y);
		GfxFill(rc.left, rc.top, rc.right, rc.bottom, CL_FACE);
		GfxBevel(&rc, TRUE);
		if (nStrip)
			GfxBlitBanner(2 + MENU_PAD, my + MENU_PAD, STRIP_W, nContH); // 1:1, bottom-anchored
		y = my + MENU_PAD;
		for (i = 0; i < (int)START_N; i++)
		{
			int ih = s_start[i].sep ? SEP_H : ROW_H;
			if (s_start[i].sep) // etched groove, like the stock menu's before "Suspend"
			{
				GfxFill(ix, y + 2, ir, y + 3, RGB(128, 128, 128));
				GfxFill(ix, y + 3, ir, y + 4, RGB(255, 255, 255));
			}
			else
			{
				if (i == s_nMenuSel)
					GfxFill(ix, y, ir, y + ih, CL_SEL);
				GfxIcon(s_start[i].icon, ix + 4, y + 3);
				if (s_start[i].sub >= 0) // cascade glyph (the stock menu's Marlett "4")
				{
					int d, ax = ir - 10, ay = y + (ih - 7) / 2;
					COLORREF cl = (i == s_nMenuSel) ? CL_WHITE : CL_TEXT;
					for (d = 0; d < 4; d++) // right-pointing arrowhead: 7/5/3/1px columns
						GfxFill(ax + d, ay + d, ax + d + 1, ay + 7 - d, cl);
				}
			}
			y += ih;
		}
		if (s_nSubRoot >= 0) // open cascade: plain item menu, no brand strip
		{
			RECT rs;
			int n = 0, sx, sr, sy;
			const SHORTCUT *a = SubOpenItems(&n);
			SubRect(&rs);
			GfxFill(rs.left, rs.top, rs.right, rs.bottom, CL_FACE);
			GfxBevel(&rs, TRUE);
			sx = rs.left + MENU_PAD;
			sr = rs.right - MENU_PAD;
			sy = rs.top + MENU_PAD;
			for (i = 0; i < n; i++)
			{
				int ih = a[i].sep ? SEP_H : ROW_H;
				if (a[i].sep)
				{
					GfxFill(sx, sy + 2, sr, sy + 3, RGB(128, 128, 128));
					GfxFill(sx, sy + 3, sr, sy + 4, RGB(255, 255, 255));
				}
				else
				{
					if (i == s_nSubSel)
						GfxFill(sx, sy, sr, sy + ih, CL_SEL);
					if (!a[i].dis)
						GfxIcon(a[i].icon, sx + 4, sy + 3);
					if (a[i].sub >= 0) // nested cascade glyph
					{
						int d, ax = sr - 10, ay = sy + (ih - 7) / 2;
						COLORREF cl = (i == s_nSubSel) ? CL_WHITE : CL_TEXT;
						for (d = 0; d < 4; d++)
							GfxFill(ax + d, ay + d, ax + d + 1, ay + 7 - d, cl);
					}
				}
				sy += ih;
			}
		}
		if (s_nSub2Root >= 0) // open nested cascade
		{
			RECT rs;
			int n = 0, sx, sr, sy;
			const SHORTCUT *a = Sub2OpenItems(&n);
			Sub2Rect(&rs);
			GfxFill(rs.left, rs.top, rs.right, rs.bottom, CL_FACE);
			GfxBevel(&rs, TRUE);
			sx = rs.left + MENU_PAD;
			sr = rs.right - MENU_PAD;
			sy = rs.top + MENU_PAD;
			for (i = 0; i < n; i++)
			{
				int ih = a[i].sep ? SEP_H : ROW_H;
				if (a[i].sep)
				{
					GfxFill(sx, sy + 2, sr, sy + 3, RGB(128, 128, 128));
					GfxFill(sx, sy + 3, sr, sy + 4, RGB(255, 255, 255));
				}
				else
				{
					if (i == s_nSub2Sel)
						GfxFill(sx, sy, sr, sy + ih, CL_SEL);
					if (!a[i].dis)
						GfxIcon(a[i].icon, sx + 4, sy + 3);
				}
				sy += ih;
			}
		}
	}
}

static void RenderBarsText(HDC hdc)
{
	SYSTEMTIME t;
	WCHAR szClk[16];
	int i;

	if (TaskbarUp())
	{
		GfxText(hdc, 24, TASK_Y + 5, CL_TEXT, CL_FACE, g_FontBold, L"Start");
		GetLocalTime(&t);
		{ // the H/PC tray shows 12-hour time: "3:07 PM"
			int h12 = t.wHour % 12;
			if (!h12)
				h12 = 12;
			wsprintfW(szClk, L"%d:%02d %s", h12, t.wMinute, t.wHour < 12 ? L"AM" : L"PM");
		}
		if (s_bTbClock)
			GfxText(hdc, SCREEN_W - 68, TASK_Y + 5, CL_TEXT, CL_FACE, g_FontUI, szClk);
	}

	if (s_pShared && TaskbarUp())
	{
		int bx = 56, k;
		for (k = 0; k < DCWIN_MAXWIN; k++)
		{
			int w;
			if (!s_pShared->win[k].inUse)
				continue;
			w = GfxTextWidth(g_FontUI, s_pShared->win[k].title) + 30;
			if (w < 56)
				w = 56;
			if (w > 128)
				w = 128;
			if (bx + w > SCREEN_W - 96)
				break;
			GfxText(hdc, bx + 22, TASK_Y + 5, CL_TEXT, CL_FACE, g_FontUI, s_pShared->win[k].title);
			bx += w + 2;
		}
	}

	if (s_bMenuOpen)
	{
		int nStrip = StartMenuStrip();
		int my = TASK_Y - StartMenuContentH() - MENU_PAD * 2;
		int ix = 2 + MENU_PAD + nStrip;
		int y = my + MENU_PAD;
		for (i = 0; i < (int)START_N; i++)
		{
			int ih = s_start[i].sep ? SEP_H : ROW_H;
			if (!s_start[i].sep)
			{
				COLORREF fg = (i == s_nMenuSel) ? CL_WHITE : CL_TEXT;
				COLORREF bg = (i == s_nMenuSel) ? CL_SEL : CL_FACE;
				GfxText(hdc, ix + 24, y + (ih - 14) / 2, fg, bg, g_FontUI,
				        s_start[i].label);
			}
			y += ih;
		}
		if (s_nSubRoot >= 0) // open cascade labels
		{
			RECT rs;
			int n = 0;
			const SHORTCUT *a = SubOpenItems(&n);
			SubRect(&rs);
			y = rs.top + MENU_PAD;
			for (i = 0; i < n; i++)
			{
				int ih = a[i].sep ? SEP_H : ROW_H;
				if (!a[i].sep)
				{
					COLORREF fg = a[i].dis ? RGB(128, 128, 128)
					                     : (i == s_nSubSel) ? CL_WHITE : CL_TEXT;
					COLORREF bg = (i == s_nSubSel && !a[i].dis) ? CL_SEL : CL_FACE;
					GfxText(hdc, rs.left + MENU_PAD + 24, y + (ih - 14) / 2, fg, bg,
					        g_FontUI, a[i].label);
				}
				y += ih;
			}
		}
		if (s_nSub2Root >= 0) // nested cascade labels
		{
			RECT rs;
			int n = 0;
			const SHORTCUT *a = Sub2OpenItems(&n);
			Sub2Rect(&rs);
			y = rs.top + MENU_PAD;
			for (i = 0; i < n; i++)
			{
				int ih = a[i].sep ? SEP_H : ROW_H;
				if (!a[i].sep)
				{
					COLORREF fg = a[i].dis ? RGB(128, 128, 128)
					                     : (i == s_nSub2Sel) ? CL_WHITE : CL_TEXT;
					COLORREF bg = (i == s_nSub2Sel && !a[i].dis) ? CL_SEL : CL_FACE;
					GfxText(hdc, rs.left + MENU_PAD + 24, y + (ih - 14) / 2, fg, bg,
					        g_FontUI, a[i].label);
				}
				y += ih;
			}
		}
	}
}

static void SnapWindows(void)
{
	int wi, i, nTries;

	if (!s_pShared)
		return;
	for (wi = 0; wi < DCWIN_MAXWIN; wi++)
	{
		DcWindow *w = &s_pShared->win[wi];
		s_anSnapN[wi] = 0;
		if (!w->inUse)
			continue;
		for (nTries = 0; nTries < 8; nTries++)
		{
			DWORD dwGen1 = w->gen, dwGen2;
			int cCmd;
			if (dwGen1 & 1)
			{
				Sleep(1);
				continue;
			} // client mid-write (Sleep(1):
			//            we run ABOVE_NORMAL, so Sleep(0) would never yield to the
			//            lower-priority client writing the frame -> it could never finish
			cCmd = (int)w->cmdCount;
			if (cCmd > DCWIN_MAXCMD)
				cCmd = DCWIN_MAXCMD;
			for (i = 0; i < cCmd; i++)
				s_aSnap[wi][i] = w->cmd[i];
			dwGen2 = w->gen;
			if (dwGen1 == dwGen2)
			{
				s_anSnapN[wi] = cCmd;
				break;
			} // consistent
		}
	}
}

static void DrawWinFills(DcWindow *w, int wi, BOOL bActive)
{
	RECT rcFrame;
	int i;

	SetRect(&rcFrame, w->x - 2, w->y - (CAPT_H + 2), w->x + w->w + 2,
	        w->y + w->h + 2); // frame
	GfxFill(rcFrame.left, rcFrame.top, rcFrame.right, rcFrame.bottom, CL_FACE);
	GfxBevel(&rcFrame, TRUE);
	GfxFill(w->x - 2, w->y - (CAPT_H + 2), w->x + w->w + 2, w->y - 2,
	        bActive ? CL_TITLE : RGB(112, 112, 112));
	GfxIcon((int)w->icon, w->x + 2, w->y - (CAPT_H + 2) + 3); // title-bar icon
	{ // close box: right-aligned ~14px button inside the caption
		// the stock CE close glyph: a thin 2-stroke x-mark (extracted, not a text 'X')
		static const char *s_aXArt[9] = {"XX.....XX", "XXX...XXX", ".XXX.XXX.", "..XXXXX..",
		                                 "...XXX...", "..XXXXX..", ".XXX.XXX.", "XXX...XXX",
		                                 "XX.....XX"};
		RECT rcClose;
		int gx, gy;
		SetRect(&rcClose, w->x + w->w - 15, w->y - (CAPT_H + 2) + 3, w->x + w->w - 1,
		        w->y - 4);
		GfxFill(rcClose.left, rcClose.top, rcClose.right, rcClose.bottom, CL_FACE);
		GfxBevel(&rcClose, TRUE);
		for (gy = 0; gy < 9; gy++)
			for (gx = 0; gx < 9; gx++)
				if (s_aXArt[gy][gx] == 'X')
					GfxFill(rcClose.left + 3 + gx, rcClose.top + 4 + gy, rcClose.left + 4 + gx,
					        rcClose.top + 5 + gy, CL_TEXT);
	}
	{ // maximize/restore box (left of close) + its glyph (a little window w/ a title bar)
		int nMaxX0 = w->x + w->w - 30;
		RECT rcMax;
		SetRect(&rcMax, nMaxX0, w->y - (CAPT_H + 2) + 3, w->x + w->w - 16, w->y - 4);
		GfxFill(rcMax.left, rcMax.top, rcMax.right, rcMax.bottom, CL_FACE);
		GfxBevel(&rcMax, TRUE);
		GfxFill(nMaxX0 + 3, w->y - (CAPT_H + 2) + 6, nMaxX0 + 11, w->y - 9, CL_TEXT);
		GfxFill(nMaxX0 + 4, w->y - (CAPT_H + 2) + 8, nMaxX0 + 10, w->y - 10,
		        CL_FACE); // interior (leaves a top "title bar")
	}
	{ // minimize box (left of maximize) + its glyph (a low bar)
		int nMinX0 = w->x + w->w - 45;
		RECT rcMin;
		SetRect(&rcMin, nMinX0, w->y - (CAPT_H + 2) + 3, w->x + w->w - 31, w->y - 4);
		GfxFill(rcMin.left, rcMin.top, rcMin.right, rcMin.bottom, CL_FACE);
		GfxBevel(&rcMin, TRUE);
		GfxFill(nMinX0 + 3, w->y - 10, nMinX0 + 11, w->y - 8, CL_TEXT); // the "minimize" bar
	}

	GfxSetClip(w->x, w->y, w->x + w->w, w->y + w->h); // clip content to the client area
	for (i = 0; i < s_anSnapN[wi]; i++)
	{
		DcCmd *c = &s_aSnap[wi][i];
		if (c->op == DCOP_FILL)
			GfxFill(w->x + c->x, w->y + c->y, w->x + c->x + c->w, w->y + c->y + c->h, c->color);
		else if (c->op == DCOP_ICON)
			GfxIcon((int)c->color, w->x + c->x, w->y + c->y);
		else if (c->op == DCOP_ICONBIG)
			GfxIconBig((int)c->color, w->x + c->x, w->y + c->y);
	}
	GfxClearClip();
}

static void DrawWinText(HDC hdc, DcWindow *w, int wi, BOOL bActive)
{
	int i;

	// Clip title-bar text to the bar: a glyph cell is GH(16)px tall but the bar is only
	// 16px, so without this the opaque text bg quad spills ~2px below the bar's bottom edge
	// (the blue "dip" under the caption). The glyph itself fits, only the overhang is cut.
	GfxSetClip(w->x - 2, w->y - (CAPT_H + 2), w->x + w->w + 2, w->y - 2);
	GfxText(hdc, w->x + 20, w->y - CAPT_H - 2 + 4, CL_WHITE,
	        bActive ? CL_TITLE : RGB(112, 112, 112), g_FontBold, w->title);
	GfxClearClip();
	GfxSetClip(w->x, w->y, w->x + w->w, w->y + w->h); // clip content text to the client area
	for (i = 0; i < s_anSnapN[wi]; i++)
	{
		DcCmd *c = &s_aSnap[wi][i];
		if (c->op == DCOP_TEXT)
			GfxText(hdc, w->x + c->x, w->y + c->y, c->color, c->color2, g_FontUI, c->text);
	}
	GfxClearClip();
}

// Composite ONE window completely (fills, then text) so an overlapping window's
// text never lands on a window above it.
static void RenderWindow(int wi, BOOL bActive)
{
	HDC hdc;
	DcWindow *w = &s_pShared->win[wi];

	DrawWinFills(w, wi, bActive);
	hdc = GfxLockDC();
	if (hdc)
	{
		DrawWinText(hdc, w, wi, bActive);
		GfxUnlockDC(hdc);
	}
}

// Repaint the static desktop layer (bg + icons + labels) into the dcgfx desktop
// cache. Called only when its visual inputs (selection, menu-open) change - never
// for clock ticks, window republishes, or focus changes.
static void RebuildDesktopCache(void)
{
	GfxBeginDesktopCache();
	RenderDesktopFills();
	RenderDesktopText((HDC)1); // GfxText ignores the HDC; target is s_lockPix
	GfxEndDesktopCache();
	s_bDeskDirty = 0;
	s_nCachedSel = s_nDeskSel;
	s_nCachedMenu = s_bMenuOpen;
}

// Rebuild the scene: a PVR2 quad list, back-to-front (desktop -> windows -> taskbar)
// = submit order = painter's Z. The desktop layer is a cached vertex sub-list,
// rebuilt only when its inputs (selection / menu) change. GfxPresent consumes it.
static const WCHAR *ExcDesc(DWORD dwCode)
{
	switch (dwCode)
	{
		case 0xC0000005:
			return L"access violation";
		case 0xC000001D:
			return L"illegal instruction";
		case 0x80000002:
			return L"datatype misalignment";
		case 0x80000003:
			return L"breakpoint";
		case 0xC0000094:
			return L"integer divide by zero";
		case 0xC0000096:
			return L"privileged instruction";
		case 0xC00000FD:
			return L"stack overflow";
		default:
			return L"unhandled exception";
	}
}

// Windows-9x-style blue screen: a client process faulted. Drawn instead of the desktop until
// dismissed. Fills must precede GfxLockDC (same rule as every other layer).
#define BSOD_BLUE RGB(0, 0, 168)
static void RenderBsod(void)
{
	HDC hdc;
	WCHAR sz[96];
	int nMidX = SCREEN_W / 2;

	GfxFill(0, 0, SCREEN_W, SCREEN_H, BSOD_BLUE);
	GfxFill(nMidX - 28, 70, nMidX + 28, 88, CL_FACE); // "DCWin" banner box
	hdc = GfxLockDC();
	if (!hdc)
		return;
	GfxText(hdc, nMidX - 19, 72, BSOD_BLUE, CL_FACE, g_FontBold, L"DCWin");
	GfxText(hdc, 60, 124, CL_WHITE, BSOD_BLUE, g_FontUI, L"An error has occurred. To continue:");
	GfxText(hdc, 60, 154, CL_WHITE, BSOD_BLUE, g_FontUI,
	        L"Press any key to return to the desktop, or");
	GfxText(hdc, 60, 172, CL_WHITE, BSOD_BLUE, g_FontUI, L"press CTRL+ALT+DEL to restart.");
	GfxText(hdc, 60, 190, CL_WHITE, BSOD_BLUE, g_FontUI,
	        L"You will lose unsaved information in all open applications.");
	wsprintfW(sz, L"%s  caused an exception  %08X  (%s)",
	          s_crash.proc[0] ? s_crash.proc : L"A program", (unsigned)s_crash.code,
	          ExcDesc(s_crash.code));
	GfxText(hdc, 60, 230, CL_WHITE, BSOD_BLUE, g_FontUI, sz);
	wsprintfW(sz, L"at address  %08X.", (unsigned)s_crash.addr);
	GfxText(hdc, 60, 248, CL_WHITE, BSOD_BLUE, g_FontUI, sz);
	if (s_crash.access != 0xFFFFFFFF)
	{
		wsprintfW(sz, L"The instruction %s memory at  %08X.", s_crash.access ? L"wrote" : L"read",
		          (unsigned)s_crash.badAddr);
		GfxText(hdc, 60, 266, CL_WHITE, BSOD_BLUE, g_FontUI, sz);
	}
	GfxText(hdc, nMidX - 92, 330, CL_WHITE, BSOD_BLUE, g_FontUI, L"Press any key to continue _");
	GfxUnlockDC(hdc);
}

static void Render(void)
{
	HDC hdc;
	int i;

	if (s_bBsod) // a client crashed: blue screen replaces the whole desktop
	{
		RenderBsod();
		return;
	}

	SnapWindows();

	// layer 0: desktop (cached vertex sub-list; rebuild only on layout change)
	if (s_bDeskDirty || s_nDeskSel != s_nCachedSel || s_bMenuOpen != s_nCachedMenu)
		RebuildDesktopCache();
	GfxBlitDesktopCache();

	// layers 1..N: app windows, back-to-front, each composited fully. The quad arrays grow on
	// demand (dcgfx), so a text-heavy window can't starve the taskbar/cursor drawn after it.
	if (s_pShared)
	{
		for (i = 0; i < DCWIN_MAXWIN; i++)
			if (s_pShared->win[i].inUse && !s_abWinMin[i] && i != s_nFocus)
				RenderWindow(i, FALSE);
		if (s_nFocus >= 0 && s_pShared->win[s_nFocus].inUse && !s_abWinMin[s_nFocus])
			RenderWindow(s_nFocus, TRUE);
	}

	// top layer: taskbar + Start menu, then the context menu / modal dialog above everything
	RenderTaskbarFills();
	RenderContextFills();
	RenderDialogFills();
	hdc = GfxLockDC();
	if (hdc)
	{
		RenderBarsText(hdc);
		RenderContextText(hdc);
		RenderDialogText(hdc);
		GfxUnlockDC(hdc);
	}
	// The cursor is NOT in the scene here - GfxPresent appends it as the last quad.
}

//
// Input
//
// Close the Start menu + any open cascade (the two are one popup tree).
static void CloseStartMenu(void)
{
	s_bMenuOpen = 0;
	s_nSubRoot = -1;
	s_nSubSel = -1;
	s_nHoverSub = -1;
	s_nSub2Root = -1;
	s_nSub2Sel = -1;
	s_nHoverSub2 = -1;
}

// Run a shortcut's action (shared by root items, cascade items and desktop icons).
static void ActivateShortcut(const SHORTCUT *pShc)
{
	if (pShc->dis)
		return;
	if (pShc->path)
		LaunchApp(L"dcwexp.exe", pShc->path); // Explorer window at this path
	else if (pShc->exe)
		ShellLaunch(pShc->exe); // dcw* -> composited; others -> display hand-off
	else
		switch (pShc->cmd)
		{
			case SCMD_HELP:
				LaunchApp(L"dcwhelp.exe", NULL); // the peghelp port
				break;
			case SCMD_RUN:
				OpenRunDialog();
				break;
			case SCMD_TASKBAROPT:
				OpenTaskbarDialog();
				break;
			case SCMD_SUSPEND:
				PostQuitMessage(0); // power down -> back to the DCWin loader
				break;
		}
}

static void ActivateSubItem(void)
{
	int n = 0;
	const SHORTCUT *a = SubOpenItems(&n);
	SHORTCUT it;
	if (!a || s_nSubSel < 0 || s_nSubSel >= n || a[s_nSubSel].dis)
		return;
	if (a[s_nSubSel].sub >= 0) // a nested cascade opens level 2, it doesn't launch
	{
		OpenSub2(s_nSubSel);
		return;
	}
	it = a[s_nSubSel];
	CloseStartMenu();
	ActivateShortcut(&it);
}

static void ActivateSub2Item(void)
{
	int n = 0;
	const SHORTCUT *a = Sub2OpenItems(&n);
	SHORTCUT it;
	if (!a || s_nSub2Sel < 0 || s_nSub2Sel >= n || a[s_nSub2Sel].dis)
		return;
	it = a[s_nSub2Sel];
	CloseStartMenu();
	ActivateShortcut(&it);
}

static void ActivateStartItem(void)
{
	const SHORTCUT *pShc = &s_start[s_nMenuSel];
	if (pShc->sub >= 0) // a cascade item opens/closes its submenu; it launches nothing
	{
		if (s_nSubRoot == s_nMenuSel)
			CloseSub();
		else
			OpenSub(s_nMenuSel);
		return;
	}
	CloseStartMenu();
	ActivateShortcut(pShc);
}

static void OnKey(WPARAM wParam)
{
	if (s_bDlgOpen) // the modal dialog captures all keys
	{
		DlgOnKey(wParam);
		return;
	}
	if (s_pCtxItems) // the context menu captures all keys while it is open
	{
		if (wParam == VK_UP && s_nCtxSel > 0)
			s_nCtxSel--;
		else if (wParam == VK_DOWN && s_nCtxSel < s_nCtxCount - 1)
			s_nCtxSel++;
		else if (wParam == VK_RETURN)
		{
			ContextActivate();
			return;
		}
		else if (wParam == VK_ESCAPE)
		{
			CloseContextMenu();
			return;
		}
		s_bDirty = 1;
		return;
	}
	if (wParam == VK_TAB) // task-switch; or Start menu when no windows open
	{
		if (CountWindows() == 0)
		{
			if (s_bMenuOpen)
				CloseStartMenu();
			else
			{
				s_bMenuOpen = 1;
				s_nMenuSel = 0;
			}
		}
		else
			CycleFocus();
		s_bDirty = 1;
		return;
	}

	// A focused window captures input: forward the key to its ring and return
	// WITHOUT marking the scene dirty. The shell must NOT recomposite here - the
	// window content hasn't changed yet; when the client processes the key and
	// republishes (gen bump) the loop's gen-change check triggers exactly one
	// render. (Marking dirty here recomposited stale frames every keypress and
	// kept the shell busy, starving the client -> the in-window nav lag.)
	if (s_nFocus >= 0 && s_pShared && s_pShared->win[s_nFocus].inUse)
	{
		DcWindow *w = &s_pShared->win[s_nFocus];
		if (wParam == VK_ESCAPE)
		{
			w->wantClose = 1;
			return;
		} // FixupFocus redraws on close
		w->in[w->inHead % DCWIN_MAXIN].type = 1;
		w->in[w->inHead % DCWIN_MAXIN].key = (DWORD)wParam;
		w->inHead++;
		return;
	}

	if (s_bMenuOpen)
	{
		if (s_nSubRoot >= 0) // keys drive the open cascade while it is up
		{
			int nSub = 0;
			const SHORTCUT *aSub = SubOpenItems(&nSub);
			if (s_nSub2Root >= 0) // nested cascade up: keys drive it instead
			{
				int nSub2 = 0;
				const SHORTCUT *aSub2 = Sub2OpenItems(&nSub2);
				if (wParam == VK_LEFT || wParam == VK_ESCAPE)
					CloseSub2();
				else if (aSub2 && (wParam == VK_UP || wParam == VK_DOWN))
				{
					int d = (wParam == VK_UP) ? -1 : 1, s = s_nSub2Sel, k;
					for (k = 0; k < nSub2; k++)
					{
						s += d;
						if (s < 0)
							s = nSub2 - 1;
						if (s >= nSub2)
							s = 0;
						if (!aSub2[s].sep && !aSub2[s].dis)
						{
							s_nSub2Sel = s;
							break;
						}
					}
				}
				else if (wParam == VK_RETURN)
					ActivateSub2Item();
				s_bDirty = 1;
				return;
			}
			if (wParam == VK_LEFT || wParam == VK_ESCAPE)
				CloseSub();
			else if (aSub && (wParam == VK_UP || wParam == VK_DOWN))
			{
				// bounded walk: an all-disabled list ("(Empty)") has nowhere to go
				int d = (wParam == VK_UP) ? -1 : 1, s = s_nSubSel, k;
				for (k = 0; k < nSub; k++)
				{
					s += d;
					if (s < 0)
						s = nSub - 1;
					if (s >= nSub)
						s = 0;
					if (!aSub[s].sep && !aSub[s].dis)
					{
						if (s != s_nSubSel && s_nSub2Root >= 0)
							CloseSub2(); // moving off the cascade item drops level 2
						s_nSubSel = s;
						break;
					}
				}
			}
			else if (aSub && wParam == VK_RIGHT && s_nSubSel >= 0 &&
			         s_nSubSel < nSub && aSub[s_nSubSel].sub >= 0)
			{
				int n2 = 0;
				const SHORTCUT *a2 = SubItems(aSub[s_nSubSel].sub, &n2);
				int s;
				OpenSub2(s_nSubSel);
				for (s = 0; s < n2; s++) // land on the first selectable row
					if (!a2[s].sep && !a2[s].dis)
					{
						s_nSub2Sel = s;
						break;
					}
			}
			else if (wParam == VK_RETURN)
				ActivateSubItem();
			s_bDirty = 1;
			return;
		}
		if (wParam == VK_UP && s_nMenuSel > 0)
			do { s_nMenuSel--; } while (s_nMenuSel > 0 && s_start[s_nMenuSel].sep);
		if (wParam == VK_DOWN && s_nMenuSel < (int)START_N - 1)
			do { s_nMenuSel++; } while (s_nMenuSel < (int)START_N - 1 && s_start[s_nMenuSel].sep);
		if (wParam == VK_RIGHT && s_start[s_nMenuSel].sub >= 0)
		{
			int n = 0;
			const SHORTCUT *a = SubItems(s_start[s_nMenuSel].sub, &n);
			int s;
			OpenSub(s_nMenuSel);
			for (s = 0; s < n; s++) // land on the first selectable row
				if (!a[s].sep && !a[s].dis)
				{
					s_nSubSel = s;
					break;
				}
		}
		if (wParam == VK_RETURN)
			ActivateStartItem();
		if (wParam == VK_ESCAPE)
			CloseStartMenu();
		s_bDirty = 1;
		return;
	}

	// desktop - grid navigation (row-major: index = row*cols + col)
	s_bDirty = 1;
	if (s_nDeskSel < 0) // nothing selected (pointer last moved over empty desktop)
	{
		if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT)
			s_nDeskSel = 0; // first D-pad press picks an icon; Enter on empty space does nothing
		return;
	}
	{
		int nCol = s_nDeskSel % s_nDeskCols;
		if (wParam == VK_LEFT && nCol > 0)
			s_nDeskSel--;
		if (wParam == VK_RIGHT && nCol < s_nDeskCols - 1 && s_nDeskSel + 1 < s_cDesk)
			s_nDeskSel++;
		if (wParam == VK_UP && s_nDeskSel - s_nDeskCols >= 0)
			s_nDeskSel -= s_nDeskCols;
		if (wParam == VK_DOWN && s_nDeskSel + s_nDeskCols < s_cDesk)
			s_nDeskSel += s_nDeskCols;
	}
	if (wParam == VK_DELETE)
	{
		RemoveShortcut(s_nDeskSel);
		return;
	} // remove a user-added shortcut
	if (wParam == VK_RETURN)
	{
		if (s_aDesk[s_nDeskSel].path)
			LaunchApp(L"dcwexp.exe", s_aDesk[s_nDeskSel].path); // Explorer window
		else
			ShellLaunch(s_aDesk[s_nDeskSel].exe); // hand off the display for non-dcw apps
	}
}

// Maximize/restore a window (toggle). Full-screen = the desktop area above the taskbar,
// leaving room for the title bar. Saves the pre-maximize geometry to restore.
static void ToggleMaximize(int wi)
{
	DcWindow *w;
	if (!s_pShared || wi < 0 || wi >= DCWIN_MAXWIN || !s_pShared->win[wi].inUse)
		return;
	w = &s_pShared->win[wi];
	if (s_abWinMax[wi])
	{
		w->x = s_awinRestore[wi][0];
		w->y = s_awinRestore[wi][1];
		w->w = s_awinRestore[wi][2];
		w->h = s_awinRestore[wi][3];
		s_abWinMax[wi] = 0;
	}
	else
	{
		s_awinRestore[wi][0] = w->x;
		s_awinRestore[wi][1] = w->y;
		s_awinRestore[wi][2] = w->w;
		s_awinRestore[wi][3] = w->h;
		w->x = 2;
		w->y = CAPT_H + 4; // caption sits at y=2..24
		w->w = SCREEN_W - 4;
		w->h = TASK_Y - (CAPT_H + 6); // fill down to the taskbar
		s_abWinMax[wi] = 1;
	}
	s_bDirty = 1;
}

// Cursor hit-test (top-down): Start menu -> Start button -> taskbar buttons ->
// windows (close box / maximize box / body) -> desktop icons. Returns 1 if the click was
// consumed by a target (close box, taskbar/start, desktop icon), 0 if it landed
// on a window body (focus it, but let an Enter through to the window) or empty
// space. The controller's "A" uses the return: consumed -> done; else -> Enter.
static int HandleClick(int x, int y)
{
	int k;
	s_bDirty = 1;

	if (s_bDlgOpen) // the modal dialog swallows all clicks
	{
		DlgClick(x, y);
		return 1;
	}
	if (s_pCtxItems) // context menu open: a click on a row runs it, a click elsewhere dismisses
	{
		if (x >= s_nCtxX && x < s_nCtxX + CTX_W && y >= s_nCtxY + 3 &&
		    y < s_nCtxY + 3 + s_nCtxCount * CTX_ROW_H)
		{
			s_nCtxSel = (y - (s_nCtxY + 3)) / CTX_ROW_H;
			ContextActivate();
		}
		else
			CloseContextMenu();
		return 1;
	}
	if (s_bMenuOpen)
	{
		int nStrip = StartMenuStrip(), nItemW = StartMenuItemW();
		int nContH = StartMenuContentH(), my = TASK_Y - nContH - MENU_PAD * 2;
		int ix = 2 + MENU_PAD + nStrip, ir = ix + nItemW, cy0 = my + MENU_PAD;
		int nRow;
		if (s_nSubRoot >= 0) // the open cascade is topmost: hit-test it first
		{
			RECT rs;
			int n = 0;
			const SHORTCUT *a = SubOpenItems(&n);
			if (s_nSub2Root >= 0) // nested cascade sits above its parent
			{
				RECT rs2;
				int n2 = 0;
				const SHORTCUT *a2 = Sub2OpenItems(&n2);
				Sub2Rect(&rs2);
				if (x >= rs2.left && x < rs2.right && y >= rs2.top + MENU_PAD &&
				    y < rs2.bottom - MENU_PAD)
				{
					nRow = MenuItemAtY(a2, n2, y - (rs2.top + MENU_PAD));
					if (nRow >= 0 && !a2[nRow].dis)
					{
						s_nSub2Sel = nRow;
						ActivateSub2Item();
					}
					return 1;
				}
			}
			SubRect(&rs);
			if (x >= rs.left && x < rs.right && y >= rs.top + MENU_PAD &&
			    y < rs.bottom - MENU_PAD)
			{
				nRow = MenuItemAtY(a, n, y - (rs.top + MENU_PAD));
				if (nRow >= 0 && !a[nRow].dis)
				{
					s_nSubSel = nRow;
					ActivateSubItem(); // a .sub item here opens level 2, it doesn't launch
				}
				return 1; // clicks inside the submenu don't dismiss the menu tree
			}
		}
		// the brand strip itself isn't clickable - a click there dismisses, like stock CE
		if (x >= ix && x < ir && y >= cy0 && y < cy0 + nContH)
		{
			nRow = StartItemAtY(y - cy0);
			if (nRow >= 0)
			{
				s_nMenuSel = nRow;
				ActivateStartItem(); // cascade items open their submenu here
			}
			return 1; // a click inside the menu (separator/strip zone) doesn't dismiss
		}
		CloseStartMenu(); // click elsewhere closes the menu
		return 1;
	}
	if (y >= TASK_Y && TaskbarUp() && x >= 2 && x < 53) // Start button
	{
		if (s_bMenuOpen)
			CloseStartMenu();
		else
		{
			s_bMenuOpen = 1;
			s_nMenuSel = 0;
		}
		return 1;
	}
	if (y >= TASK_Y && TaskbarUp() && s_pShared) // taskbar window buttons
	{
		int bx = 56;
		for (k = 0; k < DCWIN_MAXWIN; k++)
		{
			int w;
			if (!s_pShared->win[k].inUse)
				continue;
			w = GfxTextWidth(g_FontUI, s_pShared->win[k].title) + 30;
			if (w < 56)
				w = 56;
			if (w > 128)
				w = 128;
			if (bx + w > SCREEN_W - 96)
				break;
			if (x >= bx && x < bx + w) // restore a minimized window + focus it
			{
				if (s_abWinMin[k])
					s_abWinMin[k] = 0;
				s_nFocus = k;
				s_bDirty = 1;
				return 1;
			}
			bx += w + 2;
		}
	}
	if (s_pShared) // windows: topmost (focused) first
	{
		int anOrder[DCWIN_MAXWIN + 1], n = 0, j;
		if (s_nFocus >= 0 && s_pShared->win[s_nFocus].inUse)
			anOrder[n++] = s_nFocus;
		for (k = 0; k < DCWIN_MAXWIN; k++)
			if (s_pShared->win[k].inUse && k != s_nFocus)
				anOrder[n++] = k;
		for (j = 0; j < n; j++)
		{
			DcWindow *w = &s_pShared->win[anOrder[j]];
			if (s_abWinMin[anOrder[j]])
				continue; // minimized: not on the desktop
			if (x >= w->x - 2 && x < w->x + w->w + 2 && y >= w->y - (CAPT_H + 2) &&
			    y < w->y + w->h + 2)
			{
				if (x >= w->x + w->w - 15 && x <= w->x + w->w - 1 &&
				    y >= w->y - (CAPT_H + 2) + 3 && y <= w->y - 4)
				{
					w->wantClose = 1;
					return 1;
				} // close box -> consumed
				if (x >= w->x + w->w - 30 && x <= w->x + w->w - 16 &&
				    y >= w->y - (CAPT_H + 2) + 3 && y <= w->y - 4)
				{
					ToggleMaximize(anOrder[j]);
					return 1;
				} // maximize/restore box
				if (x >= w->x + w->w - 45 && x <= w->x + w->w - 31 &&
				    y >= w->y - (CAPT_H + 2) + 3 && y <= w->y - 4)
				{
					s_abWinMin[anOrder[j]] = 1;
					if (s_nFocus == anOrder[j])
						s_nFocus = -1;
					s_bDirty = 1;
					return 1;
				} // minimize
				s_nFocus = anOrder[j]; // body -> focus it, fall through to Enter
				return 0;
			}
		}
	}
	for (k = 0; k < s_cDesk; k++) // desktop icons (mutable cell positions)
	{
		int nIconX = s_aDeskPos[k].x, nIconY = s_aDeskPos[k].y;
		if (x >= nIconX && x < nIconX + ICELL_W && y >= nIconY && y < nIconY + ICELL_H)
		{
			s_nDeskSel = k;
			if (s_aDesk[k].path)
				LaunchApp(L"dcwexp.exe", s_aDesk[k].path);
			else
				ShellLaunch(s_aDesk[k].exe);
			return 1;
		}
	}
	return 0; // empty space
}

// ---- pointer drag: move/resize windows, move desktop shortcuts --------------------
// Hit-test at a pointer-DOWN to decide what (if anything) a drag would grab.
static void DragHitTest(int x, int y)
{
	int j, k;
	s_nDragKind = DRAG_NONE;
	s_nDragTarget = -1;

	if (s_bMenuOpen) // grabbing a Start-menu row -> drag-to-desktop
	{
		int nStrip = StartMenuStrip(), nItemW = StartMenuItemW(), nRow;
		int nContH = StartMenuContentH(), my = TASK_Y - nContH - MENU_PAD * 2;
		int ix = 2 + MENU_PAD + nStrip, ir = ix + nItemW, cy0 = my + MENU_PAD;
		if (x >= ix && x < ir && y >= cy0 && y < cy0 + nContH)
		{
			nRow = StartItemAtY(y - cy0);
			if (nRow >= 0 && (s_start[nRow].exe || s_start[nRow].path))
			{
				s_nDragKind = DRAG_STARTITEM;
				s_nDragTarget = nRow;
			}
		}
		return; // menu is modal: never grab windows/icons under it
	}
	if (s_pShared && !s_bMenuOpen) // windows, top-most (focused) first
	{
		int anOrder[DCWIN_MAXWIN + 1], n = 0;
		if (s_nFocus >= 0 && s_pShared->win[s_nFocus].inUse)
			anOrder[n++] = s_nFocus;
		for (k = 0; k < DCWIN_MAXWIN; k++)
			if (s_pShared->win[k].inUse && k != s_nFocus)
				anOrder[n++] = k;
		for (j = 0; j < n; j++)
		{
			DcWindow *w = &s_pShared->win[anOrder[j]];
			if (s_abWinMin[anOrder[j]])
				continue; // minimized windows aren't on the desktop
			// bottom-right corner (12x12) -> resize
			if (x >= w->x + w->w - 12 && x <= w->x + w->w + 2 && y >= w->y + w->h - 12 &&
			    y <= w->y + w->h + 2)
			{
				s_nDragKind = DRAG_WSIZE;
				s_nDragTarget = anOrder[j];
				s_nDragOffX = x - (w->x + w->w);
				s_nDragOffY = y - (w->y + w->h);
				s_nFocus = anOrder[j];
				return;
			}
			// title bar (excluding the min/max/close boxes) -> move
			if (x >= w->x - 2 && x < w->x + w->w - 45 && y >= w->y - (CAPT_H + 2) &&
			    y < w->y - 2)
			{
				s_nDragKind = DRAG_WMOVE;
				s_nDragTarget = anOrder[j];
				s_nDragOffX = x - w->x;
				s_nDragOffY = y - w->y;
				s_nFocus = anOrder[j];
				return;
			}
		}
	}
	for (k = 0; k < s_cDesk; k++) // desktop icon -> move
	{
		int nIconX = s_aDeskPos[k].x, nIconY = s_aDeskPos[k].y;
		if (x >= nIconX && x < nIconX + ICELL_W && y >= nIconY && y < nIconY + ICELL_H)
		{
			s_nDragKind = DRAG_ICON;
			s_nDragTarget = k;
			s_nDragOffX = x - nIconX;
			s_nDragOffY = y - nIconY;
			s_nDeskSel = k;
			return;
		}
	}
}

// Drop a desktop icon to (x,y): clamp into the desktop and persist if it's a user shortcut.
static void DropIcon(int nTarget, int x, int y)
{
	int nNewX = x - s_nDragOffX, nNewY = y - s_nDragOffY;
	if (nNewX < 0)
		nNewX = 0;
	if (nNewX > SCREEN_W - ICELL_W)
		nNewX = SCREEN_W - ICELL_W;
	if (nNewY < 0)
		nNewY = 0;
	if (nNewY > TASK_Y - ICELL_H)
		nNewY = TASK_Y - ICELL_H;
	s_aDeskPos[nTarget].x = nNewX;
	s_aDeskPos[nTarget].y = nNewY;
	s_bDeskDirty = 1; // icons live in the cached desktop layer
	if (nTarget < s_cDesk && s_anDeskUser[nTarget] >= 0)
		SaveShortcuts();
}

// Apply a drag while the pointer is held + has moved past the threshold.
static void DragApply(int x, int y)
{
	if (s_nDragKind == DRAG_ICON)
	{
		// Windows-style: the original icon stays put; a translucent ghost (drawn by GfxPresent
		// at the cursor) tracks the drag, and the icon commits to the new spot on drop.
	}
	else if (s_pShared && s_nDragTarget >= 0 && s_pShared->win[s_nDragTarget].inUse)
	{
		DcWindow *w = &s_pShared->win[s_nDragTarget];
		if (s_nDragKind == DRAG_WMOVE)
		{
			int nNewX = x - s_nDragOffX, nNewY = y - s_nDragOffY;
			if (nNewY < 18)
				nNewY = 18; // keep the title bar on-screen
			if (nNewX < -(w->w - 40))
				nNewX = -(w->w - 40);
			if (nNewX > SCREEN_W - 40)
				nNewX = SCREEN_W - 40;
			if (nNewY > TASK_Y - 1)
				nNewY = TASK_Y - 1;
			w->x = nNewX;
			w->y = nNewY;
		}
		else if (s_nDragKind == DRAG_WSIZE)
		{
			int nNewW = x - s_nDragOffX - w->x, nNewH = y - s_nDragOffY - w->y;
			if (nNewW < WIN_MINW)
				nNewW = WIN_MINW;
			if (nNewW > SCREEN_W)
				nNewW = SCREEN_W;
			if (nNewH < WIN_MINH)
				nNewH = WIN_MINH;
			if (nNewH > SCREEN_H)
				nNewH = SCREEN_H;
			w->w = nNewW;
			w->h = nNewH;
		}
	}
	s_bDirty = 1;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		case WM_ERASEBKGND:
			return 1; // we own the surface
		case WM_PAINT:
			ValidateRect(hWnd, NULL); // continuous present shows the frame; don't storm
			return 0;
		case WM_KEYDOWN:
			if (!s_bDiKbd)
				OnKey(wParam); // fallback when DI keyboard not available
			return 0;
		case WM_MOUSEMOVE: // GWES mouse fallback (if DC mouse isn't on DI)
			DInSetCursor((short)LOWORD(lParam), (short)HIWORD(lParam));
			if (!s_bWmMouseSeen)
			{
				s_bWmMouseSeen = 1;
				DbgStr(L"DCSHELL: WM mouse active\r\n");
			}
			return 0;
		case WM_LBUTTONDOWN:
			DInSetCursor((short)LOWORD(lParam), (short)HIWORD(lParam));
			DInPostClick();
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Forward the analog-stick cursor to the window directly under it (client-relative), so client
// apps can hit-test their own controls (buttons, sliders, seek bars). Topmost-under-cursor wins;
// every other window gets ptrX = -1 (not over). Level-based - clients edge-detect btn for clicks.
static void PublishPointer(int nCursorX, int nCursorY, int bDown)
{
	int anOrder[DCWIN_MAXWIN], n = 0, nTop = -1, i, k;
	if (!s_pShared)
		return;
	if (s_nFocus >= 0 && s_pShared->win[s_nFocus].inUse && !s_abWinMin[s_nFocus])
		anOrder[n++] = s_nFocus;
	for (k = 0; k < DCWIN_MAXWIN; k++)
		if (s_pShared->win[k].inUse && !s_abWinMin[k] && k != s_nFocus)
			anOrder[n++] = k;
	for (i = 0; i < n; i++)
	{
		DcWindow *w = &s_pShared->win[anOrder[i]];
		if (nCursorX >= w->x && nCursorX < w->x + w->w && nCursorY >= w->y &&
		    nCursorY < w->y + w->h)
		{
			nTop = anOrder[i];
			break;
		}
	}
	for (k = 0; k < DCWIN_MAXWIN; k++)
	{
		DcWindow *w = &s_pShared->win[k];
		if (!w->inUse)
			continue;
		if ((int)k == nTop)
		{
			w->ptrX = nCursorX - w->x;
			w->ptrY = nCursorY - w->y;
			w->ptrBtn = (DWORD)(bDown ? 1 : 0);
		}
		else
			w->ptrX = -1;
	}
}

// Choose the desktop wallpaper at boot: the user's saved choice from the VMU if present, else
// the built-in Windows CE brand bitmap (centred) - the stock H/PC look. "(None)" stays
// selectable from Display Properties for a clean teal desktop.
static void ApplyDisplayConfig(void)
{
	DispCfg c;
	int nGot = 0, i;
	ScanWallpapers();
	s_nCurWall = (s_cWall > 1) ? 1 : 0; // default: wince.bmp centred, unless it isn't in the image
	s_nCurStyle = GFXWALL_CENTER;
	if (VmuLoadAs(DISP_VMUFILE, &c, sizeof(c), &nGot) && nGot >= (int)sizeof(c) &&
	    c.dwMagic == DISP_MAGIC)
	{
		s_nCurStyle = (int)c.dwStyle;
		s_nCurWall = 0; // saved "(None)" unless a name matches
		if (c.szWall[0])
			for (i = 1; i < s_cWall; i++)
				if (lstrcmpiW(s_aWall[i], c.szWall) == 0)
				{
					s_nCurWall = i;
					break;
				}
	}
	ApplyWall(s_nCurWall, s_nCurStyle);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpszCmd, int nShow)
{
	WNDCLASSW wc;
	MSG msg;
	DWORD dwNext, dwNextPresent = 0, dwFrameStart;
	int i, bMoved;

	DbgStr(L"DCSHELL: WinMain enter (hybrid desktop)\r\n");

	InitShared();

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hbrBackground = NULL;
	wc.lpszClassName = L"DCSHELL";
	RegisterClassW(&wc);
	s_hwnd = CreateWindowExW(0, L"DCSHELL", L"DCShell", WS_VISIBLE, 0, 0, SCREEN_W, SCREEN_H, NULL,
	                         NULL, hInst, NULL);

	if (!GfxInit(s_hwnd))
	{
		DbgStr(L"DCSHELL: GfxInit failed\r\n");
		return 1;
	}
	DbgStr(L"DCSHELL: desktop up\r\n");
	DeskInit();           // seed shortcuts (built-ins + VMU-persisted) + lay out
	ApplyDisplayConfig(); // load the desktop wallpaper (if any) from the disc
	// the stock Start-menu brand strip (extracted from the H/PC image; ignored if absent)
	GfxSetBanner(L"\\Windows\\startban.bmp");

	// Run a notch above client apps. We are the window server: input polling and the
	// cursor present must preempt a CPU-bound or fast-republishing client, or the
	// pointer stalls for a whole scheduler quantum while that client holds the SH-4.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	s_bDiKbd = DInInit(s_hwnd); // polled keyboard (low-latency) + controller pointer
	DbgStr(s_bDiKbd ? L"DCSHELL: DI keyboard active\r\n"
	                : L"DCSHELL: DI keyboard absent, WM fallback\r\n");
	ProbeReap(); // is OpenProcess usable for dead-window reaping?

	dwNext = GetTickCount() + 1000;
	for (;;)
	{
		dwFrameStart = GetTickCount(); // frame start (for the limiter + input-poll cost)
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				DInShutdown();
				GfxShutdown();
				return 0;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		DInUpdate(); // poll DI devices once per frame
		// Crash watch: a client filled DcCrash + bumped seq -> raise a Win9x blue screen. The
		// desktop freezes until any key/button dismisses it (the crashed app is reaped after).
		if (s_pShared && s_pShared->crash.seq != s_dwCrashSeq) // a dcwlib client faulted
		{
			s_dwCrashSeq = s_pShared->crash.seq;
			s_crash = s_pShared->crash;
			RaiseBsod();
		}
		if (s_bBsod)
		{
			DWORD dwVk;
			int bAny = 0;
			while (DInNextKey(&dwVk))
				bAny = 1;
			// Require the screen to have been up briefly, so a held button / the launch press
			// doesn't dismiss it instantly.
			if (GetTickCount() - s_dwBsodAt > 400 &&
			    (bAny || DInTookActivate() || DInTookContext() || DInTookClick()))
			{
				s_bBsod = 0;
				s_bDirty = 1;
			}
			bMoved = 0;
		}
		else
		{
			{
				DWORD dwVk;
				while (DInNextKey(&dwVk))
					OnKey(dwVk);
			}

			// Global hotkeys for windowed apps (the shell loop is running, so we read input
			// directly). Edge-detected: fire once per press, not every frame held. ALT+F4 or the
			// controller's START+A close the focused window (same path as ESC / the X button);
			// CTRL+ALT+DEL opens the task manager. (Fullscreen apps are handled inside GfxLaunch.)
			{
				static int s_nLastHk = DIN_HK_NONE;
				int nHk = DInHotkey();
				if (nHk == DIN_HK_NONE && DInPadCombo())
					nHk = DIN_HK_ALTF4; // START+A closes the focused window too
				if (nHk != DIN_HK_NONE && nHk != s_nLastHk)
				{
					if (nHk == DIN_HK_ALTF4)
					{
						if (s_nFocus >= 0 && s_pShared && s_pShared->win[s_nFocus].inUse)
						{
							s_pShared->win[s_nFocus].wantClose = 1;
							s_bDirty = 1;
						}
					}
					else if (nHk == DIN_HK_CTRLALTDEL)
						LaunchApp(L"dcwtask.exe", NULL);
				}
				s_nLastHk = nHk;
			}

			bMoved = 0;
			if (DInHasPointer())
			{
				int nOldX = s_nCx, nOldY = s_nCy;
				DInCursor(&s_nCx, &s_nCy);
				if (s_nCx != nOldX || s_nCy != nOldY)
					bMoved = 1; // cursor moved -> needs a present
				// Hover follows the pointer ONLY while it's actually moving, so a parked cursor
				// doesn't fight the D-pad (the analog stick is also "the pointer"). Moving over the
				// bare desktop CLEARS the selection (-1) so A on empty space can't re-launch the
				// last-hovered icon.
				if (bMoved && s_pCtxItems) // open context menu: highlight the row under the cursor
				{
					if (s_nCx >= s_nCtxX && s_nCx < s_nCtxX + CTX_W && s_nCy >= s_nCtxY + 3 &&
					    s_nCy < s_nCtxY + 3 + s_nCtxCount * CTX_ROW_H)
					{
						int nRow = (s_nCy - (s_nCtxY + 3)) / CTX_ROW_H;
						if (nRow >= 0 && nRow < s_nCtxCount && nRow != s_nCtxSel)
						{
							s_nCtxSel = nRow;
							s_bDirty = 1;
						}
					}
				}
				else if (bMoved &&
				         s_bMenuOpen) // Start menu open: highlight the item under the cursor
				{
					int nStrip = StartMenuStrip(), nItemW = StartMenuItemW();
					int nContH = StartMenuContentH(), my = TASK_Y - nContH - MENU_PAD * 2;
					int ix = 2 + MENU_PAD + nStrip, ir = ix + nItemW, cy0 = my + MENU_PAD;
					int bInSub = 0;
					if (s_nSubRoot >= 0)
					{
						RECT rs;
						int n = 0, nRow;
						const SHORTCUT *a = SubOpenItems(&n);
						if (s_nSub2Root >= 0) // nested popup hit-test first
						{
							RECT rs2;
							int n2 = 0, nRow2;
							const SHORTCUT *a2 = Sub2OpenItems(&n2);
							Sub2Rect(&rs2);
							if (s_nCx >= rs2.left && s_nCx < rs2.right &&
							    s_nCy >= rs2.top + MENU_PAD && s_nCy < rs2.bottom - MENU_PAD)
							{
								bInSub = 1;
								nRow2 = MenuItemAtY(a2, n2, s_nCy - (rs2.top + MENU_PAD));
								if (nRow2 >= 0 && !a2[nRow2].dis && nRow2 != s_nSub2Sel)
								{
									s_nSub2Sel = nRow2;
									s_bDirty = 1;
								}
							}
						}
						SubRect(&rs);
						if (!bInSub && s_nCx >= rs.left && s_nCx < rs.right &&
						    s_nCy >= rs.top + MENU_PAD && s_nCy < rs.bottom - MENU_PAD)
						{
							bInSub = 1;
							nRow = MenuItemAtY(a, n, s_nCy - (rs.top + MENU_PAD));
							if (nRow >= 0 && !a[nRow].dis && nRow != s_nSubSel)
							{
								s_nSubSel = nRow;
								s_bDirty = 1;
								// parked on a nested-cascade item arms level 2; moving to
								// a plain item drops it, like stock
								if (a[nRow].sub >= 0)
								{
									if (s_nHoverSub2 != nRow)
									{
										s_nHoverSub2 = nRow;
										s_dwHoverAt2 = GetTickCount();
									}
								}
								else
								{
									s_nHoverSub2 = -1;
									if (s_nSub2Root >= 0)
										CloseSub2();
								}
							}
						}
					}
					if (!bInSub && s_nCx >= ix && s_nCx < ir &&
					    s_nCy >= cy0 && s_nCy < cy0 + nContH)
					{
						int nRow = StartItemAtY(s_nCy - cy0);
						if (nRow >= 0 && nRow != s_nMenuSel)
						{
							s_nMenuSel = nRow;
							s_bDirty = 1;
						}
						// parked on a cascade item -> arm the stock ~400ms auto-open; moving
						// to any other row closes the open submenu immediately.
						if (nRow >= 0 && s_start[nRow].sub >= 0)
						{
							if (s_nHoverSub != nRow)
							{
								s_nHoverSub = nRow;
								s_dwHoverAt = GetTickCount();
							}
						}
						else
						{
							s_nHoverSub = -1;
							if (s_nSubRoot >= 0 && nRow != s_nSubRoot)
								CloseSub();
						}
					}
					else if (!bInSub)
						s_nHoverSub = -1;
				}
				else if (bMoved && !s_pCtxItems && !s_bDlgOpen && !s_bMenuOpen &&
				         !PointOverWindow(s_nCx, s_nCy))
				{
					int nHover = DeskIconAt(s_nCx, s_nCy); // -1 when over empty desktop
					if (nHover != s_nDeskSel)
					{
						s_nDeskSel = nHover;
						s_bDirty = 1;
					}
				}
			}
			// Cascade auto-open: a cursor parked on a Programs/Favorites/Documents/Settings
			// row opens its submenu after the stock ~400ms dwell (clicks + RIGHT-arrow
			// open it instantly; this only covers the hover path).
			if (s_bMenuOpen && s_nHoverSub >= 0 && s_nHoverSub != s_nSubRoot &&
			    GetTickCount() - s_dwHoverAt > 400)
				OpenSub(s_nHoverSub);
			// same dwell for the nested cascade (Communication inside Programs)
			if (s_bMenuOpen && s_nSubRoot >= 0 && s_nHoverSub2 >= 0 &&
			    s_nHoverSub2 != s_nSub2Root && GetTickCount() - s_dwHoverAt2 > 400)
				OpenSub2(s_nHoverSub2);
			// Pointer press / drag / release. A press that doesn't drag is a CLICK on release
			// (preserving the old click + controller-activate behaviour); a press that grabs a
			// title bar / window corner / desktop icon and then moves becomes a drag.
			{
				int bPtr = DInPointerDown();
				if (bPtr && !s_bPtrWas) // DOWN: arm a possible drag
				{
					s_nDownX = s_nCx;
					s_nDownY = s_nCy;
					s_bDragMoved = 0;
					DragHitTest(s_nCx, s_nCy);
					s_bDirty = 1;
				}
				else if (bPtr && s_bPtrWas) // HELD: drag once past the threshold
				{
					if (!s_bDragMoved && (abs(s_nCx - s_nDownX) > DRAG_THRESH ||
					                      abs(s_nCy - s_nDownY) > DRAG_THRESH))
						s_bDragMoved = 1;
					if (s_bDragMoved && s_nDragKind != DRAG_NONE)
						DragApply(s_nCx, s_nCy);
				}
				else if (!bPtr && s_bPtrWas) // UP: drop, or click if it never moved
				{
					if (!s_bDragMoved)
					{
						if (!HandleClick(s_nCx, s_nCy))
							OnKey(VK_RETURN);
					}
					else if (s_nDragKind == DRAG_STARTITEM && s_nDragTarget >= 0)
					{
						int nTop = TASK_Y - StartMenuContentH() - MENU_PAD * 2;
						int nRight = 2 + MENU_PAD * 2 + StartMenuStrip() + StartMenuItemW();
						if (s_nCy < TASK_Y &&
						    (s_nCy < nTop || s_nCx >= nRight)) // dropped on the desktop
							AddDesktopShortcut(s_nDragTarget, s_nCx, s_nCy);
						CloseStartMenu();
					}
					else if (s_nDragKind == DRAG_ICON && s_nDragTarget >= 0 &&
					         s_nDragTarget < s_cDesk)
						DropIcon(s_nDragTarget, s_nCx, s_nCy); // commit the icon to the drop point
					s_nDragKind = DRAG_NONE;
					s_nDragTarget = -1;
					s_bDragMoved = 0;
					s_bDirty = 1;
				}
				s_bPtrWas = bPtr;
				PublishPointer(s_nCx, s_nCy, bPtr); // deliver the cursor to the window under it
			}
			// Right-click (LT+A on the pad, or the DC mouse's right button): over an app window,
			// forward a context request (VK_APPS) to it so the app shows its own menu; over the
			// bare desktop, open the shell's context menu.
			if (DInTookContext() && !s_bDlgOpen)
			{
				int kWin = WindowAt(s_nCx, s_nCy);
				if (kWin >= 0 && s_pShared)
				{
					DcWindow *w = &s_pShared->win[kWin];
					s_nFocus = kWin;
					w->in[w->inHead % DCWIN_MAXIN].type = 1;
					w->in[w->inHead % DCWIN_MAXIN].key = VK_APPS;
					w->inHead++;
					s_bDirty = 1;
				}
				else
					OpenContextMenu(s_nCx, s_nCy);
			}
			// GWES WM-tap click path (DInPostClick) - independent of the held-button drag model.
			if (DInTookClick())
				HandleClick(s_nCx, s_nCy);

			FixupFocus(); // auto-focus new windows, drop closed ones
			if (s_pShared && s_pShared->execSeq != s_dwLastExec) // a window asked to launch an app
			{
				s_dwLastExec = s_pShared->execSeq;
				ShellLaunch(s_pShared->execPath);
			}
			if (GetTickCount() >= dwNext) // clock tick -> repaint + reap dead windows
			{
				s_bDirty = 1;
				dwNext += 1000;
				ReapDeadWindows(); // free slots whose owner process is gone
			}
			// re-render only when a window published a new frame (digit typed, clock tick)
			if (s_pShared)
				for (i = 0; i < DCWIN_MAXWIN; i++)
				{
					DWORD dwGen = s_pShared->win[i].inUse ? s_pShared->win[i].gen : 0;
					if (dwGen != s_adwLastGen[i])
					{
						s_bDirty = 1;
						s_adwLastGen[i] = dwGen;
					}
					if (!s_pShared->win[i].inUse)
					{
						s_abWinMax[i] = 0;
						s_abWinMin[i] = 0;
					} // freed slot -> normal
				}
			// Drag ghost: a translucent icon trails the cursor while a drag is in flight.
			if (s_bDragMoved && s_nDragKind == DRAG_ICON && s_nDragTarget >= 0 &&
			    s_nDragTarget < s_cDesk)
				GfxSetDragGhost(s_aDesk[s_nDragTarget].icon);
			else if (s_bDragMoved && s_nDragKind == DRAG_STARTITEM && s_nDragTarget >= 0 &&
			         s_nDragTarget < (int)START_N)
				GfxSetDragGhost(s_start[s_nDragTarget].icon);
			else
				GfxSetDragGhost(-1);
		} // end of the normal (!s_bBsod) input/window processing
		// The scene is a PVR2 quad list that GfxPresent consumes + clears, so we MUST
		// Render() (rebuild quads) before every present - a bare present would submit
		// an empty scene. Rebuild is cheap (no rasterization), so recompose+present on
		// any change, cursor move, or keepalive.
		if (s_bDirty || bMoved || GetTickCount() >= dwNextPresent)
		{
			Render();
			s_bDirty = 0;
			if (GfxPresent(s_nCx, s_nCy, s_bBsod ? FALSE : DInHasPointer()))
				s_bDirty = 1; // surface lost -> rebuild next loop
			dwNextPresent = GetTickCount() + 100;
		}
		// Pace to the PVR vblank (~60 Hz) instead of Sleep, which rounds up to the
		// ~25 ms CE tick + a 25 ms guard (= the 20 fps cap). WaitForVerticalBlank
		// yields during the blank at the real refresh rate; if it no-ops (returns
		// non-OK, or the loop ran <8 ms so it clearly didn't pace), fall back to Sleep.
		{
			HRESULT hrVb = GfxWaitVBlank();
			DWORD dwElapsed = GetTickCount() - dwFrameStart;
			if (hrVb != 0 /*DD_OK*/ || dwElapsed < 8)
				Sleep(dwElapsed < 16 ? 16 - dwElapsed : 1);
		}
	}
}
