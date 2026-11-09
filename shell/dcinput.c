//
// dcinput.c - DirectInput keyboard + pointer (see dcinput.h).
//
// Keyboard: polled each frame, edge-detected (with auto-repeat for nav keys),
// DIK scan codes mapped to VK and queued for the shell's OnKey.
// Pointer (two sources, either drives the cursor):
//   - Mouse (DC Maple mouse / host-injected mouse): relative deltas, 1:1.
//   - Controller analog stick: deadzone + time-based speed + sub-pixel accumulate.
// Buttons (mouse L/M/R, or controller A/B/X/Y) -> click.
//
#define CINTERFACE
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <dinput.h>
#include "dcinput.h"

#define SCRW 640
#define SCRH 480

static LPDIRECTINPUT g_pDi = NULL;
static LPDIRECTINPUTDEVICE2 g_pKbd = NULL;
static LPDIRECTINPUTDEVICE2 g_pJoy = NULL;
static LPDIRECTINPUTDEVICE2 g_pMouse = NULL;
static HWND g_hwnd = NULL;
static int g_nWmPointer = 0;      // a GWES WM_MOUSE* arrived -> pointer exists
static HANDLE g_hNewDev = NULL;   // maple "MAPLE_NEW_DEVICE" hotplug event
static DWORD g_dwReEnumUntil = 0; // poll-rescan for devices until this tick

static BYTE g_aNow[256], g_aLast[256];
static DWORD g_aRepeatAt[256];
static int g_nCx = SCRW / 2, g_nCy = SCRH / 2;
static int g_nBtnLast = 0, g_nMbtnLast = 0, g_nClick = 0, g_nActivate = 0;
static int g_nLtHeld = 0;   // L trigger currently held (context-menu modifier)
static int g_nCtxClick = 0; // one-shot: LT+A or DC-mouse right-button -> open context menu
static int g_nCtxAHeld = 0; // edge tracking for the LT+A combo
static int g_nMHeld = 0,
           g_nJHeld = 0; // pointer button currently HELD (mouse-L / controller-A) for drag
static DWORD g_dwLastTick = 0;
static long g_lAccX = 0, g_lAccY = 0; // sub-pixel motion accumulators (axis*ms)

// DC controller buttons are identified by Maple HID USAGE (inc\maplusag.h), NOT by a fixed
// rgbButtons[] bit position - the array index depends on enumeration order and varies. So we
// build a usage->index map at acquire time via EnumObjects, exactly like the SDK
// samples\dinput\Controller. (The old hardcoded FACE_BITS/DPAD_BITS were wrong-order
// guesses, which is why A never worked on a real pad.)  usage = 0xFF00 + index:
#define USG_FIRST 0xFF00
#define USG_A     0
#define USG_B     1
#define USG_START 3
#define USG_LA    4 // D-pad left
#define USG_RA    5 // D-pad right
#define USG_DA    6 // D-pad down
#define USG_UA    7 // D-pad up
#define USG_X     8
#define USG_Y     9
#define USG_LTRIG 0x11 // L trigger (maple USAGE_LTRIG_BUTTON 0xFF11) - enumerated as a button
#define USG_N     24
static signed char g_abBtnIdx[USG_N]; // Maple usage index -> rgbButtons[] index (-1 absent)
static int g_nBtnEnum;                // running button count while enumerating
static const DWORD s_adwDpadVk[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT}; // matches dn[] order below
// 1 if the button with Maple usage 'u' is pressed in joystate 'js'.
#define JBTN(js, u) ((g_abBtnIdx[u] >= 0 && ((js).rgbButtons[g_abBtnIdx[u]] & 0x80)) ? 1 : 0)
static int g_aDpadHeld[4];
static DWORD g_aDpadRepeatAt[4];
// Raw face/Start button edges for DInButtonEdge (indices match DIN_BTN_* in dcinput.h).
static int g_aBeEdge[5], g_aBeHeld[5];
static const int s_aBeUsg[5] = {USG_A, USG_B, USG_X, USG_Y, USG_START};
static int g_nMousePrimed = 0;      // skip edge events on first read
static DWORD g_dwJoyPrimeUntil = 0; // joy: track baseline (no edges) until this tick
#define JOY_PRIME_MS 400            // startup settle window for the controller

