//
// dcwcpl.c - the "Control Panel" window: a port of the stock CEPC ctlpnl.exe. A grid
// of CPL applets (the cplmain.cpl/intll.cpl set) with a File menu + status bar.
// Applets that have a DC-side implementation open their page; the rest show the
// stock "(not available on this device)" body - same behaviour the real cplmain
// falls back to when a CPL's entry point declines.
//
#include "dcwlib.h"

#define CELLW 72
#define CELLH 56
#define MENUH 18
#define STATH 16

typedef struct
{
	const WCHAR *pszName;
	int nIcon;
	int nPage; // -1 = stub
} CPLAPP;

static const CPLAPP s_aCpl[] = {
    {L"Communications", ICON_COMM, -1},
    {L"Date/Time", ICON_CLOCK, 0},
    {L"Dialing", ICON_NET, -1},
    {L"Display", ICON_APP, 1},
    {L"Keyboard", ICON_FILE, -1},
    {L"Network", ICON_NET, -1},
    {L"Owner", ICON_APP, -1},
    {L"Password", ICON_FILE, -1},
    {L"PC Card", ICON_DRIVE, -1},
    {L"Power", ICON_POWER, 2},
    {L"Regional Settings", ICON_FOLDER, -1},
    {L"Remove Programs", ICON_BIN, -1},
    {L"Stylus", ICON_SIP, -1},
    {L"System", ICON_CPL, 3},
    {L"Volume and Sounds", ICON_HELP, -1},
};
#define CPLN (sizeof(s_aCpl) / sizeof(s_aCpl[0]))

static int s_nSel = 0;
static int s_nPage = -1; // -1 = the applet grid
static int s_nMenuOpen = 0;

// ---- functional applet bodies --------------------------------------------------

static void PageDateTime(int nCw, int nCh, DCWin *w)
{
	SYSTEMTIME st;
	WCHAR sz[80];
	GetLocalTime(&st);
	DCWinText(w, 8, 40, 0, RGB(192, 192, 192), L"Date:");
	wsprintfW(sz, L"%d/%d/%d", st.wMonth, st.wDay, st.wYear);
	DCWinText(w, 60, 40, 0, RGB(192, 192, 192), sz);
	DCWinText(w, 8, 60, 0, RGB(192, 192, 192), L"Time:");
	wsprintfW(sz, L"%d:%02d:%02d %s", (st.wHour % 12) ? st.wHour % 12 : 12,
	          st.wMinute, st.wSecond, st.wHour >= 12 ? L"PM" : L"AM");
	DCWinText(w, 60, 60, 0, RGB(192, 192, 192), sz);
	DCWinText(w, 8, 88, RGB(128, 128, 128), RGB(192, 192, 192),
	          L"Time Zone: (GMT-08:00) Pacific Time");
}

static void PageDisplay(int nCw, int nCh, DCWin *w)
{
	static const COLORREF aC[8] = {RGB(0, 0, 0),       RGB(128, 0, 0),
	                               RGB(0, 128, 0),     RGB(128, 128, 0),
	                               RGB(0, 0, 128),     RGB(128, 0, 128),
	                               RGB(0, 128, 128),   RGB(192, 192, 192)};
	int i;
	DCWinText(w, 8, 40, 0, RGB(192, 192, 192), L"Color scheme:");
	for (i = 0; i < 8; i++)
		DCWinFill(w, 8 + i * 24, 56, 20, 20, aC[i]);
}

static void PagePower(int nCw, int nCh, DCWin *w)
{
	// the Dreamcast runs on wall power - the stock page's fields are constant here
	DCWinText(w, 8, 40, 0, RGB(192, 192, 192), L"Power device: AC adapter");
	DCWinText(w, 8, 56, 0, RGB(192, 192, 192), L"Main battery: not present");
	DCWinText(w, 8, 72, 0, RGB(192, 192, 192), L"Suspend: exits to the loader");
}

