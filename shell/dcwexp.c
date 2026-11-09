//
// dcwexp.c - Windows Explorer, ported 1:1 from the stock CE shell (CEPC build).
// Layout off the real capture: a command bar with the File/Edit/View/Go/Favorites
// menubar on the left and back-arrow / ? / X buttons on the right, an "Address"
// bar with the current folder, a large-icon view of the directory contents, and
// a status bar. Enter opens a folder or launches an app; Backspace/Back = parent.
//
#include "dcwlib.h"

#define EW     320
#define EH     240
#define MAXENT 256

// chrome metrics measured off the real Explorer window
#define CMBAR_H 20 // command bar: menubar + back/?/X
#define ADDR_H  20 // "Address" bar
#define STAT_H  16 // status bar
#define CELL_W  64 // large-icon cell
#define CELL_H  56
#define LROW_H  20 // list/details row
#define MROW_H  18 // dropdown menu row

#define CL_FACEW RGB(192, 192, 192)
#define CL_SELW  RGB(0, 0, 128)
#define CL_TXT   RGB(0, 0, 0)
#define CL_DIS   RGB(128, 128, 128)

typedef struct
{
	WCHAR name[128];
	DWORD attr;
	DWORD size;
} ENT;

static ENT g_aEnt[MAXENT];
static int g_nCount, g_nSel, g_nTop;
static WCHAR g_szDir[MAX_PATH] = L"\\";

// view state: the real Explorer's View menu switches between these
enum
{
	VIEW_LARGE = 0,
	VIEW_LIST,
	VIEW_DETAIL
};
static int g_nView = VIEW_LARGE;
static int g_bAddrBar = 1; // View -> Toolbar/Address bar toggle
static int g_bStatBar = 1; // View -> Status Bar toggle

// dropdown menus off the menubar (stock Explorer: File/Edit/View/Go/Favorites)
#define MENU_N   5
#define MENU_POP 132

typedef struct
{
	const WCHAR *pszLabel;
	int nCmd;
	int bDis; // greyed - shown but can't be picked, like a stock separator/empty item
	int bSep;
} EMenu;

enum
{
	EMC_NONE = 0,
	EMC_SEP,
	EMC_CLOSE,
	EMC_SELALL,
	EMC_VIEW_LARGE,
	EMC_VIEW_LIST,
	EMC_VIEW_DETAIL,
	EMC_ADDRBAR,
	EMC_STATBAR,
	EMC_BACK,
	EMC_UP,
	EMC_GODOCS,
	EMC_GOWIN
};

static const EMenu s_aFile[] = {
    {L"New Folder", 0, 1, 0},
    {NULL, 0, 0, 1},
    {L"Delete", 0, 1, 0},
    {L"Rename", 0, 1, 0},
    {NULL, 0, 0, 1},
    {L"Properties", 0, 1, 0},
    {NULL, 0, 0, 1},
    {L"Close", EMC_CLOSE, 0, 0},
};
static const EMenu s_aEdit[] = {
    {L"Cut", 0, 1, 0},
    {L"Copy", 0, 1, 0},
    {L"Paste", 0, 1, 0},
    {NULL, 0, 0, 1},
    {L"Select All", EMC_SELALL, 0, 0},
};
static const EMenu s_aView[] = {
    {L"Toolbar", EMC_ADDRBAR, 0, 0},
    {L"Status Bar", EMC_STATBAR, 0, 0},
    {NULL, 0, 0, 1},
    {L"Large Icons", EMC_VIEW_LARGE, 0, 0},
    {L"Small Icons", EMC_VIEW_LIST, 0, 0},
    {L"Details", EMC_VIEW_DETAIL, 0, 0},
};
static const EMenu s_aGo[] = {
    {L"Back", EMC_BACK, 0, 0},
    {L"Up One Level", EMC_UP, 0, 0},
    {NULL, 0, 0, 1},
    {L"My Documents", EMC_GODOCS, 0, 0},
    {L"Windows", EMC_GOWIN, 0, 0},
};
static const EMenu s_aFav[] = {
    {L"(Empty)", 0, 1, 0},
};