#define STICK_DZ     150  // analog deadzone (range is +-1000)
#define STICK_DIV    1250 // px = axis * dt_ms / DIV; full deflection ~= 800 px/sec

static DWORD g_aQ[32];
static int g_nQh = 0, g_nQt = 0;

static void Push(DWORD dwVk)
{
	int n = (g_nQh + 1) & 31;
	if (n != g_nQt)
	{
		g_aQ[g_nQh] = dwVk;
		g_nQh = n;
	}
}

// Win32 OEM VKs for the punctuation keys; CE 2.12's winuser.h doesn't ship them.
#ifndef VK_OEM_1
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE
#endif

static const struct
{
	int dik;
	DWORD vk;
	int nav;
} g_aMap[] = {
    {DIK_UP, VK_UP, 1},
    {DIK_DOWN, VK_DOWN, 1},
    {DIK_LEFT, VK_LEFT, 1},
    {DIK_RIGHT, VK_RIGHT, 1},
    {DIK_RETURN, VK_RETURN, 0},
    {DIK_ESCAPE, VK_ESCAPE, 0},
    {DIK_TAB, VK_TAB, 0},
    {DIK_BACK, VK_BACK, 1},
    {DIK_DELETE, VK_DELETE, 0},
    {DIK_0, '0', 0},
    {DIK_1, '1', 0},
    {DIK_2, '2', 0},
    {DIK_3, '3', 0},
    {DIK_4, '4', 0},
    {DIK_5, '5', 0},
    {DIK_6, '6', 0},
    {DIK_7, '7', 0},
    {DIK_8, '8', 0},
    {DIK_9, '9', 0},
    // full US alpha + punctuation, so shell dialogs (Run...) can take typed input
    {DIK_A, 'A', 0},
    {DIK_B, 'B', 0},
    {DIK_C, 'C', 0},
    {DIK_D, 'D', 0},
    {DIK_E, 'E', 0},
    {DIK_F, 'F', 0},
    {DIK_G, 'G', 0},
    {DIK_H, 'H', 0},
    {DIK_I, 'I', 0},
    {DIK_J, 'J', 0},
    {DIK_K, 'K', 0},
    {DIK_L, 'L', 0},
    {DIK_M, 'M', 0},
    {DIK_N, 'N', 0},
    {DIK_O, 'O', 0},
    {DIK_P, 'P', 0},
    {DIK_Q, 'Q', 0},
    {DIK_R, 'R', 0},
    {DIK_S, 'S', 0},
    {DIK_T, 'T', 0},
    {DIK_U, 'U', 0},
    {DIK_V, 'V', 0},
    {DIK_W, 'W', 0},
    {DIK_X, 'X', 0},
    {DIK_Y, 'Y', 0},
    {DIK_Z, 'Z', 0},
    {DIK_SPACE, VK_SPACE, 0},
    {DIK_MINUS, VK_OEM_MINUS, 0},
    {DIK_EQUALS, VK_OEM_PLUS, 0},
    {DIK_LBRACKET, VK_OEM_4, 0},
    {DIK_RBRACKET, VK_OEM_6, 0},
    {DIK_BACKSLASH, VK_OEM_5, 0},
    {DIK_SEMICOLON, VK_OEM_1, 0},
    {DIK_APOSTROPHE, VK_OEM_7, 0},
    {DIK_GRAVE, VK_OEM_3, 0},
    {DIK_COMMA, VK_OEM_COMMA, 0},
    {DIK_PERIOD, VK_OEM_PERIOD, 0}, // NOT '.': VK_DELETE is also 0x2E
    {DIK_SLASH, VK_OEM_2, 0},
};
#define NMAP (sizeof(g_aMap) / sizeof(g_aMap[0]))

// EnumObjects callback: record each button's rgbButtons[] index (its ordinal among buttons)
// keyed by Maple HID usage, so we can read A/B/X/Y/Start/D-pad regardless of pad layout.
static BOOL CALLBACK JoyObjCb(LPCDIDEVICEOBJECTINSTANCE pObj, LPVOID pvCtx)
{
	(void)pvCtx;
	if (LOBYTE(LOWORD(pObj->dwType)) & DIDFT_BUTTON)
	{
		int u = (int)pObj->wUsage - USG_FIRST;
		if (u >= 0 && u < USG_N)
			g_abBtnIdx[u] = (signed char)g_nBtnEnum;
		g_nBtnEnum++;
	}
	return DIENUM_CONTINUE;
}

static BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCE pDi, LPVOID pvCtx)
{
	LPDIRECTINPUTDEVICE pD1 = NULL;
	LPDIRECTINPUTDEVICE2 pD2 = NULL;
	DWORD dwType = GET_DIDEVICE_TYPE(pDi->dwDevType);
	(void)pvCtx;

	// Log each device DInput reports (before CreateDevice) - but only types we don't yet
	// have, so the 1s hotplug re-scan doesn't spam; we still see a mouse the moment it appears.
	{
		int nHave = (dwType == DIDEVTYPE_KEYBOARD && g_pKbd) ||
		            (dwType == DIDEVTYPE_MOUSE && g_pMouse) ||
		            (dwType == DIDEVTYPE_JOYSTICK && g_pJoy);
		if (!nHave)
		{
			WCHAR b[96];
			wsprintfW(b, L"DCIN: enum type=%u (2=mouse 3=kbd 4=joy) dwDevType=%08x\r\n",
			          (unsigned)dwType, (unsigned)pDi->dwDevType);
			OutputDebugStringW(b);
		}
	}

	if (IDirectInput_CreateDevice(g_pDi, &pDi->guidInstance, &pD1, NULL) != DI_OK)
	{
		OutputDebugStringW(L"DCIN:   CreateDevice FAILED\r\n");
		return DIENUM_CONTINUE;
	}
	if (IDirectInputDevice_QueryInterface(pD1, &IID_IDirectInputDevice2, (LPVOID *)&pD2) != S_OK)
	{
		OutputDebugStringW(L"DCIN:   QueryInterface(IDirectInputDevice2) FAILED\r\n");
		IDirectInputDevice_Release(pD1);
		return DIENUM_CONTINUE;
	}
	IDirectInputDevice_Release(pD1);
	if (dwType == DIDEVTYPE_KEYBOARD && !g_pKbd)
	{
		IDirectInputDevice2_SetDataFormat(pD2, &c_dfDIKeyboard);
		IDirectInputDevice2_Acquire(pD2);
		g_pKbd = pD2;
		OutputDebugStringW(L"DCIN: keyboard acquired\r\n");
	}
	else if (dwType == DIDEVTYPE_MOUSE && !g_pMouse)
	{
		HRESULT hr;
		WCHAR b[64];
		// DC/CE mouse: SetDataFormat + Acquire ONLY. The SDK (samples\misc\DesktopCompat)
		// wraps SetCooperativeLevel in #ifndef UNDER_CE - it is NOT set on the Dreamcast.
		// Our previous SetCooperativeLevel(DISCL_BACKGROUND) made the mouse deliver no data
		// (c_dfDIMouse is relative-axis by default, which is what we want for a pointer).
		IDirectInputDevice2_SetDataFormat(pD2, &c_dfDIMouse);
		hr = IDirectInputDevice2_Acquire(pD2);
		g_pMouse = pD2;
		wsprintfW(b, L"DCIN: mouse acquired (hr=%08x)\r\n", (unsigned)hr);
		OutputDebugStringW(b);
	}
	else if (dwType == DIDEVTYPE_JOYSTICK && !g_pJoy)
	{
		DIPROPRANGE r;
		int i;
		// Build the Maple usage -> rgbButtons[] index map BEFORE SetDataFormat (SDK order):
		// EnumObjects visits each button; its ordinal among buttons is its rgbButtons[] index.
		for (i = 0; i < USG_N; i++)
			g_abBtnIdx[i] = -1;
		g_nBtnEnum = 0;
		IDirectInputDevice2_EnumObjects(pD2, JoyObjCb, NULL, DIDFT_BUTTON);
		IDirectInputDevice2_SetDataFormat(pD2, &c_dfDIJoystick);
		memset(&r, 0, sizeof(r));
		r.diph.dwSize = sizeof(DIPROPRANGE);
		r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		r.diph.dwHow = DIPH_DEVICE;
		r.lMin = -1000;
		r.lMax = 1000;
		IDirectInputDevice2_SetProperty(pD2, DIPROP_RANGE, &r.diph);
		IDirectInputDevice2_Acquire(pD2);
		g_pJoy = pD2;
		{
			WCHAR b[96];
			wsprintfW(b, L"DCIN: joystick acquired, %d buttons (A@%d B@%d X@%d Start@%d)\r\n",
			          g_nBtnEnum, g_abBtnIdx[USG_A], g_abBtnIdx[USG_B], g_abBtnIdx[USG_X],
			          g_abBtnIdx[USG_START]);
			OutputDebugStringW(b);
		}
	}
	else
	{
		IDirectInputDevice2_Release(pD2);
	}
	return DIENUM_CONTINUE;
}