static void PageSystem(int nCw, int nCh, DCWin *w)
{
	MEMORYSTATUS ms;
	SYSTEM_INFO si;
	WCHAR sz[80];
	DCWinText(w, 8, 40, 0, RGB(192, 192, 192), L"Microsoft Windows CE");
	DCWinText(w, 8, 54, 0, RGB(192, 192, 192), L"Version 2.12 (Build 9018)");
	DCWinText(w, 8, 68, 0, RGB(192, 192, 192), L"Dreamcast (Dragon)");
	GlobalMemoryStatus(&ms);
	wsprintfW(sz, L"Memory: %ld KB total, %ld KB free",
	          (long)(ms.dwTotalPhys / 1024), (long)(ms.dwAvailPhys / 1024));
	DCWinText(w, 8, 88, 0, RGB(192, 192, 192), sz);
	GetSystemInfo(&si);
	wsprintfW(sz, L"Processor: SH-4, %ld MHz", 200L);
	DCWinText(w, 8, 102, 0, RGB(192, 192, 192), sz);
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1, nLastBtn = 0;

	w = DCWinOpen(20, 20, 310, 172, L"Control Panel", ICON_CPL);
	if (!w)
		return 1;
	for (;;)
	{
		int nCw = 310, nCh = 172, nCols, i, px, py, btn;

		DCWinClientSize(w, &nCw, &nCh);
		nCols = (nCw - 4) / CELLW;
		if (nCols < 1)
			nCols = 1;
		while (DCWinPollKey(w, &dwKey))
		{
			if (s_nPage >= 0)
			{
				if (dwKey == VK_ESCAPE || dwKey == VK_BACK)
				{
					s_nPage = -1;
					nDirty = 1;
				}
			}
			else if (dwKey == VK_LEFT && s_nSel > 0)
				s_nSel--, nDirty = 1;
			else if (dwKey == VK_RIGHT && s_nSel < (int)CPLN - 1)
				s_nSel++, nDirty = 1;
			else if (dwKey == VK_UP && s_nSel >= nCols)
				s_nSel -= nCols, nDirty = 1;
			else if (dwKey == VK_DOWN && s_nSel + nCols < (int)CPLN)
				s_nSel += nCols, nDirty = 1;
			else if (dwKey == VK_RETURN)
			{
				// a body click forwards here too - the pointer hit-test below
				// re-picks the item when a click is involved
				if (s_aCpl[s_nSel].nPage >= 0)
					s_nPage = s_aCpl[s_nSel].nPage;
				nDirty = 1;
			}
		}
		if (DCWinGetPointer(w, &px, &py, &btn))
		{
			if (btn && !nLastBtn && s_nPage < 0 && py >= MENUH &&
			    py < nCh - STATH)
			{
				int n = ((py - MENUH) / CELLH) * nCols + px / CELLW;
				if (n >= 0 && n < (int)CPLN)
				{
					if (s_nSel == n && s_aCpl[n].nPage >= 0)
						s_nPage = s_aCpl[n].nPage;
					s_nSel = n;
					nDirty = 1;
				}
			}
		}
		nLastBtn = btn;
		if (DCWinResized(w))
			nDirty = 1;
		if (nDirty)
		{
			DCWinBeginFrame(w);
			DCWinFillBg(w, RGB(192, 192, 192));
			// menu bar
			DCWinFill(w, 0, 0, nCw, MENUH, RGB(192, 192, 192));
			DCWinFill(w, 0, MENUH - 1, nCw, 1, RGB(128, 128, 128));
			DCWinText(w, 6, 2, 0, RGB(192, 192, 192), L"File");
			if (s_nPage >= 0)
			{
				switch (s_nPage)
				{
					case 0: PageDateTime(nCw, nCh, w); break;
					case 1: PageDisplay(nCw, nCh, w); break;
					case 2: PagePower(nCw, nCh, w); break;
					case 3: PageSystem(nCw, nCh, w); break;
				}
				DCWinText(w, 8, 24, 0, RGB(192, 192, 192), s_aCpl[s_nSel].pszName);
			}
			else
			{
				for (i = 0; i < (int)CPLN; i++)
				{
					int cx = 2 + (i % nCols) * CELLW;
					int cy = MENUH + 2 + (i / nCols) * CELLH;
					if (cy + CELLH > nCh - STATH)
						break;
					if (i == s_nSel)
						DCWinFill(w, cx, cy, CELLW - 4, CELLH - 4,
						          RGB(0, 0, 128));
					DCWinIconBig(w, cx + (CELLW - 4 - 32) / 2, cy + 2,
					             s_aCpl[i].nIcon);
					DCWinText(w, cx + 2, cy + 36,
					          i == s_nSel ? RGB(255, 255, 255) : 0,
					          i == s_nSel ? RGB(0, 0, 128)
					                      : RGB(192, 192, 192),
					          s_aCpl[i].pszName);
				}
			}
			// status bar
			DCWinFill(w, 0, nCh - STATH, nCw, STATH, RGB(192, 192, 192));
			DCWinFill(w, 0, nCh - STATH, nCw, 1, RGB(255, 255, 255));
			{
				WCHAR sz[64];
				wsprintfW(sz, L"%d object(s)", s_nPage >= 0 ? 1 : (int)CPLN);
				DCWinText(w, 6, nCh - STATH + 2, 0, RGB(192, 192, 192), sz);
			}
			DCWinEndFrame(w);
			nDirty = 0;
		}
		if (DCWinShouldClose(w))
			break;
		Sleep(60);
	}
	DCWinClose(w);
	return 0;
}
