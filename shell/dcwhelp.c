//
// dcwhelp.c - the "Windows CE Help" window: a port of peghelp.exe's layout. Toolbar
// with Contents / Back / All Topics, a topic body with wrapped text and blue links,
// and a contents tree. Topics match the CEPC help set (Getting Started, Working
// With Programs, The Desktop, Taskbar, etc) - the text is the short stock summary.
//
#include "dcwlib.h"

#define TBH 22
#define NMAXLINK 4

typedef struct
{
	const WCHAR *pszTitle;
	const WCHAR *pszBody; // '\n' separated paragraphs; links refer to topic indices
	int aLink[NMAXLINK];
} TOPIC;

static const TOPIC s_aTopics[] = {
    {L"Getting Started",
     L"Welcome to Microsoft Windows CE.\n"
     L"To open an item on the desktop, double-tap it.\n"
     L"To see the commands available for an item, tap "
     L"and hold it.\n"
     L"The taskbar shows the Start button, the programs "
     L"that are running, and the current time.",
     {1, 3, -1, -1}},
    {L"Working With Programs",
     L"To start a program, tap the Start button, point to "
     L"Programs, and then tap the program you want.\n"
     L"To switch between programs, tap a program button "
     L"on the taskbar.",
     {3, -1, -1, -1}},
    {L"Finding Files",
     L"To find a file or folder, double-tap My Computer "
     L"on the desktop and browse the folders.\n"
     L"Recently used documents appear on the Documents "
     L"menu.",
     {-1, -1, -1, -1}},
    {L"The Desktop",
     L"The desktop contains shortcuts to the items you "
     L"use most: My Computer, the Recycle Bin, your "
     L"Inbox and My Documents.\n"
     L"Tap and hold a shortcut for more commands.",
     {4, -1, -1, -1}},
    {L"The Taskbar",
     L"The taskbar contains the Start button, buttons "
     L"for each open program, and the status area.\n"
     L"To move the taskbar or hide it, tap Start, point "
     L"to Settings, and then tap Taskbar.",
     {-1, -1, -1, -1}},
    {L"Getting Help",
     L"Tap Contents to see the list of help books.\n"
     L"Tap All Topics to list every topic.\n"
     L"Tap Back to return to the previous topic.",
     {-1, -1, -1, -1}},
};
#define NTOPIC (sizeof(s_aTopics) / sizeof(s_aTopics[0]))

static int s_nTopic = -1; // -1 = contents page
static int s_nBack[16];
static int s_nBackTop = 0;
static int s_nScroll = 0;

static void DrawTb(DCWin *w, int nCw)
{
	DCWinFill(w, 0, 0, nCw, TBH, RGB(192, 192, 192));
	DCWinFill(w, 0, TBH - 1, nCw, 1, RGB(128, 128, 128));
	// Contents / Back / All Topics buttons (raised)
	{
		static const WCHAR *aB[3] = {L"Contents", L"Back", L"All Topics"};
		int x = 2, i;
		for (i = 0; i < 3; i++)
		{
			int bw = 8 + lstrlenW(aB[i]) * 7;
			DCWinFill(w, x, 2, bw, TBH - 4, RGB(192, 192, 192));
			DCWinFill(w, x, 2, bw, 1, RGB(255, 255, 255));
			DCWinFill(w, x, 2, 1, TBH - 4, RGB(255, 255, 255));
			DCWinFill(w, x, TBH - 4, bw, 1, RGB(128, 128, 128));
			DCWinFill(w, x + bw - 1, 2, 1, TBH - 4, RGB(128, 128, 128));
			DCWinText(w, x + 4, 5, 0, RGB(192, 192, 192), aB[i]);
			x += bw + 2;
		}
	}
}

// Wrap psz into <= CW columns starting at y0; returns rows used.
static void CpyN(WCHAR *pDst, const WCHAR *pSrc, int n)
{
	int i = 0;
	while (i < n - 1 && pSrc[i])
	{
		pDst[i] = pSrc[i];
		i++;
	}
	pDst[i] = 0;
}