BOOL DInInit(HWND hwnd)
{
	g_hwnd = hwnd;
	if (DirectInputCreate(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &g_pDi, NULL) != DI_OK)
	{
		OutputDebugStringW(L"DCIN: DirectInputCreate FAILED\r\n");
		return FALSE;
	}
	// Maple hotplug: a mouse/controller on a later port often registers with DInput a moment
	// AFTER boot, so the initial enum may only see the port-A keyboard. The SDK re-enumerates
	// when the driver signals "MAPLE_NEW_DEVICE"; we also poll-rescan for a few seconds.
	g_hNewDev = CreateEventW(NULL, FALSE, FALSE, L"MAPLE_NEW_DEVICE");
	g_dwReEnumUntil = GetTickCount() + 10000;
	// Flags = 0 (DIEDFL_ALLDEVICES), NOT DIEDFL_ATTACHEDONLY (the SDK uses 0; ATTACHEDONLY
	// dropped the mouse). The maple driver only reports actually-present devices.
	IDirectInput_EnumDevices(g_pDi, 0, EnumCb, NULL, 0);
	{
		WCHAR b[80];
		wsprintfW(b, L"DCIN: found kbd=%d mouse=%d joy=%d\r\n", g_pKbd ? 1 : 0, g_pMouse ? 1 : 0,
		          g_pJoy ? 1 : 0);
		OutputDebugStringW(b);
	}
	g_dwJoyPrimeUntil =
	    GetTickCount() + JOY_PRIME_MS; // settle the controller before edge-detecting
	return (g_pKbd != NULL);
}

void DInShutdown(void)
{
	if (g_pKbd)
	{
		IDirectInputDevice2_Unacquire(g_pKbd);
		IDirectInputDevice2_Release(g_pKbd);
		g_pKbd = NULL;
	}
	if (g_pMouse)
	{
		IDirectInputDevice2_Unacquire(g_pMouse);
		IDirectInputDevice2_Release(g_pMouse);
		g_pMouse = NULL;
	}
	if (g_pJoy)
	{
		IDirectInputDevice2_Unacquire(g_pJoy);
		IDirectInputDevice2_Release(g_pJoy);
		g_pJoy = NULL;
	}
	if (g_pDi)
	{
		IDirectInput_Release(g_pDi);
		g_pDi = NULL;
	}
}

// Hand the input devices to a launched full-screen app (e.g. a DirectInput game):
// drop OUR acquisition so the app's Acquire (often EXCLUSIVE) succeeds. We keep the
// device objects so we can re-Acquire when the app exits (DInReacquire).
void DInRelease(void)
{
	if (g_pKbd)
		IDirectInputDevice2_Unacquire(g_pKbd);
	if (g_pMouse)
		IDirectInputDevice2_Unacquire(g_pMouse);
	if (g_pJoy)
		IDirectInputDevice2_Unacquire(g_pJoy);
	OutputDebugStringW(L"DCIN: released input to app\r\n");
}

// Hand off to a fullscreen app but KEEP the keyboard + controller acquired so the shell
// can still poll the panic combos (ALT+F4 / CTRL+ALT+DEL / START+A) while the app runs.
// Only the pointer (mouse) is released. DC games read the controller off the Maple bus
// directly (not DirectInput), so our non-exclusive hold doesn't starve them; our own apps
// (e.g. the browser) acquire non-exclusively too. DInReacquire restores the mouse.
void DInHandoffToApp(void)
{
	if (g_pMouse)
		IDirectInputDevice2_Unacquire(g_pMouse);
	OutputDebugStringW(L"DCIN: handoff (kept keyboard+controller for hotkeys)\r\n");
}

