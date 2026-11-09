//
// dcwpword.c - "Microsoft Pocket Word": a port of the stock CEPC pword.exe. Menu
// bar (File Edit Format View), toolbar (New/Open/Save + B/I/U), white document
// surface with a blinking caret. Documents are real text files under
// \My Documents\*.pwt (the stock extension). Typing uses the shell's unshifted
// VK map; cursor keys move the caret; F2/F3 or the toolbar save/load.
//
#include "dcwlib.h"

#define MENUH 18
#define TBH 26
#define ROWH 12
#define MAXDOC 8192
#define DOCDIR L"\\My Documents"

static WCHAR s_aDoc[MAXDOC];
static int s_nLen = 0;
static int s_nCaret = 0;
static int s_nScroll = 0;
static WCHAR s_szFile[MAX_PATH] = L"";
static int s_nTick = 0;

static int VkChar(DWORD vk)
{
	if (vk >= 'A' && vk <= 'Z')
		return (int)vk + 32;
	if (vk >= '0' && vk <= '9')
		return (int)vk;
	switch (vk)
	{
		case VK_SPACE: return ' ';
		case VK_OEM_MINUS: return '-';
		case VK_OEM_PLUS: return '=';
		case VK_OEM_4: return '[';
		case VK_OEM_6: return ']';
		case VK_OEM_5: return '\\';
		case VK_OEM_1: return ';';
		case VK_OEM_7: return '\'';
		case VK_OEM_3: return '`';
		case VK_OEM_COMMA: return ',';
		case VK_OEM_PERIOD: return '.';
		case VK_OEM_2: return '/';
	}
	return 0;
}

