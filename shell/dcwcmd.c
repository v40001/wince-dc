//
// dcwcmd.c - the "Command Prompt" window: a port of the stock CEPC cmd.exe. White-on-
// black console with the real cmd command set over the CE filesystem: dir, cd, ver,
// cls, type, copy, del, md, rd, ren, attrib, help, exit. VKs arrive from the shell's
// keyboard map (unshifted US layout - the SIP/controller never produces shifted chars,
// which matches how a real H/PC session types commands in lowercase).
//
#include "dcwlib.h"
#include <stdarg.h>

#define CW_CH 7 // avg char cell (the 11px font's advance for the glyphs we emit)
#define ROW_H 11
#define NSCROLL 300
#define LINW 76

static WCHAR s_aLines[NSCROLL][LINW];
static int s_nLine = 0;
static WCHAR s_szCwd[MAX_PATH] = L"\\";
static WCHAR s_szIn[LINW];
static int s_nIn = 0;
static int s_nHist = 0; // scrollback offset

static void Put(const WCHAR *psz)
{
	int n = 0;
	while (*psz && n < LINW - 1)
		s_aLines[s_nLine % NSCROLL][n++] = *psz++;
	s_aLines[s_nLine % NSCROLL][n] = 0;
	s_nLine++;
}

static void PutF(const WCHAR *fmt, ...)
{
	WCHAR sz[LINW];
	va_list ap;
	va_start(ap, fmt);
	wvsprintfW(sz, fmt, ap);
	va_end(ap);
	Put(sz);
}

// VK -> console char (same unshifted US map the shell's Run dialog uses).
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

// Build an absolute path from s_szCwd + a (possibly absolute) argument.
static void AbsPath(WCHAR *pOut, const WCHAR *pArg)
{
	if (pArg[0] == L'\\')
		lstrcpyW(pOut, pArg);
	else
	{
		lstrcpyW(pOut, s_szCwd);
		if (pOut[lstrlenW(pOut) - 1] != L'\\')
			lstrcatW(pOut, L"\\");
		lstrcatW(pOut, pArg);
	}
}

// Normalize a path in place: collapse "." and ".." segments.
static void NormPath(WCHAR *p)
{
	WCHAR *pSeg, sz[MAX_PATH];
	int i = 0;
	sz[0] = L'\\';
	sz[1] = 0;
	pSeg = p;
	while (*pSeg == L'\\')
		pSeg++;
	while (*pSeg)
	{
		WCHAR *pEnd = pSeg;
		WCHAR szSeg[MAX_PATH];
		int n;
		while (*pEnd && *pEnd != L'\\')
			pEnd++;
		n = (int)(pEnd - pSeg);
		lstrcpynW(szSeg, pSeg, n + 1);
		szSeg[n] = 0;
		if (!lstrcmpW(szSeg, L".."))
		{
			int L = lstrlenW(sz);
			while (L > 1 && sz[L - 1] != L'\\')
				L--;
			if (L > 1)
				sz[--L] = 0; // drop the last segment (keep the root)
		}
		else if (lstrcmpW(szSeg, L".") && szSeg[0])
		{
			if (lstrlenW(sz) > 1)
				lstrcatW(sz, L"\\");
			lstrcatW(sz, szSeg);
		}
		pSeg = pEnd;
		while (*pSeg == L'\\')
			pSeg++;
	}
	lstrcpyW(p, sz);
}

static void CmdDir(const WCHAR *pArg)
{
	WCHAR szPat[MAX_PATH], sz[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	int nDirs = 0, nFiles = 0;
	DWORD cb = 0;

	AbsPath(szPat, pArg && *pArg ? pArg : L"\\");
	NormPath(szPat);
	if (szPat[lstrlenW(szPat) - 1] != L'\\')
		lstrcatW(szPat, L"\\");
	lstrcatW(szPat, L"*");
	PutF(L" Directory of %s", szPat);
	Put(L"");
	h = FindFirstFileW(szPat, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		Put(L"File Not Found");
		return;
	}
	do {
		if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L".."))
			continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			wsprintfW(sz, L"%s <DIR>", fd.cFileName);
			nDirs++;
		}
		else
		{
			wsprintfW(sz, L"%s %ld", fd.cFileName, (long)fd.nFileSizeLow);
			nFiles++;
			cb += fd.nFileSizeLow;
		}
		Put(sz);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	PutF(L"        %ld File(s) %ld bytes", (long)nFiles, (long)cb);
	PutF(L"        %ld Dir(s)", (long)nDirs);
}