// App exited: take input back. Re-prime edge detection + clear the key snapshot so a
// key/button still held at hand-back doesn't fire a phantom press into the shell.
void DInReacquire(void)
{
	if (g_pKbd)
		IDirectInputDevice2_Acquire(g_pKbd);
	if (g_pMouse)
		IDirectInputDevice2_Acquire(g_pMouse);
	if (g_pJoy)
		IDirectInputDevice2_Acquire(g_pJoy);
	memset(g_aNow, 0, sizeof(g_aNow));
	memset(g_aLast, 0, sizeof(g_aLast));
	g_dwJoyPrimeUntil = GetTickCount() + JOY_PRIME_MS;
	g_nMousePrimed = 0;
	g_nQh = g_nQt = 0; // drop any queued keys from before the hand-off
	memset(g_aBeEdge, 0, sizeof(g_aBeEdge));
	memset(g_aBeHeld, 0, sizeof(g_aBeHeld));
	g_nCtxClick = 0;
	g_nCtxAHeld = 0;
	g_nLtHeld = 0;
	OutputDebugStringW(L"DCIN: reacquired input\r\n");
}

//
// Global hotkey state (ALT+F4, CTRL+ALT+DEL). The modifier keys and F4 are not in
// the VK translation table, so read the raw 256-key array straight off the keyboard
// device. Self-contained (own GetDeviceState) so a caller can poll it standalone,
// not only from the per-frame DInUpdate path. Returns the combo currently held; the
// caller is responsible for edge-detecting (fire once per press).
//
int DInHotkey(void)
{
	BYTE abKeys[256];
	int bAlt, bCtrl;

	if (!g_pKbd)
		return DIN_HK_NONE;
	if (IDirectInputDevice2_GetDeviceState(g_pKbd, 256, abKeys) != DI_OK)
	{
		IDirectInputDevice2_Acquire(
		    g_pKbd); // lost (e.g. an app grabbed it) - reprime, skip this tick
		return DIN_HK_NONE;
	}
	bAlt = (abKeys[DIK_LMENU] | abKeys[DIK_RMENU]) & 0x80;
	bCtrl = (abKeys[DIK_LCONTROL] | abKeys[DIK_RCONTROL]) & 0x80;
	if (bCtrl && bAlt && (abKeys[DIK_DELETE] & 0x80))
		return DIN_HK_CTRLALTDEL;
	if (bAlt && (abKeys[DIK_F4] & 0x80))
		return DIN_HK_ALTF4;
	return DIN_HK_NONE;
}

//
// Controller panic combo: START + A. Self-contained joystick read (own Poll +
// GetDeviceState) so it can be polled during a fullscreen hand-off, when the per-frame
// DInUpdate loop isn't running. Returns 1 while both buttons are physically held; the
// caller edge-detects. This is the keyboard-less analogue of ALT+F4 (kill the app).
//
int DInPadCombo(void)
{
	DIJOYSTATE js;

	if (!g_pJoy)
		return 0;
	IDirectInputDevice2_Poll(g_pJoy);
	if (IDirectInputDevice2_GetDeviceState(g_pJoy, sizeof(js), &js) != DI_OK)
	{
		IDirectInputDevice2_Acquire(g_pJoy); // lost - reprime, skip this tick
		return 0;
	}
	return (JBTN(js, USG_START) && JBTN(js, USG_A)) ? 1 : 0;
}

