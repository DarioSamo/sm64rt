#if defined(RAPI_RT64) && defined(BETTERCAMERA)

#include "controller_rt64.h"
#include "controller_mouse.h"
#include "pc/gfx/gfx_rt64_context.h"

#include <cassert>

#ifndef HID_USAGE_PAGE_GENERIC
#   define HID_USAGE_PAGE_GENERIC         ((unsigned short) 0x01)
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#   define HID_USAGE_GENERIC_MOUSE        ((unsigned short) 0x02)
#endif

#if !defined(CAPI_SDL2) && !defined(CAPI_SDL1)
int mouse_x = 0;
int mouse_y = 0;
#endif

extern u8 newcam_mouse;

static void controller_rt64_api_init(void) {
    assert(RT64.hwnd != NULL);

    // Register mouse as raw device for WM_INPUT.
    RAWINPUTDEVICE Rid[1];
	Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC; 
	Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE; 
	Rid[0].dwFlags = RIDEV_INPUTSINK;   
	Rid[0].hwndTarget = RT64.hwnd;
	RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));
}

static void controller_rt64_api_read(OSContPad *pad) {
    RT64.mouselookEnabled = (newcam_mouse == 1);

    if (RT64.mouselookEnabled && RT64.windowActive) {
        mouse_x = RT64.deltaMouseX;
        mouse_y = RT64.deltaMouseY;

        // Clip the cursor to the window rectangle.
        RECT windowRect;
        GetWindowRect(RT64.hwnd, &windowRect);
        ClipCursor(&windowRect);
    }
    else {
        mouse_x = 0;
        mouse_y = 0;
        ClipCursor(NULL);
    }

    RT64.deltaMouseX = RT64.deltaMouseY = 0;
}

static void controller_rt64_api_shutdown(void) {
    ClipCursor(NULL);
}

static u32 controller_rt64_api_rawkey(void) {
    return VK_INVALID;
}

// Barebones controller API to handle mouse movement.
struct ControllerAPI controller_rt64_api = {
    VK_INVALID,
    controller_rt64_api_init,
    controller_rt64_api_read,
    controller_rt64_api_rawkey,
    NULL,
    NULL,
    NULL,
    controller_rt64_api_shutdown
};

#endif