static int PutWrapped(DCWin *w, int x, int y, const WCHAR *psz, int nCw,
                      int nMaxY)
{
	WCHAR sz[80];
	int n = 0;
	while (*psz && y < nMaxY)
	{
		int nTake = 0, nLast = 0;
		// take up to nCw/7 chars, break at a space
		while (psz[nTake] && psz[nTake] != L'\n' && nTake < nCw / 7)
		{
			if (psz[nTake] == L' ')
				nLast = nTake;
			nTake++;
		}
		if (psz[nTake] && psz[nTake] != L'\n' && nLast > 0)
			nTake = nLast + 1;
		CpyN(sz, psz, nTake + 1);
		sz[nTake] = 0;
		while (nTake > 0 && sz[nTake - 1] == L' ')
			sz[--nTake] = 0;
		DCWinText(w, x, y, 0, RGB(255, 255, 255), sz);
		y += 12;
		psz += nTake;
		while (*psz == L' ')
			psz++;
		if (*psz == L'\n')
			psz++, y += 4;
		n++;
	}
	return n;
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1, nLastBtn = 0;

	w = DCWinOpen(40, 24, 290, 170, L"Windows CE Help", ICON_HELP);
	if (!w)
		return 1;
	for (;;)
	{
		int nCw = 290, nCh = 170, i, px, py, btn;
		DCWinClientSize(w, &nCw, &nCh);
		while (DCWinPollKey(w, &dwKey))
		{
			if (dwKey == VK_UP && s_nScroll > 0)
				s_nScroll--, nDirty = 1;
			else if (dwKey == VK_DOWN)
				s_nScroll++, nDirty = 1;
			else if (dwKey == VK_BACK || dwKey == VK_ESCAPE)
			{
				if (s_nBackTop > 0)
					s_nTopic = s_nBack[--s_nBackTop];
				else
					s_nTopic = -1;
				s_nScroll = 0;
				nDirty = 1;
			}
			else if (dwKey == VK_RETURN)
			{
				// click forwards here with the pointer placed; hit-test below
			}
		}
		if (DCWinGetPointer(w, &px, &py, &btn) && btn && !nLastBtn)
		{
			if (py < TBH)
			{
				// toolbar buttons
				if (px < 60) // Contents
					s_nTopic = -1, s_nBackTop = 0, s_nScroll = 0;
				else if (px < 102) // Back
				{
					if (s_nBackTop > 0)
						s_nTopic = s_nBack[--s_nBackTop];
				}
				else if (px < 178) // All Topics
					s_nTopic = -2, s_nScroll = 0;
				nDirty = 1;
			}
			else if (s_nTopic == -1)
			{
				int n = (py - TBH - 24 - 4) / 14 + s_nScroll;
				if (n >= 0 && n < (int)NTOPIC)
				{
					if (s_nBackTop < 15)
						s_nBack[s_nBackTop++] = s_nTopic;
					s_nTopic = n;
					s_nScroll = 0;
					nDirty = 1;
				}
			}
			else if (s_nTopic == -2)
			{
				int n = (py - TBH - 24 - 4) / 14 + s_nScroll;
				if (n >= 0 && n < (int)NTOPIC)
				{
					if (s_nBackTop < 15)
						s_nBack[s_nBackTop++] = s_nTopic;
					s_nTopic = n;
					s_nScroll = 0;
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
			DCWinFillBg(w, RGB(255, 255, 255));
			DrawTb(w, nCw);
			if (s_nTopic == -1) // contents
			{
				DCWinText(w, 8, TBH + 6, 0, RGB(255, 255, 255),
				          L"Contents");
				for (i = 0; i < (int)NTOPIC; i++)
				{
					int y = TBH + 24 + (i - s_nScroll) * 14;
					if (y < TBH + 20 || y > nCh - 12)
						continue;
					DCWinText(w, 12, y, RGB(0, 0, 255), RGB(255, 255, 255),
					          s_aTopics[i].pszTitle);
				}
			}
			else if (s_nTopic == -2) // all topics
			{
				DCWinText(w, 8, TBH + 6, 0, RGB(255, 255, 255),
				          L"All Topics");
				for (i = 0; i < (int)NTOPIC; i++)
				{
					int y = TBH + 24 + (i - s_nScroll) * 14;
					if (y < TBH + 20 || y > nCh - 12)
						continue;
					DCWinText(w, 12, y, RGB(0, 0, 255), RGB(255, 255, 255),
					          s_aTopics[i].pszTitle);
				}
			}
			else
			{
				const TOPIC *t = &s_aTopics[s_nTopic];
				DCWinText(w, 8, TBH + 6, 0, RGB(255, 255, 255),
				          t->pszTitle);
				DCWinFill(w, 8, TBH + 20, nCw - 16, 1, RGB(0, 0, 0));
				{
					// scroll by skipping leading paragraphs
					const WCHAR *p = t->pszBody;
					int n = s_nScroll;
					while (n-- > 0 && *p)
					{
						while (*p && *p != L'\n')
							p++;
						if (*p)
							p++;
					}
					PutWrapped(w, 8, TBH + 26, p, nCw, nCh - 8);
				}
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
