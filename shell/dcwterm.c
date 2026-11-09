//
// dcwterm.c - "Terminal": a port of the stock CEPC pegterm.exe. Menu bar
// (File/Edit/View) + toolbar + the "Make a New Session" body. Once a session is
// made it bridges the window to COM1: (the SCIF serial port): typed chars go
// out the wire, received chars render in the session text.
//
#include "dcwlib.h"

#define MENUH 18
#define TBH 24
#define ROWH 11
#define LINW 78
#define NLINE 120

static WCHAR s_aLine[NLINE][LINW];
static int s_nLine = 0;
static HANDLE s_hCom = INVALID_HANDLE_VALUE;
static int s_nSess = 0; // 0 = the "Make a New Session" body
static int s_nScroll = 0;

static void Put(const WCHAR *psz)
{
	int n = 0;
	while (*psz && n < LINW - 1)
		s_aLine[s_nLine % NLINE][n++] = *psz++;
	s_aLine[s_nLine % NLINE][n] = 0;
	s_nLine++;
}

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

static void SessOpen(void)
{
	DCB dcb;
	COMMTIMEOUTS to;
	if (s_hCom != INVALID_HANDLE_VALUE)
		CloseHandle(s_hCom);
	s_hCom = CreateFileW(L"COM1:", GENERIC_READ | GENERIC_WRITE, 0, NULL,
	                     OPEN_EXISTING, 0, NULL);
	if (s_hCom == INVALID_HANDLE_VALUE)
	{
		Put(L"Unable to open COM1:");
		s_nSess = 2; // session view anyway, shows the error
		return;
	}
	dcb.DCBlength = sizeof(dcb);
	GetCommState(s_hCom, &dcb);
	dcb.BaudRate = CBR_115200;
	dcb.fBinary = 1;
	dcb.fParity = 0;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	SetCommState(s_hCom, &dcb);
	to.ReadIntervalTimeout = 1;
	to.ReadTotalTimeoutMultiplier = 0;
	to.ReadTotalTimeoutConstant = 1;
	to.WriteTotalTimeoutMultiplier = 0;
	to.WriteTotalTimeoutConstant = 100;
	SetCommTimeouts(s_hCom, &to);
	s_nSess = 1;
	Put(L"--- session opened on COM1: 115200 8N1 ---");
}

static void SessClose(void)
{
	if (s_hCom != INVALID_HANDLE_VALUE)
		CloseHandle(s_hCom);
	s_hCom = INVALID_HANDLE_VALUE;
	s_nSess = 0;
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1, nLastBtn = 0;

	w = DCWinOpen(24, 24, 296, 168, L"Terminal", ICON_TERM);
	if (!w)
		return 1;
	for (;;)
	{
		int nCw = 296, nCh = 168, i, px, py, btn, nVis;
		DCWinClientSize(w, &nCw, &nCh);
		// serial rx -> lines
		if (s_hCom != INVALID_HANDLE_VALUE)
		{
			char ab[64];
			DWORD cb;
			if (ReadFile(s_hCom, ab, sizeof(ab), &cb, NULL) && cb)
			{
				WCHAR sz[LINW];
				int n = 0;
				for (i = 0; i < (int)cb && n < LINW - 2; i++)
				{
					char c = ab[i];
					if (c == '\r')
						continue;
					sz[n++] = (WCHAR)(unsigned char)c;
				}
				sz[n] = 0;
				Put(sz);
				nDirty = 1;
			}
		}
		while (DCWinPollKey(w, &dwKey))
		{
			if (s_nSess == 0)
			{
				if (dwKey == VK_RETURN)
					SessOpen(), nDirty = 1;
			}
			else
			{
				int ch;
				if (dwKey == VK_ESCAPE)
					SessClose(), nDirty = 1;
				else if ((ch = VkChar(dwKey)) &&
				         s_hCom != INVALID_HANDLE_VALUE)
				{
					char c = (char)ch;
					DWORD cb;
					WriteFile(s_hCom, &c, 1, &cb, NULL);
				}
				else if (dwKey == VK_RETURN &&
				         s_hCom != INVALID_HANDLE_VALUE)
				{
					DWORD cb;
					WriteFile(s_hCom, "\r", 1, &cb, NULL);
				}
			}
			if (dwKey == VK_UP && s_nScroll < s_nLine)
				s_nScroll++, nDirty = 1;
			else if (dwKey == VK_DOWN && s_nScroll > 0)
				s_nScroll--, nDirty = 1;
		}
		if (DCWinGetPointer(w, &px, &py, &btn) && btn && !nLastBtn)
		{
			if (s_nSess == 0 && py > MENUH + TBH)
				SessOpen(), nDirty = 1;
		}
		nLastBtn = btn;
		if (DCWinResized(w))
			nDirty = 1;
		if (nDirty)
		{
			DCWinBeginFrame(w);
			DCWinFillBg(w, RGB(192, 192, 192));
			DCWinText(w, 6, 3, 0, RGB(192, 192, 192), L"File  Edit  View");
			DCWinFill(w, 0, MENUH - 1, nCw, 1, RGB(128, 128, 128));
			// toolbar (grip + place-holder buttons like pegterm's)
			DCWinFill(w, 0, MENUH, nCw, TBH, RGB(192, 192, 192));
			DCWinFill(w, 4, MENUH + 4, 2, TBH - 8, RGB(128, 128, 128));
			DCWinFill(w, 0, MENUH + TBH - 1, nCw, 1, RGB(128, 128, 128));
			if (s_nSess == 0)
			{
				// stock pegterm body
				DCWinText(w, 14, MENUH + TBH + 20, 0, RGB(192, 192, 192),
				          L"Make a New Session");
				DCWinText(w, 14, MENUH + TBH + 40, RGB(128, 128, 128),
				          RGB(192, 192, 192),
				          L"Press Enter or tap to connect on COM1:");
			}
			else
			{
				DCWinFill(w, 0, MENUH + TBH, nCw, nCh - MENUH - TBH,
				          RGB(255, 255, 255));
				nVis = (nCh - MENUH - TBH - 4) / ROWH;
				for (i = 0; i < nVis; i++)
				{
					int n = s_nLine - nVis - s_nScroll + i;
					if (n < 0 || n >= s_nLine)
						continue;
					DCWinText(w, 4, MENUH + TBH + 2 + i * ROWH, 0,
					          RGB(255, 255, 255), s_aLine[n % NLINE]);
				}
			}
			DCWinEndFrame(w);
			nDirty = 0;
		}
		if (DCWinShouldClose(w))
			break;
		Sleep(50);
	}
	SessClose();
	DCWinClose(w);
	return 0;
}
