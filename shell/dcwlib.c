//
// dcwlib.c - DCWin client library (see dcwlib.h). Maps the shared section, claims
// a window slot, and marshals draw commands / input. One window per client.
//
#include "dcwlib.h"

struct DCWin
{
	HANDLE hMap;
	DcShared *sh;
	DcWindow *w;
	DcCmd build[DCWIN_MAXCMD];
	int buildN;
	int lastW, lastH; // last size seen by DCWinClientSize (change detection)
};

static struct DCWin g_win; // single window per client process

DCWin *DCWinOpen(int x, int y, int w, int h, const WCHAR *title, int iconId)
{
	int i, nIdx = -1, nTries;

	g_win.hMap =
	    CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(DcShared), DCWIN_SECTION);
	if (!g_win.hMap)
		return NULL;
	g_win.sh = (DcShared *)MapViewOfFile(g_win.hMap, FILE_MAP_WRITE, 0, 0, sizeof(DcShared));
	if (!g_win.sh)
		return NULL;

	for (nTries = 0; nTries < 50 && g_win.sh->magic != DCWIN_MAGIC; nTries++)
		Sleep(20); // wait for the shell to initialise the section
	if (g_win.sh->magic != DCWIN_MAGIC)
		return NULL;

	for (i = 0; i < DCWIN_MAXWIN; i++)
		if (!g_win.sh->win[i].inUse)
		{
			nIdx = i;
			break;
		}
	if (nIdx < 0)
		return NULL;

	g_win.w = &g_win.sh->win[nIdx];
	memset(g_win.w, 0, sizeof(DcWindow));
	g_win.w->ownerPid = GetCurrentProcessId();
	g_win.w->x = x;
	g_win.w->y = y;
	g_win.w->w = w;
	g_win.w->h = h;
	lstrcpyW(g_win.w->title, title);
	g_win.w->icon = (DWORD)iconId;
	g_win.w->cmdCount = 0;
	g_win.w->gen = 0; // even = stable (seqlock)
	g_win.buildN = 0;
	g_win.lastW = w;
	g_win.lastH = h;
	g_win.w->inUse = 1; // publish only once fully initialised
	return &g_win;
}

// Current client size (the shell may have resized us). Returns 1 if it changed since last call.
int DCWinClientSize(DCWin *pWin, int *cw, int *ch)
{
	int w = (int)pWin->w->w, h = (int)pWin->w->h, bChanged;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	bChanged = (w != pWin->lastW || h != pWin->lastH);
	pWin->lastW = w;
	pWin->lastH = h;
	if (cw)
		*cw = w;
	if (ch)
		*ch = h;
	return bChanged;
}

// 1 if the shell resized us since last call (consumes the change). OR into your dirty flag.
int DCWinResized(DCWin *pWin)
{
	return DCWinClientSize(pWin, 0, 0);
}

// Fill the whole current client area with one colour - the standard first draw of a frame, so
// the window always fills after a resize/maximize without the app tracking dimensions.
void DCWinFillBg(DCWin *pWin, COLORREF color)
{
	int w = (int)pWin->w->w, h = (int)pWin->w->h;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	DCWinFill(pWin, 0, 0, w, h, color);
}

void DCWinBeginFrame(DCWin *pWin)
{
	pWin->buildN = 0;
}

void DCWinFill(DCWin *pWin, int x, int y, int w, int h, COLORREF color)
{
	DcCmd *pCmd;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_FILL;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->w = w;
	pCmd->h = h;
	pCmd->color = color;
}

void DCWinText(DCWin *pWin, int x, int y, COLORREF fg, COLORREF bg, const WCHAR *text)
{
	DcCmd *pCmd;
	int k;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_TEXT;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->color = fg;
	pCmd->color2 = bg;
	for (k = 0; k < 39 && text[k]; k++)
		pCmd->text[k] = text[k];
	pCmd->text[k] = 0;
}

void DCWinIcon(DCWin *pWin, int x, int y, int iconId)
{
	DcCmd *pCmd;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_ICON;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->color = (DWORD)iconId;
}

void DCWinIconBig(DCWin *pWin, int x, int y, int iconId)
{
	DcCmd *pCmd;
	if (pWin->buildN >= DCWIN_MAXCMD)
		return;
	pCmd = &pWin->build[pWin->buildN++];
	pCmd->op = DCOP_ICONBIG;
	pCmd->x = x;
	pCmd->y = y;
	pCmd->color = (DWORD)iconId;
}

