#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../api/plugin_api.h"
#include "../../common/accessibility/element.h"
#include "../../common/accessibility/window.h"
#include "../../common/accessibility/application.h"
#include "../../common/config/cvar.h"
#include "../../common/config/tokenize.h"
#include "../../common/dispatch/cgeventtap.h"

#include "../../common/accessibility/element.cpp"
#include "../../common/config/cvar.cpp"
#include "../../common/config/tokenize.cpp"
#include "../../common/dispatch/cgeventtap.cpp"

#define ArrayCount(a) (sizeof(a) / sizeof((*a)))
#define internal static
#define local_persist static

extern "C" int CGSMainConnectionID(void);
extern "C" CGError CGSGetWindowLevel(const int cid, int wid, int *wlvl);
extern "C" OSStatus CGSFindWindowByGeometry(int cid, int zero, int one, int zero_again, CGPoint *screen_point, CGPoint *window_coords_out, int *wid_out, int *cid_out);
extern "C" CGError CGSConnectionGetPID(const int cid, pid_t *pid);

internal event_tap EventTap;
internal uint32_t MouseModifier;
internal bool volatile IsActive;
internal uint32_t volatile FocusedWindowId;
internal chunkwm_api API;
internal AXUIElementRef SystemWideElement;
internal float MouseMotionInterval;
internal float LastEventTime;
internal bool StandbyOnFloat;
internal int CurrentWindowId = 0;
internal CFRunLoopTimerRef CurrentTimerRef = NULL;
internal float FocusDelay;

internal bool
IsWindowLevelAllowed(int WindowLevel)
{
    local_persist int ValidWindowLevels[] = {
        CGWindowLevelForKey(kCGNormalWindowLevelKey),
        CGWindowLevelForKey(kCGFloatingWindowLevelKey),
        CGWindowLevelForKey(kCGModalPanelWindowLevelKey),
    };
    local_persist int Count = ArrayCount(ValidWindowLevels);

    for (int Index = 0; Index < Count; ++Index) {
        if (WindowLevel == ValidWindowLevels[Index]) {
            return true;
        }
    }

    return false;
}

internal void Reset() {
    CurrentWindowId = 0;
    if (CurrentTimerRef == NULL) return;
    
    CFRunLoopTimerInvalidate(CurrentTimerRef);
    CFRelease(CurrentTimerRef);
    CurrentTimerRef = NULL;
}

struct WindowInfo {
    int WindowId;
    AXUIElementRef WindowRef;
    pid_t WindowPid;
};

void FocusWindow(CFRunLoopTimerRef timer, void* timerInfo) {
    WindowInfo* info = (WindowInfo*) timerInfo;
    
    AXLibSetFocusedWindow(info->WindowRef);
    AXLibSetFocusedApplication(info->WindowPid);
    
    CFRelease(info->WindowRef);
    delete info;
}

internal inline void
FocusFollowsMouse(CGEventRef Event)
{
    int WindowId = 0;
    int WindowLevel = 0;
    pid_t WindowPid = 0;
    int WindowConnection = 0;
    CGPoint WindowPosition;
    CFStringRef Role;
    AXUIElementRef Element;
    AXUIElementRef WindowRef = NULL;

    local_persist int Connection = CGSMainConnectionID();
    CGPoint CursorPosition = CGEventGetLocation(Event);
    CGSFindWindowByGeometry(Connection, 0, 1, 0, &CursorPosition, &WindowPosition, &WindowId, &WindowConnection);

    if (WindowId == 0) {
	    Reset();
	    return;
    }
    if (Connection == WindowConnection) {
	    Reset();
	    return;
    }
    if (WindowId == FocusedWindowId) {
	    Reset();
	    return;
    }

    CGSGetWindowLevel(Connection, WindowId, &WindowLevel);
    if (!IsWindowLevelAllowed(WindowLevel)) {
	    Reset();
	    return;
    }

    CGSConnectionGetPID(WindowConnection, &WindowPid);

    AXUIElementCopyElementAtPosition(SystemWideElement, CursorPosition.x, CursorPosition.y, &Element);
    if (!Element) {
	    Reset();
	    return;
    }

    if (AXLibGetWindowRole(Element, &Role)) {
        if (CFEqual(Role, kAXWindowRole)) {
            WindowRef = Element;
        } else {
            AXUIElementCopyAttributeValue(Element, kAXWindowAttribute, (CFTypeRef*)&WindowRef);
            CFRelease(Element);
        }
        CFRelease(Role);
    }

    if (!WindowRef){
	    Reset();
	    return;
    }
    if (WindowId == CurrentWindowId) return;

    Reset();

    CurrentWindowId = WindowId;

    WindowInfo* info = new WindowInfo;
    info->WindowId = WindowId;
    info->WindowPid = WindowPid;
    info->WindowRef = WindowRef;

    CFRunLoopTimerContext context;
    context.info = info;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + FocusDelay, 0, 0, 0, &FocusWindow, &context);
    CurrentTimerRef = timer;
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopAddTimer(loop, timer, kCFRunLoopCommonModes);
}


