//
// dcwlib.h - DCWin client library. An app links this to become a windowed program
// the dcshell compositor draws and routes input to. Typical use:
//
//     DCWin *w = DCWinOpen(x, y, cw, ch, L"Title");
//     for (;;) {
//         DWORD key;
//         while (DCWinPollKey(w, &key)) { ...handle key... }
//         DCWinBeginFrame(w);
//         DCWinFill(w, 0,0, cw,ch, RGB(192,192,192));
//         DCWinText(w, 8,8, RGB(0,0,0), RGB(192,192,192), L"hello");
//         DCWinEndFrame(w);
//         if (DCWinShouldClose(w)) break;
//         Sleep(33);
//     }
//     DCWinClose(w);
//
#ifndef DCWLIB_H
#define DCWLIB_H

#include "dcwin.h"

typedef struct DCWin DCWin;

DCWin *DCWinOpen(int x, int y, int w, int h, const WCHAR *title, int iconId); // NULL on failure
void DCWinBeginFrame(DCWin *win);
void DCWinFill(DCWin *win, int x, int y, int w, int h, COLORREF color);
void DCWinText(DCWin *win, int x, int y, COLORREF fg, COLORREF bg, const WCHAR *text);
void DCWinIcon(DCWin *win, int x, int y, int iconId);
void DCWinIconBig(DCWin *win, int x, int y, int iconId); // 32x32 cell
void DCWinEndFrame(DCWin *win);           // publishes the frame atomically
int DCWinPollKey(DCWin *win, DWORD *key); // returns 1 and a VK if one was queued
// Live analog-stick cursor over this window's client area. Returns 1 with client-relative x/y
// and btn (1 = primary button down) when the cursor is over this window; 0 when it isn't. Apps
// edge-detect btn themselves for clicks and hit-test their own controls.
int DCWinGetPointer(DCWin *win, int *x, int *y, int *btn);
int DCWinShouldClose(DCWin *win); // shell asked us to close
// Current CLIENT size. The shell can resize/maximize the window, so apps should read this
// each frame and lay out to fill it (fill the background to (cw,ch), stretch full-width
// elements) instead of using fixed open-time dimensions. Returns 1 if it changed since the
// last call (so the caller can mark itself dirty and republish). cw/ch may be NULL.
int DCWinClientSize(DCWin *win, int *cw, int *ch);
// Universal "redraw needed?": 1 if the window was resized since last frame (the lib's half of
// the dirty decision - OR it into your own change flag). Doesn't consume the size.
int DCWinResized(DCWin *win);
// Fill the WHOLE current client area with one colour (a DCWinFill(0,0,cw,ch,color) that always
// tracks the live size). The standard first call of a frame so the window fills on resize.
void DCWinFillBg(DCWin *win, COLORREF color);
void DCWinExec(DCWin *win, const WCHAR *path); // ask the shell to launch an app
void DCWinClose(DCWin *win);

// Crash handling: dcwlib provides WinMain (it wraps your entry in SEH and reports any unhandled
// exception to the shell, which raises a blue screen). Each client implements DcwMain INSTEAD of
// WinMain - same hInstance + command line, no WinMain boilerplate.
int DcwMain(HINSTANCE hInst, LPWSTR lpCmd);

#endif // DCWLIB_H