static void LoadFile(const WCHAR *pszPath)
{
	HANDLE h;
	char ab[MAXDOC];
	DWORD cb;
	int i;
	s_nLen = 0;
	h = CreateFileW((WCHAR *)pszPath, GENERIC_READ, FILE_SHARE_READ, NULL,
	                OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (ReadFile(h, ab, sizeof(ab) - 1, &cb, NULL))
		for (i = 0; i < (int)cb && s_nLen < MAXDOC - 1; i++)
		{
			char c = ab[i];
			if (c == '\r')
				continue;
			s_aDoc[s_nLen++] = (WCHAR)(unsigned char)c;
		}
	CloseHandle(h);
	s_aDoc[s_nLen] = 0;
	s_nCaret = s_nLen;
	lstrcpyW(s_szFile, pszPath);
}

static void SaveFile(void)
{
	WCHAR szPath[MAX_PATH];
	char ab[MAXDOC];
	HANDLE h;
	DWORD cb;
	int i;
	if (!s_szFile[0])
	{
		lstrcpyW(szPath, DOCDIR);
		lstrcatW(szPath, L"\\Document.pwt");
		lstrcpyW(s_szFile, szPath);
	}
	for (i = 0; i < s_nLen; i++)
		ab[i] = (char)s_aDoc[i];
	h = CreateFileW(s_szFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	WriteFile(h, ab, s_nLen, &cb, NULL);
	CloseHandle(h);
}

// Open the first .pwt in \My Documents (stock File->Open shows a picker; the
// browser part is the Explorer port - this is the fast path).
static void OpenFirst(void)
{
	WCHAR szPat[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	lstrcpyW(szPat, DOCDIR);
	lstrcatW(szPat, L"\\*.pwt");
	h = FindFirstFileW(szPat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	{
		WCHAR szPath[MAX_PATH];
		lstrcpyW(szPath, DOCDIR);
		lstrcatW(szPath, L"\\");
		lstrcatW(szPath, fd.cFileName);
		LoadFile(szPath);
	}
	FindClose(h);
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1, nLastBtn = 0;

	w = DCWinOpen(14, 14, 300, 172, L"Pocket Word - Document", ICON_PWORD);
	if (!w)
		return 1;
	if (lpCmd && lpCmd[0])
		LoadFile(lpCmd);
	for (;;)
	{
		int nCw = 300, nCh = 172, px, py, btn, i, y, x, nCols, nLine = 0;
		int nCx = -1, nCy = -1;
		WCHAR szLine[80];

		DCWinClientSize(w, &nCw, &nCh);
		nCols = (nCw - 16) / 7;
		while (DCWinPollKey(w, &dwKey))
		{
			int ch;
			if ((ch = VkChar(dwKey)) && s_nLen < MAXDOC - 1)
			{
				for (i = s_nLen; i > s_nCaret; i--)
					s_aDoc[i] = s_aDoc[i - 1];
				s_aDoc[s_nCaret++] = (WCHAR)ch;
				s_aDoc[++s_nLen] = 0;
			}
			else if (dwKey == VK_RETURN && s_nLen < MAXDOC - 1)
			{
				for (i = s_nLen; i > s_nCaret; i--)
					s_aDoc[i] = s_aDoc[i - 1];
				s_aDoc[s_nCaret++] = L'\n';
				s_aDoc[++s_nLen] = 0;
			}
			else if (dwKey == VK_BACK && s_nCaret > 0)
			{
				for (i = s_nCaret - 1; i < s_nLen; i++)
					s_aDoc[i] = s_aDoc[i + 1];
				s_nCaret--;
				s_nLen--;
			}
			else if (dwKey == VK_LEFT && s_nCaret > 0)
				s_nCaret--;
			else if (dwKey == VK_RIGHT && s_nCaret < s_nLen)
				s_nCaret++;
			else if (dwKey == VK_UP && s_nCaret > nCols)
				s_nCaret -= nCols;
			else if (dwKey == VK_DOWN && s_nCaret + nCols < s_nLen)
				s_nCaret += nCols;
			else if (dwKey == VK_F2)
				SaveFile();
			else if (dwKey == VK_F3)
				OpenFirst();
			nDirty = 1;
		}
		if (DCWinGetPointer(w, &px, &py, &btn) && btn && !nLastBtn)
		{
			if (py >= MENUH && py < MENUH + TBH)
			{
				if (px < 16)
					s_nLen = 0, s_aDoc[0] = 0, s_nCaret = 0; // New
				else if (px < 32)
					OpenFirst(); // Open
				else if (px < 48)
					SaveFile(); // Save
				nDirty = 1;
			}
			else if (py >= MENUH + TBH)
			{
				int n = (py - MENUH - TBH - 4) / ROWH + s_nScroll * nCols;
				s_nCaret = n * nCols + (px - 8) / 7;
				if (s_nCaret < 0)
					s_nCaret = 0;
				if (s_nCaret > s_nLen)
					s_nCaret = s_nLen;
				nDirty = 1;
			}
		}
		nLastBtn = btn;
		if (DCWinResized(w))
			nDirty = 1;
		if (nDirty)
		{
			DCWinBeginFrame(w);
			DCWinFillBg(w, RGB(192, 192, 192));
			DCWinText(w, 6, 3, 0, RGB(192, 192, 192),
			          L"File  Edit  Format  View");
			DCWinFill(w, 0, MENUH - 1, nCw, 1, RGB(128, 128, 128));
			// toolbar buttons
			DCWinFill(w, 0, MENUH, nCw, TBH, RGB(192, 192, 192));
			DCWinText(w, 2, MENUH + 7, 0, RGB(192, 192, 192), L"N");
			DCWinText(w, 18, MENUH + 7, 0, RGB(192, 192, 192), L"O");
			DCWinText(w, 34, MENUH + 7, 0, RGB(192, 192, 192), L"S");
			DCWinFill(w, 52, MENUH + 4, 1, TBH - 8, RGB(128, 128, 128));
			DCWinText(w, 60, MENUH + 7, 0, RGB(192, 192, 192), L"B I U");
			DCWinFill(w, 0, MENUH + TBH - 1, nCw, 1, RGB(128, 128, 128));
			// document surface (white)
			DCWinFill(w, 0, MENUH + TBH, nCw, nCh - MENUH - TBH,
			          RGB(255, 255, 255));
			y = MENUH + TBH + 4;
			x = 8;
			szLine[0] = 0;
			{
				int li = 0, ln = 0;
				for (i = s_nScroll * nCols; i <= s_nLen; i++)
				{
					WCHAR c = s_aDoc[i];
					if (i == s_nCaret)
					{
						nCx = x;
						nCy = y;
					}
					if (c == L'\n' || li >= nCols - 1 || i == s_nLen)
					{
						szLine[li] = 0;
						if (y + ROWH <= nCh - 4)
							DCWinText(w, 8, y, 0, RGB(255, 255, 255), szLine);
						y += ROWH;
						x = 8;
						li = 0;
						ln++;
						if (y > nCh)
							break;
						continue;
					}
					szLine[li++] = c;
					x += 7;
				}
				if (s_nCaret == s_nLen && nCx < 0)
				{
					nCx = x;
					nCy = y - ROWH;
				}
			}
			// caret (blinks ~8 frames)
			if ((s_nTick & 8) && nCx >= 0 && nCy + ROWH <= nCh)
				DCWinFill(w, nCx, nCy, 1, ROWH, RGB(0, 0, 0));
			DCWinEndFrame(w);
			nDirty = 0;
		}
		s_nTick++;
		// keep caret visible
		{
			int nCw2 = 300, nCh2 = 172, nCols2, nRow;
			DCWinClientSize(w, &nCw2, &nCh2);
			nCols2 = (nCw2 - 16) / 7;
			nRow = s_nCaret / (nCols2 ? nCols2 : 1);
			if (nRow < s_nScroll)
				s_nScroll = nRow, nDirty = 1;
			else if (nRow > s_nScroll + (nCh2 - MENUH - TBH) / ROWH - 2)
				s_nScroll = nRow - (nCh2 - MENUH - TBH) / ROWH + 2,
				nDirty = 1;
		}
		if (DCWinShouldClose(w))
			break;
		Sleep(60);
	}
	DCWinClose(w);
	return 0;
}
