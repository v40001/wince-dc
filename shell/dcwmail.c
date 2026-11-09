//
// dcwmail.c - the "Inbox" window: a port of the stock CEPC pmail.exe layout. Menu
// bar (File/Compose/Services/Go), folder list on the left, message list + preview
// on the right, status bar. Messages are real files under
// \My Documents\Mail\<Folder>\*.txt: "From:", "Subject:" headers then the body.
// Delete moves the file into Deleted Items; New Message writes into Outbox.
//
#include "dcwlib.h"

#define MENUH 18
#define TREEW 84
#define STATH 16
#define ROWH 13
#define MSGDIR L"\\My Documents\\Mail"

static const WCHAR *const s_aFolder[5] = {
    L"Inbox", L"Outbox", L"Sent Items", L"Deleted Items", L"Drafts"};
static int s_nFolder = 0;
static int s_nSel = 0;
static int s_nMenuOpen = -1;

static WCHAR s_aMsg[64][MAX_PATH]; // message file names in the open folder
static int s_nMsg = 0;

static void Scan(void)
{
	WCHAR szPat[MAX_PATH], szDir[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	s_nMsg = 0;
	lstrcpyW(szDir, MSGDIR);
	lstrcatW(szDir, L"\\");
	lstrcatW(szDir, s_aFolder[s_nFolder]);
	CreateDirectoryW(MSGDIR, NULL);
	CreateDirectoryW(szDir, NULL);
	lstrcpyW(szPat, szDir);
	lstrcatW(szPat, L"\\*.txt");
	h = FindFirstFileW(szPat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		    s_nMsg < 64)
			lstrcpyW(s_aMsg[s_nMsg++], fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (s_nSel >= s_nMsg)
		s_nSel = s_nMsg - 1;
	if (s_nSel < 0)
		s_nSel = 0;
}

// Read the header fields + first body lines of the selected message.
static void MsgHead(int n, WCHAR *pFrom, WCHAR *pSub, WCHAR *pBody, int nBodyW)
{
	WCHAR szPath[MAX_PATH];
	char ab[2048];
	DWORD cb;
	HANDLE h;
	char *pE, *p;
	pFrom[0] = pSub[0] = 0;
	pBody[0] = 0;
	lstrcpyW(szPath, MSGDIR);
	lstrcatW(szPath, L"\\");
	lstrcatW(szPath, s_aFolder[s_nFolder]);
	lstrcatW(szPath, L"\\");
	lstrcatW(szPath, s_aMsg[n]);
	h = CreateFileW(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (!ReadFile(h, ab, sizeof(ab) - 1, &cb, NULL))
		cb = 0;
	CloseHandle(h);
	ab[cb] = 0;
	// headers: "From: x\nSubject: y\n\n<body>"
	for (p = ab; *p;)
	{
		pE = p;
		while (*pE && *pE != '\n')
			pE++;
		if (*p == '\r' || !*p) // blank line ends the headers
		{
			p = pE;
			while (*p == '\r' || *p == '\n')
				p++;
			break;
		}
		if (pE - p > 6 && p[0] == 'F' && p[1] == 'r' && p[2] == 'o' &&
		    p[3] == 'm' && p[4] == ':')
		{
			char *q = p + 5;
			int i = 0;
			while (*q == ' ')
				q++;
			while (q < pE && i < 30)
				pFrom[i++] = (WCHAR)(unsigned char)*q++;
			pFrom[i] = 0;
		}
		else if (pE - p > 8 && p[0] == 'S' && p[1] == 'u' && p[2] == 'b' &&
		         p[3] == 'j' && p[4] == 'e' && p[5] == 'c' && p[6] == 't' &&
		         p[7] == ':')
		{
			char *q = p + 8;
			int i = 0;
			while (*q == ' ')
				q++;
			while (q < pE && i < 40)
				pSub[i++] = (WCHAR)(unsigned char)*q++;
			pSub[i] = 0;
		}
		p = *pE ? pE + 1 : pE;
	}
	// body -> wide, first nBodyW chars
	{
		int i = 0;
		while (p && *p && i < nBodyW - 1)
		{
			WCHAR c = (WCHAR)(unsigned char)*p++;
			if (c == L'\r')
				continue;
			pBody[i++] = c;
		}
		pBody[i] = 0;
	}
}

static void DeleteSel(void)
{
	WCHAR szFrom[MAX_PATH], szTo[MAX_PATH];
	if (!s_nMsg)
		return;
	lstrcpyW(szFrom, MSGDIR);
	lstrcatW(szFrom, L"\\");
	lstrcatW(szFrom, s_aFolder[s_nFolder]);
	lstrcatW(szFrom, L"\\");
	lstrcatW(szFrom, s_aMsg[s_nSel]);
	lstrcpyW(szTo, MSGDIR);
	lstrcatW(szTo, L"\\Deleted Items");
	CreateDirectoryW(szTo, NULL);
	lstrcatW(szTo, L"\\");
	lstrcatW(szTo, s_aMsg[s_nSel]);
	MoveFileW(szFrom, szTo);
	Scan();
}

static void NewMsg(void)
{
	WCHAR sz[MAX_PATH];
	SYSTEMTIME st;
	HANDLE h;
	DWORD cb;
	static const char abHdr[] = "From: me@dreamcast\r\nSubject: (no subject)\r\n\r\n";
	GetLocalTime(&st);
	lstrcpyW(sz, MSGDIR);
	lstrcatW(sz, L"\\Outbox");
	CreateDirectoryW(sz, NULL);
	wsprintfW(sz + lstrlenW(sz), L"\\msg%02d%02d%02d.txt", st.wHour, st.wMinute,
	          st.wSecond);
	h = CreateFileW(sz, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (h != INVALID_HANDLE_VALUE)
	{
		WriteFile(h, abHdr, sizeof(abHdr) - 1, &cb, NULL);
		CloseHandle(h);
	}
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1, nLastBtn = 0;

	w = DCWinOpen(10, 10, 308, 176, L"Inbox", ICON_INBOX);
	if (!w)
		return 1;
	Scan();
	for (;;)
	{
		int nCw = 308, nCh = 176, i, px, py, btn, nListY;
		DCWinClientSize(w, &nCw, &nCh);
		while (DCWinPollKey(w, &dwKey))
		{
			if (dwKey == VK_LEFT && s_nFolder > 0)
				s_nFolder--, Scan(), nDirty = 1;
			else if (dwKey == VK_RIGHT && s_nFolder < 4)
				s_nFolder++, Scan(), nDirty = 1;
			else if (dwKey == VK_UP && s_nSel > 0)
				s_nSel--, nDirty = 1;
			else if (dwKey == VK_DOWN && s_nSel < s_nMsg - 1)
				s_nSel++, nDirty = 1;
			else if (dwKey == VK_DELETE)
				DeleteSel(), nDirty = 1;
			else if (dwKey == 'N')
				NewMsg(), Scan(), nDirty = 1;
			else if (dwKey == VK_RETURN)
			{
			}
		}
		if (DCWinGetPointer(w, &px, &py, &btn) && btn && !nLastBtn)
		{
			if (py < MENUH)
				; // menu bar (labels only - the stock pull-downs need commands)
			else if (px < TREEW)
			{
				int n = (py - MENUH - 4) / 14;
				if (n >= 0 && n < 5)
					s_nFolder = n, Scan(), nDirty = 1;
			}
			else
			{
				int n = (py - MENUH - 4) / ROWH;
				if (n >= 0 && n < s_nMsg)
					s_nSel = n, nDirty = 1;
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
			          L"File  Compose  Services  Go");
			DCWinFill(w, 0, MENUH - 1, nCw, 1, RGB(128, 128, 128));
			// folder tree
			for (i = 0; i < 5; i++)
			{
				int y = MENUH + 4 + i * 14;
				if (i == s_nFolder)
					DCWinFill(w, 2, y - 1, TREEW - 6, 14, RGB(0, 0, 128));
				DCWinIcon(w, 6, y, ICON_FOLDER);
				DCWinText(w, 26, y + 1,
				          i == s_nFolder ? RGB(255, 255, 255) : 0,
				          i == s_nFolder ? RGB(0, 0, 128)
				                         : RGB(192, 192, 192),
				          s_aFolder[i]);
			}
			DCWinFill(w, TREEW, MENUH, 1, nCh - MENUH - STATH,
			          RGB(128, 128, 128));
			// message list (white pane) + preview in the lower half
			nListY = MENUH;
			DCWinFill(w, TREEW + 1, nListY, nCw - TREEW - 1,
			          nCh - MENUH - STATH, RGB(255, 255, 255));
			for (i = 0; i < s_nMsg && i < (nCh - MENUH - STATH) / ROWH; i++)
			{
				int y = nListY + 4 + i * ROWH;
				WCHAR szFrom[32], szSub[40], szBody[256], szRow[MAX_PATH];
				MsgHead(i, szFrom, szSub, szBody, sizeof(szBody));
				if (i == s_nSel)
					DCWinFill(w, TREEW + 2, y - 1, nCw - TREEW - 4, ROWH,
					          RGB(0, 0, 128));
				wsprintfW(szRow, L"%s  %s", szFrom[0] ? szFrom : s_aMsg[i],
				          szSub);
				DCWinText(w, TREEW + 6, y,
				          i == s_nSel ? RGB(255, 255, 255) : 0,
				          i == s_nSel ? RGB(0, 0, 128)
				                      : RGB(255, 255, 255),
				          szRow);
			}
			// status bar
			DCWinFill(w, 0, nCh - STATH, nCw, STATH, RGB(192, 192, 192));
			DCWinFill(w, 0, nCh - STATH, nCw, 1, RGB(255, 255, 255));
			{
				WCHAR sz[64];
				wsprintfW(sz, L"%d message(s)", s_nMsg);
				DCWinText(w, 6, nCh - STATH + 2, 0, RGB(192, 192, 192), sz);
				DCWinText(w, 120, nCh - STATH + 2, 0, RGB(192, 192, 192),
				          L"N=New  Del=Delete");
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