static void CmdType(const WCHAR *pArg)
{
	WCHAR sz[MAX_PATH];
	char ab[256];
	HANDLE h;
	DWORD cb, i;
	WCHAR szLine[LINW];
	int n = 0;

	AbsPath(sz, pArg);
	NormPath(sz);
	h = CreateFileW(sz, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		Put(L"File not found");
		return;
	}
	for (;;)
	{
		if (!ReadFile(h, ab, sizeof(ab) - 1, &cb, NULL) || !cb)
			break;
		ab[cb] = 0;
		for (i = 0; i < cb; i++)
		{
			char c = ab[i];
			if (c == '\r')
				continue;
			if (c == '\n' || n >= LINW - 1)
			{
				szLine[n] = 0;
				Put(szLine);
				n = 0;
			}
			else
				szLine[n++] = (WCHAR)(unsigned char)c;
		}
	}
	if (n)
	{
		szLine[n] = 0;
		Put(szLine);
	}
	CloseHandle(h);
}

static void CmdCopy(const WCHAR *pArgs)
{
	WCHAR szFrom[MAX_PATH], szTo[MAX_PATH];
	const WCHAR *pSp = pArgs;
	while (*pSp && *pSp != L' ')
		pSp++;
	if (!*pSp)
	{
		Put(L"The syntax of the command is incorrect.");
		return;
	}
	lstrcpynW(szFrom, pArgs, (int)(pSp - pArgs) + 1);
	szFrom[pSp - pArgs] = 0;
	while (*pSp == L' ')
		pSp++;
	AbsPath(szFrom, szFrom);
	NormPath(szFrom);
	AbsPath(szTo, pSp);
	NormPath(szTo);
	if (GetFileAttributesW(szTo) != 0xFFFFFFFF &&
	    (GetFileAttributesW(szTo) & FILE_ATTRIBUTE_DIRECTORY))
	{
		// dest is a dir: append the source's filename
		const WCHAR *pName = szFrom + lstrlenW(szFrom);
		while (pName > szFrom && pName[-1] != L'\\')
			pName--;
		if (szTo[lstrlenW(szTo) - 1] != L'\\')
			lstrcatW(szTo, L"\\");
		lstrcatW(szTo, pName);
	}
	if (CopyFileW(szFrom, szTo, FALSE))
		Put(L"        1 file(s) copied.");
	else
		Put(L"Access denied");
}

static void ExecLine(void)
{
	WCHAR szCmd[LINW], szArg[LINW], szPath[MAX_PATH];
	int i = 0, j = 0;

	szArg[0] = 0;
	while (s_szIn[i] == L' ')
		i++;
	while (s_szIn[i] && s_szIn[i] != L' ')
		szCmd[j++] = s_szIn[i++];
	szCmd[j] = 0;
	while (s_szIn[i] == L' ')
		i++;
	lstrcpyW(szArg, s_szIn + i);

	PutF(L"%s>%s", s_szCwd, s_szIn);
	if (!szCmd[0])
		return;
	if (!lstrcmpW(szCmd, L"exit"))
	{
		Put(L"(close the window to quit)");
		return;
	}
	if (!lstrcmpW(szCmd, L"ver"))
	{
		Put(L"");
		Put(L"Pocket CMD v 2.11");
		return;
	}
	if (!lstrcmpW(szCmd, L"cls"))
	{
		s_nLine = 0;
		s_nHist = 0;
		return;
	}
	if (!lstrcmpW(szCmd, L"help") || !lstrcmpW(szCmd, L"?"))
	{
		Put(L"attrib  cd/chdir  cls  copy  del/erase");
		Put(L"dir     exit      help  md/mkdir");
		Put(L"rd/rmdir  ren/rename  type  ver");
		return;
	}
	if (!lstrcmpW(szCmd, L"dir"))
	{
		CmdDir(szArg);
		return;
	}
	if (!lstrcmpW(szCmd, L"cd") || !lstrcmpW(szCmd, L"chdir"))
	{
		if (!szArg[0])
		{
			Put(s_szCwd);
			return;
		}
		AbsPath(szPath, szArg);
		NormPath(szPath);
		{
			DWORD dw = GetFileAttributesW(szPath);
			if (dw == 0xFFFFFFFF || !(dw & FILE_ATTRIBUTE_DIRECTORY))
			{
				Put(L"Invalid directory");
				return;
			}
		}
		lstrcpyW(s_szCwd, szPath);
		return;
	}
	if (!lstrcmpW(szCmd, L"md") || !lstrcmpW(szCmd, L"mkdir"))
	{
		AbsPath(szPath, szArg);
		NormPath(szPath);
		if (!CreateDirectoryW(szPath, NULL))
			Put(L"A subdirectory or file already exists");
		return;
	}
	if (!lstrcmpW(szCmd, L"rd") || !lstrcmpW(szCmd, L"rmdir"))
	{
		AbsPath(szPath, szArg);
		NormPath(szPath);
		if (!RemoveDirectoryW(szPath))
			Put(L"Access denied");
		return;
	}
	if (!lstrcmpW(szCmd, L"del") || !lstrcmpW(szCmd, L"erase"))
	{
		AbsPath(szPath, szArg);
		NormPath(szPath);
		if (!DeleteFileW(szPath))
			Put(L"File not found");
		return;
	}
	if (!lstrcmpW(szCmd, L"ren") || !lstrcmpW(szCmd, L"rename"))
	{
		const WCHAR *pSp = szArg;
		WCHAR szTo[MAX_PATH];
		while (*pSp && *pSp != L' ')
			pSp++;
		if (!*pSp)
		{
			Put(L"The syntax of the command is incorrect.");
			return;
		}
		lstrcpynW(szPath, szArg, (int)(pSp - szArg) + 1);
		szPath[pSp - szArg] = 0;
		while (*pSp == L' ')
			pSp++;
		lstrcpyW(szTo, szPath); // from
		AbsPath(szPath, szTo);
		NormPath(szPath);
		{
			WCHAR szToAbs[MAX_PATH];
			AbsPath(szToAbs, pSp);
			NormPath(szToAbs);
			if (!MoveFileW(szPath, szToAbs))
				Put(L"Access denied");
		}
		return;
	}
	if (!lstrcmpW(szCmd, L"copy"))
	{
		CmdCopy(szArg);
		return;
	}
	if (!lstrcmpW(szCmd, L"type"))
	{
		if (!szArg[0])
			Put(L"The syntax of the command is incorrect.");
		else
			CmdType(szArg);
		return;
	}
	if (!lstrcmpW(szCmd, L"attrib"))
	{
		DWORD dw;
		AbsPath(szPath, szArg);
		NormPath(szPath);
		dw = GetFileAttributesW(szPath);
		if (dw == 0xFFFFFFFF)
			Put(L"File not found");
		else
			PutF(L"%s%s%s%s %s", (dw & FILE_ATTRIBUTE_READONLY) ? L"R" : L" ",
			     (dw & FILE_ATTRIBUTE_HIDDEN) ? L"H" : L" ",
			     (dw & FILE_ATTRIBUTE_SYSTEM) ? L"S" : L" ",
			     (dw & FILE_ATTRIBUTE_ARCHIVE) ? L"A" : L" ", szPath);
		return;
	}
	PutF(L"'%s' is not recognized as an internal or external command,", szCmd);
	Put(L"operable program or batch file.");
}