internal bool
ShouldProcessEvent(CGEventRef Event)
{
    uint64_t CurrentEventTime = CGEventGetTimestamp(Event);
    float DeltaEventTime = ((float)CurrentEventTime  - LastEventTime) * (1.0f / 1E6);
    if (DeltaEventTime < MouseMotionInterval) return false;
    LastEventTime = CurrentEventTime;
    return true;
}

EVENTTAP_CALLBACK(EventTapCallback)
{
    event_tap *EventTap = (event_tap *) Reference;
    switch (Type) {
    case kCGEventTapDisabledByTimeout:
    case kCGEventTapDisabledByUserInput: {
        CGEventTapEnable(EventTap->Handle, true);
    } break;
    case kCGEventMouseMoved: {
        if (!ShouldProcessEvent(Event)) break;
        if (StandbyOnFloat && !IsActive) break;

        CGEventFlags Flags = CGEventGetFlags(Event);
        if ((Flags & MouseModifier) == MouseModifier) break;

        FocusFollowsMouse(Event);
    } break;
    default: {} break;
    }

    return Event;
}

internal inline void
ApplicationActivatedHandler(void *Data)
{
    macos_application *Application = (macos_application *) Data;
    AXUIElementRef WindowRef = AXLibGetFocusedWindow(Application->Ref);
    if (WindowRef) {
        uint32_t WindowId = AXLibGetWindowID(WindowRef);
        FocusedWindowId = WindowId;
        CFRelease(WindowRef);
    }
}

internal inline void
WindowFocusedHandler(void *Data)
{
    Reset();
    macos_window *Window = (macos_window *) Data;
    FocusedWindowId = Window->Id;
}

internal inline void
TilingWindowFloatHandler(void *Data)
{
    uint32_t WindowId = *(uint32_t *) Data;
    uint32_t Status = *((uint32_t *) Data + 1);
    IsActive = !(Status & 0x1);
}

internal inline void
SetMouseModifier(const char *Mod)
{
    while (Mod && *Mod) {
        token ModToken = GetToken(&Mod);
        if (TokenEquals(ModToken, "fn")) {
            MouseModifier |= Event_Mask_Fn;
        } else if (TokenEquals(ModToken, "shift")) {
            MouseModifier |= Event_Mask_Shift;
        } else if (TokenEquals(ModToken, "alt")) {
            MouseModifier |= Event_Mask_Alt;
        } else if (TokenEquals(ModToken, "cmd")) {
            MouseModifier |= Event_Mask_Cmd;
        } else if (TokenEquals(ModToken, "ctrl")) {
            MouseModifier |= Event_Mask_Control;
        }
    }

    // NOTE(koekeishiya): If no matches were found, we default to FN
    if (MouseModifier == 0) MouseModifier |= Event_Mask_Fn;
}

PLUGIN_MAIN_FUNC(PluginMain)
{
    Reset();
    if (strcmp(Node, "chunkwm_export_application_activated") == 0) {
        ApplicationActivatedHandler(Data);
        return true;
    } else if (strcmp(Node, "chunkwm_export_window_focused") == 0) {
        WindowFocusedHandler(Data);
        return true;
    } else if (strcmp(Node, "Tiling_focused_window_float") == 0) {
        TilingWindowFloatHandler(Data);
        return true;
    }
    return false;
}

PLUGIN_BOOL_FUNC(PluginInit)
{
    API = ChunkwmAPI;
    SystemWideElement = AXUIElementCreateSystemWide();
    if (!SystemWideElement) return false;

    IsActive = true;
    EventTap.Mask = (1 << kCGEventMouseMoved);
    bool Result = BeginEventTap(&EventTap, &EventTapCallback);
    if (Result) {
        BeginCVars(&API);
        CreateCVar("ffm_bypass_modifier", "fn");
        CreateCVar("ffm_standby_on_float", 1);
        CreateCVar("ffm_focus_delay", 0);
        CreateCVar("mouse_motion_interval", 35.0f);
        SetMouseModifier(CVarStringValue("ffm_bypass_modifier"));
        StandbyOnFloat = CVarIntegerValue("ffm_standby_on_float");
        MouseMotionInterval = CVarFloatingPointValue("mouse_motion_interval");
	FocusDelay = CVarFloatingPointValue("ffm_focus_delay");
    }
    return Result;
}

PLUGIN_VOID_FUNC(PluginDeInit)
{
    EndEventTap(&EventTap);
    CFRelease(SystemWideElement);
}

CHUNKWM_PLUGIN_VTABLE(PluginInit, PluginDeInit, PluginMain)
chunkwm_plugin_export Subscriptions[] =
{
    chunkwm_export_application_activated,
    chunkwm_export_window_focused
};
CHUNKWM_PLUGIN_SUBSCRIBE(Subscriptions)
CHUNKWM_PLUGIN("Focus Follows Mouse", "0.3.5")
