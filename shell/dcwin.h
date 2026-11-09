//
// dcwin.h - DCWin compositor protocol, shared between the shell (server) and
// client apps. Lives in a named shared section ("DCWIN") that CE maps at the
// same virtual address in every process. Poll-based (no cross-process events):
// clients publish a draw-command list per window; the shell composites them and
// pushes input events back into each window's input ring.
//
#ifndef DCWIN_H
#define DCWIN_H

#include <windows.h>

#define DCWIN_SECTION L"DCWIN"
#define DCWIN_MAGIC   0x44435731 // 'DCW1'
#define DCWIN_MAXWIN  4
#define DCWIN_MAXCMD  192 // per-window draw-command budget (rich UIs: on-screen keyboards etc.)
#define DCWIN_MAXIN   16

#define DCOP_NONE     0
#define DCOP_FILL     1 // x,y,w,h + color
#define DCOP_TEXT     2 // x,y + color (fg) + color2 (bg) + text
#define DCOP_ICON     3 // x,y + color = icon id (ICON_* below)

// shell icon ids (shared so clients can reference them via DCOP_ICON / DCWinIcon)
#define ICON_COMPUTER 0
#define ICON_DRIVE    1
#define ICON_FOLDER   2
#define ICON_APP      3
#define ICON_CLOCK    4
#define ICON_FILE     5
#define ICON_SWIRL    6 // Dreamcast swirl (My Dreamcast)
#define ICON_CURSOR   7 // pointer arrow
#define ICON_IE       8 // Internet Explorer globe (IMGHTML image only)
#define ICON_START    9  // Windows CE flag (Start button)
#define ICON_BIN      10 // Recycle Bin
#define ICON_NET      11 // network connections
#define ICON_POWER    12 // power plug (Suspend / Shut Down)
#define ICON_PROGRAMS 13 // Start-menu Programs (window w/ app tiles)
#define ICON_FAVORITES 14 // Start-menu Favorites (folder w/ star)
#define ICON_DOCUMENTS 15 // Start-menu Documents (folder w/ pages)
#define ICON_SETTINGS 16 // Start-menu Settings (gear)
#define ICON_HELP     17 // Start-menu Help (book w/ ?)
#define ICON_RUN      18 // Start-menu Run... (hourglass + window)
#define ICON_SUSPEND  19 // Start-menu Suspend (PC w/ moon screen)
#define ICON_CPL      20 // Settings -> Control Panel (folder w/ tools)
#define ICON_TASKBAR  21 // Settings -> Taskbar (mini taskbar + flag)
#define ICON_EXPLORER 22 // Windows Explorer (window + magnifier)
#define ICON_CMD      23 // Command Prompt (console window)
#define ICON_COUNT    24

typedef struct
{
	DWORD op;
	LONG x, y, w, h;
	DWORD color;
	DWORD color2;
	WCHAR text[40];
} DcCmd;

typedef struct
{
	DWORD type; // 1 = key down
	DWORD key;  // VK code
} DcInput;

typedef struct
{
	DWORD inUse; // 0 = free slot
	DWORD ownerPid;
	LONG x, y, w, h; // client-area position + size on the desktop
	WCHAR title[40];
	DWORD icon;      // ICON_* for the title bar / taskbar
	DWORD wantClose; // shell -> client: please close
	DWORD gen;       // client bumps after writing a full frame of commands
	DWORD cmdCount;
	DcCmd cmd[DCWIN_MAXCMD];
	DWORD inHead; // shell writes here
	DWORD inTail; // client reads here
	DcInput in[DCWIN_MAXIN];
	LONG ptrX, ptrY; // shell -> client: analog-stick cursor in client coords (ptrX < 0 = not over)
	DWORD ptrBtn;    // shell -> client: primary button (stick click / A) down = 1
} DcWindow;

// A faulting client fills this (via dcwlib's SEH wrapper) before it dies; the shell edge-detects
// `seq` and raises a Win9x-style blue screen with the details. Appended at the END of DcShared so
// existing field offsets are unchanged.
typedef struct
{
	DWORD seq;      // bumped on each new crash (shell edge-detects)
	DWORD code;     // exception code (e.g. 0xC0000005 = access violation)
	DWORD addr;     // faulting instruction address
	DWORD access;   // access violation: 0 = read, 1 = write; 0xFFFFFFFF = n/a
	DWORD badAddr;  // address the faulting access touched (access violations only)
	WCHAR proc[32]; // faulting process (exe basename)
} DcCrash;

typedef struct
{
	DWORD magic;
	DWORD execSeq; // client bumps to ask the shell to launch execPath
	WCHAR execPath[260];
	DcWindow win[DCWIN_MAXWIN];
	DcCrash crash; // last client crash report (see DcCrash)
} DcShared;

#endif // DCWIN_H