int DcwMain(HINSTANCE hInst, LPWSTR lpCmd)
{
	DCWin *w;
	DWORD dwKey;
	int nDirty = 1;

	w = DCWinOpen(30, 30, 300, 150, L"Command Prompt", ICON_CMD);
	if (!w)
		return 1;
	Put(L"Pocket CMD v 2.11");
	Put(L"");
	for (;;)
	{
		int nCw = 300, nCh = 150, nVis, i, y, nFirst;

		DCWinClientSize(w, &nCw, &nCh);
		while (DCWinPollKey(w, &dwKey))
		{
			int ch;
			if (dwKey == VK_RETURN)
			{
				ExecLine();
				s_nIn = 0;
				s_szIn[0] = 0;
			}
			else if (dwKey == VK_BACK && s_nIn > 0)
				s_szIn[--s_nIn] = 0;
			else if (dwKey == VK_UP)
				s_nHist++;
			else if (dwKey == VK_DOWN && s_nHist > 0)
				s_nHist--;
			else if (dwKey == VK_NEXT || dwKey == VK_HOME)
				s_nHist = 0;
			else if ((ch = VkChar(dwKey)) && s_nIn < LINW - 8)
			{
				s_szIn[s_nIn++] = (WCHAR)ch;
				s_szIn[s_nIn] = 0;
			}
			nDirty = 1;
		}
		if (DCWinResized(w))
			nDirty = 1;
		if (nDirty)
		{
			DCWinBeginFrame(w);
			DCWinFillBg(w, RGB(255, 255, 255));
			nVis = (nCh - 4) / ROW_H;
			if (nVis < 1)
				nVis = 1;
			if (s_nHist > s_nLine - nVis + 1)
				s_nHist = s_nLine - nVis + 1;
			if (s_nHist < 0)
				s_nHist = 0;
			nFirst = s_nLine - nVis + 1 - s_nHist;
			if (nFirst < 0)
				nFirst = 0;
			y = 2;
			for (i = nFirst; i < s_nLine && y + ROW_H <= nCh; i++)
			{
				DCWinText(w, 3, y, RGB(0, 0, 0), RGB(255, 255, 255),
				          s_aLines[i % NSCROLL]);
				y += ROW_H;
			}
			if (y + ROW_H <= nCh)
			{
				WCHAR szPrompt[LINW];
				wsprintfW(szPrompt, L"%s>%s_", s_szCwd, s_szIn);
				DCWinText(w, 3, y, RGB(0, 0, 0), RGB(255, 255, 255), szPrompt);
			}
			DCWinEndFrame(w);
			nDirty = 0;
		}
		if (DCWinShouldClose(w))
			break;
		Sleep(80);
	}
	DCWinClose(w);
	return 0;
}