static const WCHAR *s_aMenuLbl[MENU_N] = {L"File", L"Edit", L"View", L"Go", L"Favorites"};
static const EMenu *s_aMenus[MENU_N] = {s_aFile, s_aEdit, s_aView, s_aGo, s_aFav};
static const int s_anMenus[MENU_N] = {8, 5, 6, 5, 1};

static int g_nMenuOpen = -1; // which menubar dropdown is showing (-1 = none)
static int g_nMenuSel = -1;  // highlighted row inside it
static int g_bClose = 0;     // File -> Close asked us to quit

// menubar label x positions (filled by MeasureMenus): text + 8px padding per label
static int s_anMenuX[MENU_N + 1];

static int TextW(const WCHAR *psz) // ~6px/char average for the UI font
{
	return lstrlenW(psz) * 6;
}

static void MeasureMenus(void)
{
	int i, x = 4;
	for (i = 0; i < MENU_N; i++)
	{
		s_anMenuX[i] = x;
		x += TextW(s_aMenuLbl[i]) + 10;
	}
	s_anMenuX[MENU_N] = x;
}

static int MenuAtX(int x) // menubar label under x, -1 outside
{
	int i;
	for (i = 0; i < MENU_N; i++)
		if (x >= s_anMenuX[i] && x < s_anMenuX[i + 1])
			return i;
	return -1;
}