void DInUpdate(void)
{
	DWORD dwNowt = GetTickCount();
	int i;

	// Hotplug re-scan: pick up a mouse/controller that registered after the initial enum.
	// Trigger on the maple MAPLE_NEW_DEVICE event, and poll-rescan every 1s for the first 10s.
	{
		static DWORD dwLastScan = 0;
		int nEvt = (g_hNewDev && WaitForSingleObject(g_hNewDev, 0) != WAIT_TIMEOUT);
		if (g_pDi && (nEvt || (dwNowt < g_dwReEnumUntil && dwNowt - dwLastScan >= 1000)))
		{
			dwLastScan = dwNowt;
			IDirectInput_EnumDevices(g_pDi, 0, EnumCb, NULL, 0);
		}
	}

	if (g_pKbd)
	{
		for (i = 0; i < 256; i++)
			g_aLast[i] = g_aNow[i];
		IDirectInputDevice2_Poll(g_pKbd);
		if (IDirectInputDevice2_GetDeviceState(g_pKbd, 256, g_aNow) != DI_OK)
			IDirectInputDevice2_Acquire(g_pKbd);
		else
		{
			for (i = 0; i < (int)NMAP; i++)
			{
				int dik = g_aMap[i].dik;
				BOOL down = (g_aNow[dik] & 0x80) != 0;
				BOOL was = (g_aLast[dik] & 0x80) != 0;
				if (down && !was)
				{
					Push(g_aMap[i].vk);
					g_aRepeatAt[dik] = dwNowt + 350;
				}
				else if (down && was && g_aMap[i].nav && dwNowt >= g_aRepeatAt[dik])
				{
					Push(g_aMap[i].vk);
					g_aRepeatAt[dik] = dwNowt + 100;
				}
			}
		}
	}

	if (g_pMouse)
	{
		DIMOUSESTATE ms;
		IDirectInputDevice2_Poll(g_pMouse);
		if (IDirectInputDevice2_GetDeviceState(g_pMouse, sizeof(ms), &ms) != DI_OK)
			IDirectInputDevice2_Acquire(g_pMouse);
		else
		{
			int nBtn;
			g_nCx += ms.lX; // relative deltas, 1:1 (snappy)
			g_nCy += ms.lY;
			if (g_nCx < 0)
				g_nCx = 0;
			if (g_nCx >= SCRW)
				g_nCx = SCRW - 1;
			if (g_nCy < 0)
				g_nCy = 0;
			if (g_nCy >= SCRH)
				g_nCy = SCRH - 1;
			nBtn = (ms.rgbButtons[0] | ms.rgbButtons[2]) & 0x80; // left + middle drive the pointer
			// DI mouse drives the pointer button as a HELD state -> the shell's press/drag/
			// release model handles click vs drag. (g_nClick stays for the GWES WM-tap path.)
			g_nMHeld = nBtn ? 1 : 0;
			{
				int r = ms.rgbButtons[1] & 0x80; // right button -> context menu (edge)
				if (r && !g_nMbtnLast)
					g_nCtxClick = 1;
				g_nMbtnLast = r ? 1 : 0;
			}
			(void)g_nMousePrimed;
		}
	}

	if (g_pJoy)
	{
		DIJOYSTATE js;
		IDirectInputDevice2_Poll(g_pJoy);
		if (IDirectInputDevice2_GetDeviceState(g_pJoy, sizeof(js), &js) != DI_OK)
			IDirectInputDevice2_Acquire(g_pJoy);
		else
		{
			DWORD dwDt = g_dwLastTick ? (dwNowt - g_dwLastTick) : 16;
			int nAx = js.lX < 0 ? -js.lX : js.lX;
			int nAy = js.lY < 0 ? -js.lY : js.lY;
			int i, dx, dy, face, dn[4];
			int bLt = JBTN(js, USG_LTRIG); // L trigger = context-menu modifier
			g_nLtHeld = bLt;
			if (dwDt > 100)
				dwDt = 100; // clamp after a stall so the cursor doesn't leap
			// analog stick -> cursor: time-based + sub-pixel accumulator (speed is
			// frame-rate independent AND small deflections still move)
			if (nAx > STICK_DZ)
			{
				g_lAccX += (long)js.lX * (long)dwDt;
				dx = (int)(g_lAccX / STICK_DIV);
				g_lAccX -= (long)dx * STICK_DIV;
				g_nCx += dx;
			}
			else
				g_lAccX = 0;
			if (nAy > STICK_DZ)
			{
				g_lAccY += (long)js.lY * (long)dwDt;
				dy = (int)(g_lAccY / STICK_DIV);
				g_lAccY -= (long)dy * STICK_DIV;
				g_nCy += dy;
			}
			else
				g_lAccY = 0;
			if (g_nCx < 0)
				g_nCx = 0;
			if (g_nCx >= SCRW)
				g_nCx = SCRW - 1;
			if (g_nCy < 0)
				g_nCy = 0;
			if (g_nCy >= SCRH)
				g_nCy = SCRH - 1;

			// Buttons by Maple usage (the proper, layout-independent way). D-pad U/D/L/R,
			// and "activate" = A or Start.
			dn[0] = JBTN(js, USG_UA);
			dn[1] = JBTN(js, USG_DA);
			dn[2] = JBTN(js, USG_LA);
			dn[3] = JBTN(js, USG_RA);
			// While LT is held, A is the context-menu chord (handled below), NOT activate/drag.
			face = (!bLt && (JBTN(js, USG_A) || JBTN(js, USG_START))) ? 1 : 0;
			g_nJHeld = (dwNowt < g_dwJoyPrimeUntil || bLt)
			               ? 0
			               : JBTN(js, USG_A); // A held (drag), post-settle
			if (dwNowt < g_dwJoyPrimeUntil)
			{
				// STARTUP SETTLE WINDOW: just track the baseline, generate NO edges (lets the
				// device's transient first reads settle so they can't fire a phantom activate).
				for (i = 0; i < 4; i++)
					g_aDpadHeld[i] = dn[i];
				g_nBtnLast = face;
				{
					int k;
					for (k = 0; k < 5; k++)
						g_aBeHeld[k] = JBTN(js, s_aBeUsg[k]);
				}
				g_nCtxAHeld = (bLt && JBTN(js, USG_A)) ? 1 : 0; // baseline, no edge during settle
			}
			else
			{
				// D-pad -> arrow keys, per-direction edge + auto-repeat
				for (i = 0; i < 4; i++)
				{
					if (dn[i] && !g_aDpadHeld[i])
					{
						Push(s_adwDpadVk[i]);
						g_aDpadRepeatAt[i] = dwNowt + 350;
					}
					else if (dn[i] && dwNowt >= g_aDpadRepeatAt[i])
					{
						Push(s_adwDpadVk[i]);
						g_aDpadRepeatAt[i] = dwNowt + 120;
					}
					g_aDpadHeld[i] = dn[i];
				}
				// A / Start -> "activate" (D-pad selects, A opens the selection)
				if (face && !g_nBtnLast)
					g_nActivate = 1;
				g_nBtnLast = face;
				// Raw per-button edges for DInButtonEdge (browser etc.); independent of activate.
				{
					int k;
					for (k = 0; k < 5; k++)
					{
						int d = JBTN(js, s_aBeUsg[k]);
						if (d && !g_aBeHeld[k])
							g_aBeEdge[k] = 1;
						g_aBeHeld[k] = d;
					}
				}
				// LT + A chord -> context menu (right-click). One-shot edge; LT already
				// suppressed A's activate/pointer above, so this can't double as a click.
				{
					int bA = JBTN(js, USG_A);
					if (bLt && bA && !g_nCtxAHeld)
						g_nCtxClick = 1;
					g_nCtxAHeld = (bLt && bA) ? 1 : 0;
				}
			}
		}
	}
	g_dwLastTick = dwNowt;
}