void DCWinEndFrame(DCWin *pWin)
{
	int i;
	pWin->w->gen++; // odd = writing (seqlock)
	for (i = 0; i < pWin->buildN; i++)
		pWin->w->cmd[i] = pWin->build[i];
	pWin->w->cmdCount = pWin->buildN;
	pWin->w->gen++; // even = stable
}

int DCWinPollKey(DCWin *pWin, DWORD *key)
{
	if (pWin->w->inTail == pWin->w->inHead)
		return 0;
	*key = pWin->w->in[pWin->w->inTail % DCWIN_MAXIN].key;
	pWin->w->inTail++;
	return 1;
}

int DCWinGetPointer(DCWin *pWin, int *x, int *y, int *btn)
{
	if (pWin->w->ptrX < 0)
		return 0; // analog-stick cursor not over this window
	if (x)
		*x = (int)pWin->w->ptrX;
	if (y)
		*y = (int)pWin->w->ptrY;
	if (btn)
		*btn = (int)pWin->w->ptrBtn;
	return 1;
}

int DCWinShouldClose(DCWin *pWin)
{
	return pWin->w->wantClose != 0;
}

void DCWinExec(DCWin *pWin, const WCHAR *path)
{
	lstrcpyW(pWin->sh->execPath, path);
	pWin->sh->execSeq++; // shell polls execSeq and launches execPath
}

void DCWinClose(DCWin *pWin)
{
	pWin->w->inUse = 0;
	if (pWin->sh)
		UnmapViewOfFile(pWin->sh);
	if (pWin->hMap)
		CloseHandle(pWin->hMap);
}

// SEH filter: record an unhandled exception into the shared DcCrash so the shell can raise a blue
// screen, then unwind (EXCEPTION_EXECUTE_HANDLER) so this process exits cleanly + gets reaped.
static LONG DCWinCrashFilter(EXCEPTION_POINTERS *pep)
{
	DcShared *sh = g_win.sh;
	HANDLE hm = NULL;
	if (!sh) // crashed before DCWinOpen mapped the section - map a best-effort view
	{
		hm = CreateFileMappingW((HANDLE)-1, NULL, PAGE_READWRITE, 0, sizeof(DcShared),
		                        DCWIN_SECTION);
		if (hm)
			sh = (DcShared *)MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, sizeof(DcShared));
	}
	OutputDebugStringW(L"DCWLIB: crash filter entered\r\n");
	if (sh && pep && pep->ExceptionRecord)
	{
		EXCEPTION_RECORD *er = pep->ExceptionRecord;
		WCHAR szPath[MAX_PATH], achDbg[80];
		const WCHAR *pszBase = L"app", *p;
		int i;
		wsprintfW(achDbg, L"DCWLIB: exception %08X at %08X\r\n", (unsigned)er->ExceptionCode,
		          (unsigned)er->ExceptionAddress);
		OutputDebugStringW(achDbg);
		sh->crash.code = er->ExceptionCode;
		sh->crash.addr = (DWORD)er->ExceptionAddress;
		sh->crash.access = 0xFFFFFFFF;
		sh->crash.badAddr = 0;
		if (er->ExceptionCode == STATUS_ACCESS_VIOLATION && er->NumberParameters >= 2)
		{
			sh->crash.access = er->ExceptionInformation[0]; // 0 = read, 1 = write
			sh->crash.badAddr = (DWORD)er->ExceptionInformation[1];
		}
		if (GetModuleFileNameW(NULL, szPath, MAX_PATH))
			for (p = pszBase = szPath; *p; p++)
				if (*p == L'\\' || *p == L'/')
					pszBase = p + 1;
		for (i = 0; i < 31 && pszBase[i]; i++)
			sh->crash.proc[i] = pszBase[i];
		sh->crash.proc[i] = 0;
		sh->crash.seq++; // signal LAST, after the fields are filled
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

// dcwlib owns WinMain; clients implement DcwMain. Wrapping the whole app in one __try is how we
// catch faults (CE 2.12 has no SetUnhandledExceptionFilter, but SEH catches SH-4 hardware faults).
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
	int rc = 1;
	(void)hPrev;
	(void)nShow;
	__try
	{
		rc = DcwMain(hInst, lpCmd);
	}
	__except (DCWinCrashFilter(GetExceptionInformation()))
	{
		rc = 1; // crash reported to the shell; exit so it can reap us + show the BSOD
	}
	return rc;
}