static void CopyN(WCHAR *pszDst, const WCHAR *psz, int n)
{
	int i;
	for (i = 0; i < n - 1 && psz[i]; i++)
		pszDst[i] = psz[i];
	pszDst[i] = 0;
}
static int IsRoot(void)
{
	return g_szDir[0] == L'\\' && g_szDir[1] == 0;
}
static int IsDir(ENT *pEnt)
{
	return (pEnt->attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static int EndsWithExe(const WCHAR *psz)
{
	int n = lstrlenW(psz);
	if (n < 4 || psz[n - 4] != L'.')
		return 0;
	return (psz[n - 3] | 32) == 'e' && (psz[n - 2] | 32) == 'x' && (psz[n - 1] | 32) == 'e';
}
static int EndsWithExt3(const WCHAR *psz, WCHAR a, WCHAR b, WCHAR c)
{
	int n = lstrlenW(psz);
	if (n < 4 || psz[n - 4] != L'.')
		return 0;
	return (psz[n - 3] | 32) == a && (psz[n - 2] | 32) == b && (psz[n - 1] | 32) == c;
}
static int IsAudio(const WCHAR *psz)
{
	return EndsWithExt3(psz, 'm', 'p', '3') || EndsWithExt3(psz, 'w', 'a', 'v');
}
static void JoinPath(WCHAR *pszOut, const WCHAR *pszDir, const WCHAR *pszName)
{
	if (pszDir[1] == 0)
		wsprintfW(pszOut, L"\\%s", pszName);
	else
		wsprintfW(pszOut, L"%s\\%s", pszDir, pszName);
}
static int FileIcon(ENT *pEnt)
{
	return IsDir(pEnt) ? ICON_FOLDER : (EndsWithExe(pEnt->name) ? ICON_APP : ICON_FILE);
}

// Directories sort first, then files - both alphabetical, like the stock Explorer.
static void SortEnt(void)
{
	int i, j;
	for (i = 1; i < g_nCount; i++)
	{
		ENT t = g_aEnt[i];
		int bTDir = IsDir(&t);
		for (j = i; j > 0; j--)
		{
			int bPDir = IsDir(&g_aEnt[j - 1]);
			if (lstrcmpW(g_aEnt[j - 1].name, L"..") == 0)
				break; // ".." always stays first
			if (bPDir && !bTDir)
				break; // dirs before files
			if (bPDir == bTDir && lstrcmpiW(g_aEnt[j - 1].name, t.name) <= 0)
				break; // alphabetical within the group
			g_aEnt[j] = g_aEnt[j - 1];
		}
		g_aEnt[j] = t;
	}
}

static void ScanDir(void)
{
	WCHAR aszPat[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	g_nCount = 0;
	g_nSel = 0;
	g_nTop = 0;
	if (!IsRoot())
	{
		lstrcpyW(g_aEnt[g_nCount].name, L"..");
		g_aEnt[g_nCount].attr = FILE_ATTRIBUTE_DIRECTORY;
		g_aEnt[g_nCount].size = 0;
		g_nCount++;
	}
	if (IsRoot())
		lstrcpyW(aszPat, L"\\*.*");
	else
		wsprintfW(aszPat, L"%s\\*.*", g_szDir);
	h = FindFirstFileW(aszPat, &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (g_nCount >= MAXENT)
				break;
			if (fd.cFileName[0] == L'.' &&
			    (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
				continue;
			CopyN(g_aEnt[g_nCount].name, fd.cFileName, 128);
			g_aEnt[g_nCount].attr = fd.dwFileAttributes;
			g_aEnt[g_nCount].size = fd.nFileSizeLow;
			g_nCount++;
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	SortEnt();
}

static void GoParent(void)
{
	int i, nLast = -1;
	if (IsRoot())
		return;
	for (i = 0; g_szDir[i]; i++)
		if (g_szDir[i] == L'\\')
			nLast = i;
	if (nLast <= 0)
		lstrcpyW(g_szDir, L"\\");
	else
		g_szDir[nLast] = 0;
	ScanDir();
}

static DCWin *g_pWin;

static void EnterSel(void)
{
	ENT *pEnt;
	WCHAR aszFull[MAX_PATH];
	if (g_nSel < 0 || g_nSel >= g_nCount)
		return;
	pEnt = &g_aEnt[g_nSel];
	if (lstrcmpW(pEnt->name, L"..") == 0)
	{
		GoParent();
		return;
	}
	if (IsDir(pEnt))
	{
		JoinPath(aszFull, g_szDir, pEnt->name);
		lstrcpyW(g_szDir, aszFull);
		ScanDir();
		return;
	}
	if (EndsWithExe(pEnt->name))
	{
		JoinPath(aszFull, g_szDir, pEnt->name);
		DCWinExec(g_pWin, aszFull); // shell launches it (windowed dcw* or fullscreen hand-off)
	}
	else if (IsAudio(pEnt->name)) // .mp3/.wav -> open in the music player with the path as its arg
	{
		WCHAR aszCmd[MAX_PATH + 16];
		JoinPath(aszFull, g_szDir, pEnt->name);
		lstrcpyW(aszCmd, L"dcwplay.exe ");
		lstrcatW(aszCmd, aszFull);
		DCWinExec(g_pWin, aszCmd); // shell splits "dcwplay.exe <path>" into exe + arg
	}
}

// Client-area layout: command bar, optional address bar, content, optional status bar.
static int ContTop(void)
{
	return CMBAR_H + (g_bAddrBar ? ADDR_H : 0);
}
static int ContBottom(int nCh)
{
	return nCh - (g_bStatBar ? STAT_H : 0);
}

// A body click arrives as VK_RETURN with the pointer still at the click point;
// route it to the chrome zone under the cursor instead of opening a file.
static void ClickAt(int px, int py, int nCw, int nCh)
{
	if (py < CMBAR_H) // command bar: menubar labels left, back/?/X buttons right
	{
		int m;
		if (px >= nCw - 17) // X close
		{
			g_bClose = 1;
			return;
		}
		if (px >= nCw - 34) // ? help - nothing to open (visual button, like a stub)
			return;
		if (px >= nCw - 51) // back arrow
		{
			GoParent();
			return;
		}
		m = MenuAtX(px);
		if (m >= 0)
		{
			g_nMenuOpen = (g_nMenuOpen == m) ? -1 : m;
			g_nMenuSel = -1;
		}
		return;
	}
	if (g_nMenuOpen >= 0) // click in an open dropdown (or Enter while it's up)
	{
		int mx = s_anMenuX[g_nMenuOpen], my = CMBAR_H;
		int n = s_anMenus[g_nMenuOpen], row = -1;
		const EMenu *a = s_aMenus[g_nMenuOpen];
		if (px >= mx && px < mx + MENU_POP && py >= my && py < my + n * MROW_H)
			row = (py - my) / MROW_H;
		else if (g_nMenuSel >= 0) // Enter with a highlighted row -> activate it
			row = g_nMenuSel;
		if (row >= 0 && row < n)
		{
			g_nMenuSel = row;
			// activate below via MenuActivate()
			{
				if (!a[row].bSep && !a[row].bDis)
					switch (a[row].nCmd)
					{
						case EMC_CLOSE:
							g_bClose = 1;
							break;
						case EMC_SELALL:
							break;
						case EMC_VIEW_LARGE:
							g_nView = VIEW_LARGE;
							break;
						case EMC_VIEW_LIST:
							g_nView = VIEW_LIST;
							break;
						case EMC_VIEW_DETAIL:
							g_nView = VIEW_DETAIL;
							break;
						case EMC_ADDRBAR:
							g_bAddrBar = !g_bAddrBar;
							break;
						case EMC_STATBAR:
							g_bStatBar = !g_bStatBar;
							break;
						case EMC_BACK:
						case EMC_UP:
							GoParent();
							break;
						case EMC_GODOCS:
							lstrcpyW(g_szDir, L"\\My Documents");
							ScanDir();
							break;
						case EMC_GOWIN:
							lstrcpyW(g_szDir, L"\\Windows");
							ScanDir();
							break;
					}
			}
		}
		g_nMenuOpen = -1;
		g_nMenuSel = -1;
		return;
	}
	if (g_bAddrBar && py < ContTop()) // address bar strip: not editable yet
		return;
	if (g_bStatBar && py >= ContBottom(nCh)) // status bar
		return;
	{ // file area: only open when the click is actually ON an item
		int idx = -1;
		if (g_nView == VIEW_LARGE)
		{
			int nCols = nCw / CELL_W;
			if (nCols < 1)
				nCols = 1;
			if (px / CELL_W < nCols)
				idx = g_nTop + ((py - ContTop()) / CELL_H) * nCols + (px / CELL_W);
		}
		else
			idx = g_nTop + (py - ContTop()) / LROW_H;
		if (idx >= 0 && idx < g_nCount)
		{
			g_nSel = idx;
			EnterSel();
		}
	}
}

static void HandleKey(DWORD dwKey, int nCw, int nCh)
{
	int nCols = 1, nRows;
	int nContH = ContBottom(nCh) - ContTop();
	if (dwKey == VK_ESCAPE)
	{
		if (g_nMenuOpen >= 0)
		{
			g_nMenuOpen = -1;
			return;
		}
	}
	if (g_nMenuOpen >= 0) // dropdown owns the arrows while it's up
	{
		int n = s_anMenus[g_nMenuOpen];
		const EMenu *a = s_aMenus[g_nMenuOpen];
		int d = 0, s = g_nMenuSel, k;
		if (dwKey == VK_UP)
			d = -1;
		else if (dwKey == VK_DOWN)
			d = 1;
		else if (dwKey == VK_LEFT || dwKey == VK_RIGHT) // jump between menubar labels
		{
			g_nMenuOpen += (dwKey == VK_LEFT) ? -1 : 1;
			if (g_nMenuOpen < 0)
				g_nMenuOpen = MENU_N - 1;
			if (g_nMenuOpen >= MENU_N)
				g_nMenuOpen = 0;
			g_nMenuSel = -1;
			return;
		}
		else if (dwKey == VK_RETURN)
		{
			if (s >= 0 && s < n && !a[s].bSep && !a[s].bDis)
			{
				// reuse the click path's activate: emulate a click on that row
				int px = s_anMenuX[g_nMenuOpen] + 4;
				int py = CMBAR_H + s * MROW_H + 2;
				ClickAt(px, py, nCw, nCh);
			}
			return;
		}
		else
			return;
		for (k = 0; k < n; k++)
		{
			s += d;
			if (s < 0)
				s = n - 1;
			if (s >= n)
				s = 0;
			if (!a[s].bSep && !a[s].bDis)
			{
				g_nMenuSel = s;
				return;
			}
		}
		return;
	}
	if (g_nView == VIEW_LARGE)
	{
		nCols = nCw / CELL_W;
		if (nCols < 1)
			nCols = 1;
	}
	else
	{
		nRows = nContH / LROW_H;
		if (nRows < 1)
			nRows = 1;
		nCols = 1;
	}
	if (dwKey == VK_LEFT && g_nSel > 0 && g_nView == VIEW_LARGE)
		g_nSel--;
	if (dwKey == VK_RIGHT && g_nSel < g_nCount - 1 && g_nView == VIEW_LARGE)
		g_nSel++;
	if (dwKey == VK_UP && g_nSel >= nCols)
		g_nSel -= nCols;
	if (dwKey == VK_DOWN && g_nSel + nCols < g_nCount)
		g_nSel += nCols;
	if (dwKey == VK_BACK)
		GoParent();
}

// --- drawing ---------------------------------------------------------------

// Small bevelled command-bar button (stock look): face + white top/left, grey
// bottom/right. All DCWinFill args are x,y,w,h.
static void CmdBtn(DCWin *pWin, int x, int w)
{
	DCWinFill(pWin, x, 2, w, CMBAR_H - 4, CL_FACEW);
	DCWinFill(pWin, x, 2, w, 1, RGB(255, 255, 255));     // raised top/left
	DCWinFill(pWin, x, 2, 1, CMBAR_H - 4, RGB(255, 255, 255));
	DCWinFill(pWin, x, CMBAR_H - 3, w, 1, RGB(128, 128, 128)); // bottom/right
	DCWinFill(pWin, x + w - 1, 2, 1, CMBAR_H - 4, RGB(128, 128, 128));
}

static void DrawCmdBar(DCWin *pWin, int nCw)
{
	int i;
	DCWinFill(pWin, 0, 0, nCw, CMBAR_H, CL_FACEW);
	for (i = 0; i < MENU_N; i++) // menubar labels
	{
		if (i == g_nMenuOpen)
			DCWinFill(pWin, s_anMenuX[i], 1, s_anMenuX[i + 1] - s_anMenuX[i], CMBAR_H - 2,
			          CL_SELW);
		DCWinText(pWin, s_anMenuX[i] + 4, 4,
		          (i == g_nMenuOpen) ? RGB(255, 255, 255) : CL_TXT,
		          (i == g_nMenuOpen) ? CL_SELW : CL_FACEW, s_aMenuLbl[i]);
	}
	{ // the command-bar grip: two thin vertical bars before the right-side buttons
		int gx = nCw - 58;
		DCWinFill(pWin, gx, 4, 2, CMBAR_H - 8, RGB(128, 128, 128));
		DCWinFill(pWin, gx + 4, 4, 2, CMBAR_H - 8, RGB(128, 128, 128));
	}
	{ // back arrow button
		int bx = nCw - 51;
		CmdBtn(pWin, bx, 16);
		// left-pointing arrow: tip column, widening head, horizontal shaft
		DCWinFill(pWin, bx + 4, 8, 1, 3, CL_TXT);
		DCWinFill(pWin, bx + 5, 6, 1, 7, CL_TXT);
		DCWinFill(pWin, bx + 6, 7, 1, 5, CL_TXT);
		DCWinFill(pWin, bx + 7, 8, 6, 1, CL_TXT);
		DCWinFill(pWin, bx + 7, 10, 6, 1, CL_TXT);
	}
	{ // ? help button
		CmdBtn(pWin, nCw - 34, 16);
		DCWinText(pWin, nCw - 34 + 5, 4, CL_TXT, CL_FACEW, L"?");
	}
	{ // X close button
		CmdBtn(pWin, nCw - 17, 15);
		DCWinText(pWin, nCw - 17 + 4, 4, CL_TXT, CL_FACEW, L"X");
	}
}

static void DrawAddrBar(DCWin *pWin, int nCw)
{
	int y = CMBAR_H;
	DCWinFill(pWin, 0, y, nCw, ADDR_H, CL_FACEW);
	DCWinText(pWin, 6, y + 4, CL_TXT, CL_FACEW, L"Address");
	{ // sunken white field + dropdown button, like the stock address combo
		DCWinFill(pWin, 52, y + 2, nCw - 78, 18, RGB(255, 255, 255));
		DCWinFill(pWin, 52, y + 2, nCw - 78, 1, RGB(128, 128, 128)); // sunken top/left
		DCWinFill(pWin, 52, y + 2, 1, 18, RGB(128, 128, 128));
		DCWinText(pWin, 56, y + 4, CL_TXT, RGB(255, 255, 255), g_szDir);
		DCWinFill(pWin, nCw - 24, y + 2, 22, 18, CL_FACEW); // dropdown button
		DCWinFill(pWin, nCw - 24, y + 2, 22, 1, RGB(255, 255, 255));
		DCWinFill(pWin, nCw - 24, y + 2, 1, 18, RGB(255, 255, 255));
		DCWinFill(pWin, nCw - 24, y + 19, 22, 1, RGB(128, 128, 128));
		DCWinFill(pWin, nCw - 3, y + 2, 1, 18, RGB(128, 128, 128));
		{
			int dx = nCw - 24 + (22 - 9) / 2, dy = y + 7, r;
			for (r = 0; r < 5; r++) // down-arrow glyph
				DCWinFill(pWin, dx + r, dy + r, 9 - 2 * r, 1, CL_TXT);
		}
	}
}

static void DrawMenu(DCWin *pWin)
{
	int n, i, mx, my;
	const EMenu *a;
	if (g_nMenuOpen < 0)
		return;
	a = s_aMenus[g_nMenuOpen];
	n = s_anMenus[g_nMenuOpen];
	mx = s_anMenuX[g_nMenuOpen];
	my = CMBAR_H;
	DCWinFill(pWin, mx, my, MENU_POP, n * MROW_H + 2, CL_FACEW);
	// raised bevel around the popup
	DCWinFill(pWin, mx, my, MENU_POP, 1, RGB(255, 255, 255));
	DCWinFill(pWin, mx, my, 1, n * MROW_H + 2, RGB(255, 255, 255));
	DCWinFill(pWin, mx + MENU_POP - 1, my, 1, n * MROW_H + 2, RGB(128, 128, 128));
	DCWinFill(pWin, mx, my + n * MROW_H + 1, MENU_POP, 1, RGB(128, 128, 128));
	for (i = 0; i < n; i++)
	{
		int y = my + i * MROW_H + 1;
		if (a[i].bSep)
		{
			DCWinFill(pWin, mx + 2, y + 3, MENU_POP - 4, 1, RGB(128, 128, 128));
			DCWinFill(pWin, mx + 2, y + 4, MENU_POP - 4, 1, RGB(255, 255, 255));
			continue;
		}
		if (i == g_nMenuSel)
			DCWinFill(pWin, mx + 1, y, MENU_POP - 2, MROW_H, CL_SELW);
		if (a[i].pszLabel)
			DCWinText(pWin, mx + 16, y + 4,
			          a[i].bDis ? CL_DIS : (i == g_nMenuSel) ? RGB(255, 255, 255) : CL_TXT,
			          (i == g_nMenuSel) ? CL_SELW : CL_FACEW, a[i].pszLabel);
		// check mark for toggled View options (a small tick: 2px down-stroke + arm)
		if ((a[i].nCmd == EMC_ADDRBAR && g_bAddrBar) ||
		    (a[i].nCmd == EMC_STATBAR && g_bStatBar) ||
		    (a[i].nCmd == EMC_VIEW_LARGE && g_nView == VIEW_LARGE) ||
		    (a[i].nCmd == EMC_VIEW_LIST && g_nView == VIEW_LIST) ||
		    (a[i].nCmd == EMC_VIEW_DETAIL && g_nView == VIEW_DETAIL))
		{
			DCWinFill(pWin, mx + 3, y + 9, 2, 2, CL_TXT);
			DCWinFill(pWin, mx + 5, y + 11, 2, 2, CL_TXT);
			DCWinFill(pWin, mx + 7, y + 6, 5, 2, CL_TXT);
			DCWinFill(pWin, mx + 7, y + 8, 2, 2, CL_TXT);
		}
	}
}

static void DrawStatus(DCWin *pWin, int nCw, int nCh)
{
	WCHAR asz[64];
	int y = nCh - STAT_H, nObj = g_nCount - (IsRoot() ? 0 : 1); // ".." isn't an object
	DCWinFill(pWin, 0, y, nCw, STAT_H, CL_FACEW);
	DCWinFill(pWin, 0, y, nCw, y + 1, RGB(255, 255, 255)); // top edge highlight
	if (g_nSel >= 0 && g_nSel < g_nCount && !IsDir(&g_aEnt[g_nSel]))
		wsprintfW(asz, L"%s  %u bytes", g_aEnt[g_nSel].name, g_aEnt[g_nSel].size);
	else
		wsprintfW(asz, L"%d object(s)", nObj);
	DCWinText(pWin, 6, y + 3, CL_TXT, CL_FACEW, asz);
}

static void DrawLarge(DCWin *pWin, int nCw, int nCh)
{
	int i, nCols = nCw / CELL_W, nRows = (ContBottom(nCh) - ContTop()) / CELL_H, nVis;
	if (nCols < 1)
		nCols = 1;
	if (nRows < 1)
		nRows = 1;
	nVis = nCols * nRows;
	if (g_nSel < g_nTop)
		g_nTop = (g_nSel / nCols) * nCols; // scroll back to the row holding the sel
	if (g_nSel >= g_nTop + nVis)
		g_nTop = (g_nSel / nCols - nRows + 1) * nCols;
	if (g_nTop < 0)
		g_nTop = 0;
	for (i = g_nTop; i < g_nCount && i < g_nTop + nVis; i++)
	{
		ENT *pEnt = &g_aEnt[i];
		int bSel = (i == g_nSel);
		int cx = ((i - g_nTop) % nCols) * CELL_W;
		int cy = ContTop() + ((i - g_nTop) / nCols) * CELL_H;
		if (bSel)
			DCWinFill(pWin, cx + 1, cy, CELL_W - 3, CELL_H, CL_SELW);
		DCWinIconBig(pWin, cx + (CELL_W - 32) / 2, cy + 4, FileIcon(pEnt));
		{ // label centered under the icon, clamped inside the cell
			int nTW = lstrlenW(pEnt->name) * 6, tx = cx + (CELL_W - nTW) / 2;
			if (tx < cx + 1)
				tx = cx + 1;
			DCWinText(pWin, tx, cy + 38, bSel ? RGB(255, 255, 255) : CL_TXT,
			          bSel ? CL_SELW : RGB(255, 255, 255), pEnt->name);
		}
	}
}

static void DrawList(DCWin *pWin, int nCw, int nCh, int bDetail)
{
	int i, y, nVis = (ContBottom(nCh) - ContTop()) / LROW_H;
	if (nVis < 1)
		nVis = 1;
	if (g_nSel < g_nTop)
		g_nTop = g_nSel;
	if (g_nSel >= g_nTop + nVis)
		g_nTop = g_nSel - nVis + 1;
	for (i = g_nTop; i < g_nCount && i < g_nTop + nVis; i++)
	{
		ENT *pEnt = &g_aEnt[i];
		int bSel = (i == g_nSel);
		COLORREF bg = bSel ? CL_SELW : RGB(255, 255, 255);
		COLORREF fg = bSel ? RGB(255, 255, 255) : CL_TXT;
		y = ContTop() + (i - g_nTop) * LROW_H;
		if (bSel)
			DCWinFill(pWin, 2, y, nCw - 4, LROW_H, CL_SELW);
		DCWinIcon(pWin, 6, y + 2, FileIcon(pEnt));
		DCWinText(pWin, 26, y + 3, fg, bg, pEnt->name);
		if (bDetail && !IsDir(pEnt)) // Details adds the size column, like the stock view
		{
			WCHAR aszSz[20];
			wsprintfW(aszSz, L"%u", pEnt->size);
			DCWinText(pWin, nCw - 80, y + 3, fg, bg, aszSz);
		}
	}
}

static void Draw(DCWin *pWin)
{
	int nCw = EW, nCh = EH;
	DCWinClientSize(pWin, &nCw, &nCh);
	DCWinBeginFrame(pWin);
	DCWinFillBg(pWin, RGB(255, 255, 255)); // content area is white, like the stock view

	DrawCmdBar(pWin, nCw);
	if (g_bAddrBar)
		DrawAddrBar(pWin, nCw);

	if (g_nView == VIEW_LARGE)
		DrawLarge(pWin, nCw, nCh);
	else
		DrawList(pWin, nCw, nCh, g_nView == VIEW_DETAIL);

	if (g_bStatBar)
		DrawStatus(pWin, nCw, nCh);
	DrawMenu(pWin); // dropdown floats above the content
	DCWinEndFrame(pWin);
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *pWin;
	DWORD dwKey;

	(void)hInst;
	if (lpCmd && lpCmd[0])
		CopyN(g_szDir, lpCmd, MAX_PATH); // start path from the command line

	MeasureMenus();
	pWin = DCWinOpen(60, 60, EW, EH, L"Explorer", ICON_EXPLORER);
	if (!pWin)
	{
		OutputDebugStringW(L"DCWEXP: DCWinOpen failed\r\n");
		return 1;
	}
	g_pWin = pWin;
	ScanDir();
	Draw(pWin);

	for (;;)
	{
		int bChanged = 0;
		// Pointer hover: highlight the row/cell under the cursor (matches D-pad
		// selection). The shell synthesizes VK_RETURN on a body click - ClickAt
		// decides whether the click hit chrome or a file.
		{
			int px = 0, py = 0, btn = 0, nCw = EW, nCh = EH;
			DCWinClientSize(pWin, &nCw, &nCh);
			if (DCWinGetPointer(pWin, &px, &py, &btn) && py >= ContTop() &&
			    py < ContBottom(nCh))
			{
				int idx;
				if (g_nView == VIEW_LARGE)
				{
					int nCols = nCw / CELL_W, nRows =
					    (ContBottom(nCh) - ContTop()) / CELL_H;
					if (nCols < 1)
						nCols = 1;
					if (nRows < 1)
						nRows = 1;
					idx = g_nTop + ((py - ContTop()) / CELL_H) * nCols +
					      (px / CELL_W);
					if (px / CELL_W < nCols)
					{
						if (idx >= 0 && idx < g_nCount &&
						    idx < g_nTop + nCols * nRows && idx != g_nSel)
						{
							g_nSel = idx;
							bChanged = 1;
						}
					}
				}
				else
				{
					idx = g_nTop + (py - ContTop()) / LROW_H;
					if (idx >= 0 && idx < g_nCount && idx != g_nSel)
					{
						g_nSel = idx;
						bChanged = 1;
					}
				}
			}
		}
		while (DCWinPollKey(pWin, &dwKey))
		{
			int nCw = EW, nCh = EH;
			DCWinClientSize(pWin, &nCw, &nCh);
			if (dwKey == VK_RETURN) // a click forwards here with the pointer in place
			{
				int px = 0, py = 0, btn = 0;
				if (DCWinGetPointer(pWin, &px, &py, &btn))
					ClickAt(px, py, nCw, nCh);
				else
					EnterSel(); // a real Enter key: no pointer over the window
			}
			else
				HandleKey(dwKey, nCw, nCh);
			bChanged = 1;
		}
		if (DCWinResized(pWin))
			bChanged = 1;
		if (bChanged)
			Draw(pWin);
		if (DCWinShouldClose(pWin) || g_bClose)
			break;
		Sleep(20);
	}

	DCWinClose(pWin);
	return 0;
}