int DInNextKey(DWORD *vk)
{
	if (g_nQt == g_nQh)
		return 0;
	*vk = g_aQ[g_nQt];
	g_nQt = (g_nQt + 1) & 31;
	return 1;
}

// GWES window-message fallback: the DC mouse may reach us via WM_MOUSE* instead
// of DirectInput. The shell forwards those here so all paths share one cursor.
void DInSetCursor(int x, int y)
{
	if (x < 0)
		x = 0;
	if (x >= SCRW)
		x = SCRW - 1;
	if (y < 0)
		y = 0;
	if (y >= SCRH)
		y = SCRH - 1;
	g_nCx = x;
	g_nCy = y;
	g_nWmPointer = 1;
}
void DInPostClick(void)
{
	g_nClick = 1;
}

int DInTookActivate(void)
{
	int a = g_nActivate;
	g_nActivate = 0;
	return a;
} // controller A -> Enter
int DInHasPointer(void)
{
	return (g_pMouse != NULL) || (g_pJoy != NULL) || g_nWmPointer;
}
void DInCursor(int *x, int *y)
{
	*x = g_nCx;
	*y = g_nCy;
}
int DInTookClick(void)
{
	int c = g_nClick;
	g_nClick = 0;
	return c;
}
int DInPointerDown(void)
{
	return g_nMHeld || g_nJHeld;
} // pointer button currently HELD (drag)
int DInTookContext(void)
{
	int c = g_nCtxClick;
	g_nCtxClick = 0;
	return c;
} // one-shot: LT+A (controller) or right-button (DC mouse) -> open the context menu
int DInLeftTrigDown(void)
{
	return g_nLtHeld;
} // L trigger held (the context-menu modifier)
int DInButtonEdge(int btn)
{
	int e;
	if (btn < 0 || btn > 4)
		return 0;
	e = g_aBeEdge[btn];
	g_aBeEdge[btn] = 0;
	return e;
}
