/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * Keys and mouse buttons go through the toolkit's binding rules
 * (input_map.h): every pad control takes a list of key names, from the
 * built-in layout below or from mkb.ini, whose [section]s can apply to
 * one executable only -- a game whose driving part is its own .exe gets
 * its own layout there.
 *
 * The mouse drives the aim stick by its speed, averaged over the last
 * window_ms: the title integrates deflection over its own frame, so the
 * rotation comes out proportional to mouse travel whatever the frame
 * rate and however often the pad is polled. Small movements get a floor
 * of `dz` of full throw, because every console title applies its own
 * dead zone to the stick, and a mouse correction smaller than it would
 * be silently eaten.
 *
 * The wheel has no key code; each step becomes a press of the controls
 * bound to "wheelup"/"wheeldown", held for wheel_ms, then released for
 * as long, so the title's edge detection sees every step. With the aim
 * control held (aim_control, e.g. a zoom while aiming), steps go to
 * "aim+wheelup"/"aim+wheeldown" instead where any are bound.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"
#include "input_map.h"

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- configuration ----------------------------------------------------- */

enum { WHEEL_UP, WHEEL_DOWN, AIM_WHEEL_UP, AIM_WHEEL_DOWN, WHEEL_KINDS };
static const char *const k_wheel_names[WHEEL_KINDS] = {
    "wheelup", "wheeldown", "aim+wheelup", "aim+wheeldown"
};

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    double full_cps;    /* mouse counts per second for full throw    */
    int    window_ms;   /* speed is averaged over this long          */
    int    wheel_ms;    /* a wheel step is held, then released, this long */
    int    invert_y;    /* invert the vertical aim axis              */
    int    mouse_stick; /* 0 right, 1 left, 2 none                   */
    int    aim_control; /* InputControl that switches the wheel, -1 none */
    double look_sens;   /* direct look: "sensitivity" as on the PC   */
    double m_yaw, m_pitch; /* degrees per count at sensitivity 1     */
    InputBindings keys;
    uint32_t wheel[WHEEL_KINDS];   /* bit per InputControl */
} MkbConfig;

static MkbConfig s_cfg;

/* The layout without an mkb.ini, the one this layer always had: WASD and
 * mouse aim, mouse 1/2 on the triggers, Space A, R X, E B, Q and mouse 4
 * White, F and mouse 5 Black, the wheel and middle button Y. */
static const char k_default_layout[] =
    "left_stick_up = w\n" "left_stick_down = s\n"
    "left_stick_left = a\n" "left_stick_right = d\n"
    "right_trigger = mouse1\n" "left_trigger = mouse2\n"
    "a = space\n" "x = r\n" "b = e\n" "y = mouse3, wheelup, wheeldown\n"
    "white = q, mouse4\n" "black = f, mouse5\n"
    "start = enter, escape\n" "back = tab\n"
    "left_thumb = lshift\n" "right_thumb = lctrl\n"
    "dpad_up = up\n" "dpad_down = down\n" "dpad_left = left\n" "dpad_right = right\n";

static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.sens = 1.0;
    s_cfg.dz = 0.18;
    s_cfg.acc = 1.0;
    s_cfg.full_cps = 2000.0;
    s_cfg.window_ms = 25;
    s_cfg.wheel_ms = 60;
    s_cfg.aim_control = -1;
    s_cfg.look_sens = 2.0;
    s_cfg.m_yaw = 0.022;
    s_cfg.m_pitch = 0.022;
    s_cfg.keys.diagonal = INPUT_DIAGONAL_ROUND;
}

/* A control's binding list: the key names go to the bindings, the wheel
 * names to their masks. Returns the names it could not place. */
static int bind_control(int control, const char *text)
{
    char keys[256], tok[48];
    const char *p = text;
    size_t kl = 0;
    int w, bad = 0;

    for (w = 0; w < WHEEL_KINDS; w++)
        s_cfg.wheel[w] &= ~(1u << control);
    keys[0] = 0;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t a = 0, b = n;
        while (a < b && (p[a] == ' ' || p[a] == '\t')) a++;
        while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t')) b--;
        if (b > a && b - a < sizeof tok) {
            int found = 0;
            memcpy(tok, p + a, b - a);
            tok[b - a] = 0;
            for (w = 0; w < WHEEL_KINDS; w++)
                if (!_stricmp(tok, k_wheel_names[w])) {
                    s_cfg.wheel[w] |= 1u << control;
                    found = 1;
                }
            if (!found && kl + strlen(tok) + 2 < sizeof keys)
                kl += (size_t)sprintf(keys + kl, "%s%s", kl ? "," : "", tok);
        }
        if (!e)
            break;
        p = e + 1;
    }
    bad = xbox_InputMap_BindingsParse(&s_cfg.keys, control, keys);
    return bad > 0 ? bad : 0;
}

/* One "key = value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;
    int    c;

    if (!_stricmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!_stricmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!_stricmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!_stricmp(key, "full_cps")) {
        d = strtod(val, NULL); if (d > 1.0 && d < 1e6) s_cfg.full_cps = d;
    } else if (!_stricmp(key, "window_ms")) {
        l = strtol(val, NULL, 0); if (l >= 5 && l <= 200) s_cfg.window_ms = (int)l;
    } else if (!_stricmp(key, "wheel_ms")) {
        l = strtol(val, NULL, 0); if (l >= 10 && l <= 500) s_cfg.wheel_ms = (int)l;
    } else if (!_stricmp(key, "sensitivity")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.look_sens = d;
    } else if (!_stricmp(key, "m_yaw")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.m_yaw = d;
    } else if (!_stricmp(key, "m_pitch")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.m_pitch = d;
    } else if (!_stricmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!_stricmp(key, "mouse_stick")) {
        s_cfg.mouse_stick = !_stricmp(val, "left") ? 1 : !_stricmp(val, "none") ? 2 : 0;
    } else if (!_stricmp(key, "aim_control")) {
        s_cfg.aim_control = xbox_InputMap_ControlFromName(val);
    } else if (!_stricmp(key, "diagonal")) {
        s_cfg.keys.diagonal = !_stricmp(val, "full") ? INPUT_DIAGONAL_FULL
                                                     : INPUT_DIAGONAL_ROUND;
    } else if ((c = xbox_InputMap_ControlFromName(key)) >= 0) {
        int bad = bind_control(c, val);
        if (bad)
            fprintf(stderr, "  [MKB] %s: %d name(s) not understood in \"%s\"\n",
                    key, bad, val);
    } else {
        return 0;
    }
    return 1;
}

/* The executable's base name without ".exe", lowercased: the [section]
 * names an mkb.ini applies to. */
static void exe_name(char *out, size_t n)
{
    char path[MAX_PATH], *base, *dot;
    size_t i;

    out[0] = 0;
    if (!GetModuleFileNameA(NULL, path, sizeof path))
        return;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if ((dot = strrchr(base, '.')) != NULL)
        *dot = 0;
    for (i = 0; base[i] && i + 1 < n; i++)
        out[i] = (char)tolower((unsigned char)base[i]);
    out[i] = 0;
}

/* "key = value" lines; a "[name]" line starts a section that applies only
 * when the executable is called name ("[*]" or no section: always). A
 * section line may list several names separated by commas. */
static void apply_text(const char *text, const char *exe)
{
    char line[512];
    int on = 1;

    while (*text) {
        const char *e = strchr(text, '\n');
        size_t n = e ? (size_t)(e - text) : strlen(text);
        char *k, *v, *eq;
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, text, n);
        line[n] = 0;
        text += e ? n + 1 : n;
        line[strcspn(line, "\r#;")] = 0;           /* comments */
        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '[') {
            char *end = strchr(k, ']'), *name;
            on = 0;
            if (!end)
                continue;
            *end = 0;
            for (name = strtok(k + 1, ","); name; name = strtok(NULL, ",")) {
                while (*name == ' ') name++;
                name[strcspn(name, " \t")] = 0;
                if (!strcmp(name, "*") || !_stricmp(name, exe))
                    on = 1;
            }
            continue;
        }
        if (!on || !(eq = strchr(k, '=')))
            continue;
        *eq = 0;
        v = eq + 1;
        k[strcspn(k, " \t")] = 0;
        while (*v == ' ' || *v == '\t') v++;
        n = strlen(v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = 0;
        if (*k && !mkb_apply_kv(k, v))
            fprintf(stderr, "  [MKB] unknown setting \"%s\"\n", k);
    }
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL_CPS", "full_cps",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        NULL
    };
    char path[MAX_PATH], exe[64];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    cfg_defaults();
    exe_name(exe, sizeof exe);
    apply_text(k_default_layout, exe);

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            path[0] = 0;
        else
            snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = path[0] ? fopen(path, "rb") : NULL;
    if (f) {
        long len;
        char *text;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = len > 0 && len < (1 << 20) ? (char *)malloc((size_t)len + 1) : NULL;
        if (text && fread(text, 1, (size_t)len, f) == (size_t)len) {
            text[len] = 0;
            /* The file's layout replaces the built-in one entirely. */
            memset(&s_cfg.keys.count, 0, sizeof s_cfg.keys.count);
            memset(s_cfg.wheel, 0, sizeof s_cfg.wheel);
            apply_text(text, exe);
            fprintf(stderr, "  [MKB] layout from %s (as %s)\n", path, exe);
        }
        free(text);
        fclose(f);
    }
    for (i = 0; kv[i]; i += 2) {                  /* the environment wins */
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }
}

/* ---- raw input thread -------------------------------------------------- */

/* Mouse motion, as (time, dx, dy) samples: the poll averages the ones in
 * the last window_ms. One writer (the input thread), one reader. */
#define MOTION_RING 512
static struct { LONGLONG t; LONG dx, dy; } s_motion[MOTION_RING];
static volatile LONG s_motion_head;           /* samples written */
static CRITICAL_SECTION s_motion_lock;

static volatile LONG s_wheel;                 /* steps since last poll, + up */
static volatile LONG s_look_dx, s_look_dy;    /* counts since last mkb_mouse_look */
static volatile unsigned char s_vk_down[256]; /* held keys and mouse buttons */
static volatile unsigned char s_vk_hit[256];  /* went down since last poll   */

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static LONGLONG qpc_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static void key_set(UINT vk, int down)
{
    if (vk >= 256)
        return;
    if (down && !s_vk_down[vk])
        s_vk_hit[vk] = 1;
    s_vk_down[vk] = (unsigned char)(down != 0);
}

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;
        static const struct { USHORT down, up; UINT vk; } btn[] = {
            { RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   VK_LBUTTON },
            { RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  VK_RBUTTON },
            { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON },
            { RI_MOUSE_BUTTON_4_DOWN,      RI_MOUSE_BUTTON_4_UP,      VK_XBUTTON1 },
            { RI_MOUSE_BUTTON_5_DOWN,      RI_MOUSE_BUTTON_5_UP,      VK_XBUTTON2 },
        };
        size_t i;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE) && (m->lLastX || m->lLastY)) {
            LONG k;
            EnterCriticalSection(&s_motion_lock);
            k = s_motion_head % MOTION_RING;
            s_motion[k].t = qpc_now();
            s_motion[k].dx = m->lLastX;
            s_motion[k].dy = m->lLastY;
            s_motion_head++;
            LeaveCriticalSection(&s_motion_lock);
            InterlockedAdd(&s_look_dx, m->lLastX);
            InterlockedAdd(&s_look_dy, m->lLastY);
        }
        for (i = 0; i < sizeof btn / sizeof btn[0]; i++) {
            if (m->usButtonFlags & btn[i].down) key_set(btn[i].vk, 1);
            if (m->usButtonFlags & btn[i].up)   key_set(btn[i].vk, 0);
        }
        if (m->usButtonFlags & RI_MOUSE_WHEEL)
            InterlockedAdd(&s_wheel, (SHORT)m->usButtonData / WHEEL_DELTA);
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey, side = 0;
        int down = !(k->Flags & RI_KEY_BREAK);

        if (vk == MKB_TOGGLE_VK && down && !s_vk_down[vk]) {
            LONG now = !s_active;
            InterlockedExchange(&s_active, now);
            fprintf(stderr, "  [MKB] %s\n", now ? "captured" : "released (F9)");
            fflush(stderr);
        }
        /* Raw input names Shift, Ctrl and Alt without a side; the
         * bindings know them by side ("lctrl") as well. */
        if (vk == VK_SHIFT)
            side = k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        else if (vk == VK_CONTROL)
            side = (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            side = (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
        if (vk && vk < 256)
            key_set(vk, down);
        if (side)
            key_set(side, down);
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full_cps=%.0f window=%d ms)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full_cps, s_cfg.window_ms);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            InitializeCriticalSection(&s_motion_lock);
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    len = r > 1.0 ? 1.0 : r;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(len, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / r;
    fy = ay * len / r;
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Mouse speed over the last window_ms, in counts per second. */
static void mouse_speed(double *vx, double *vy)
{
    static LONGLONG freq;
    LONGLONG now = qpc_now(), from;
    LONG head, i, n;
    double sx = 0.0, sy = 0.0;

    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart ? f.QuadPart : 1;
    }
    from = now - freq * s_cfg.window_ms / 1000;
    EnterCriticalSection(&s_motion_lock);
    head = s_motion_head;
    n = head < MOTION_RING ? head : MOTION_RING;
    for (i = 1; i <= n; i++) {
        LONG k = (head - i) % MOTION_RING;
        if (s_motion[k].t < from)
            break;
        sx += s_motion[k].dx;
        sy += s_motion[k].dy;
    }
    LeaveCriticalSection(&s_motion_lock);
    *vx = sx * 1000.0 / s_cfg.window_ms;
    *vy = sy * 1000.0 / s_cfg.window_ms;
}

/* The wheel's pulses: each step holds its controls for wheel_ms, then
 * releases them as long. Returns the mask of controls pressed now. */
static uint32_t wheel_pulse(int aiming)
{
    static LONG pending;             /* steps not yet played, + up */
    static int phase;                /* 0 idle, 1 pressed, 2 released */
    static uint32_t mask;
    static DWORD since;
    DWORD now = GetTickCount();

    pending += InterlockedExchange(&s_wheel, 0);
    if (phase == 1 && now - since >= (DWORD)s_cfg.wheel_ms) {
        phase = 2;
        since = now;
    }
    if (phase == 2 && now - since >= (DWORD)s_cfg.wheel_ms)
        phase = 0;
    if (phase == 0 && pending) {
        int up = pending > 0;
        uint32_t aimed = s_cfg.wheel[up ? AIM_WHEEL_UP : AIM_WHEEL_DOWN];
        pending += up ? -1 : 1;
        mask = aiming && aimed ? aimed : s_cfg.wheel[up ? WHEEL_UP : WHEEL_DOWN];
        phase = 1;
        since = now;
    }
    return phase == 1 ? mask : 0;
}

/* One pad control pressed, by the InputControl numbering. */
static void press_control(XBOX_GAMEPAD *g, int c)
{
    if (c < 8)
        g->wButtons |= (WORD)(1u << c);
    else if (c < INPUT_PAD_CONTROL_COUNT)
        g->bAnalogButtons[c - 8] = 255;
    else if (c == INPUT_LSTICK_UP)    g->sThumbLY = 32767;
    else if (c == INPUT_LSTICK_DOWN)  g->sThumbLY = -32767;
    else if (c == INPUT_LSTICK_LEFT)  g->sThumbLX = -32767;
    else if (c == INPUT_LSTICK_RIGHT) g->sThumbLX = 32767;
    else if (c == INPUT_RSTICK_UP)    g->sThumbRY = 32767;
    else if (c == INPUT_RSTICK_DOWN)  g->sThumbRY = -32767;
    else if (c == INPUT_RSTICK_LEFT)  g->sThumbRX = -32767;
    else if (c == INPUT_RSTICK_RIGHT) g->sThumbRX = 32767;
}

int mkb_mouse_look(double *yaw_deg, double *pitch_deg)
{
    static LONGLONG last;
    LONGLONG now = qpc_now(), freq;
    LARGE_INTEGER f;
    LONG dx, dy;
    int stale;

    *yaw_deg = *pitch_deg = 0.0;
    if (!mkb_enabled())
        return 0;
    QueryPerformanceFrequency(&f);
    freq = f.QuadPart;
    /* Counts gathered while the view was not being turned (menus, pause,
     * cutscenes, another app) are dropped, not applied in one jump. */
    stale = !last || now - last > freq / 10;
    last = now;
    dx = InterlockedExchange(&s_look_dx, 0);
    dy = InterlockedExchange(&s_look_dy, 0);
    if (stale || !InterlockedCompareExchange(&s_active, 0, 0) ||
            !game_is_foreground())
        return 0;
    *yaw_deg = dx * s_cfg.look_sens * s_cfg.m_yaw;
    *pitch_deg = -dy * s_cfg.look_sens * s_cfg.m_pitch;
    if (s_cfg.invert_y)
        *pitch_deg = -*pitch_deg;
    return 1;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    static XBOX_GAMEPAD last;
    static DWORD packet;
    XBOX_GAMEPAD mk;
    uint8_t down[INPUT_KEY_COUNT];
    uint32_t wheel;
    double vx, vy;
    int i, aiming;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    if (!s_active) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        InterlockedExchange(&s_wheel, 0);
        return 0;
    }
    mkb_manage_cursor(1);

    /* Held keys, and keys that went down and up since the last poll (a
     * click shorter than a poll still counts once). */
    for (i = 0; i < INPUT_KEY_COUNT; i++) {
        down[i] = (uint8_t)(s_vk_down[i] | s_vk_hit[i]);
        s_vk_hit[i] = 0;
    }
    memset(&mk, 0, sizeof mk);
    xbox_InputMap_KeysResolve(&s_cfg.keys, down, &mk);

    aiming = s_cfg.aim_control >= 0 && s_cfg.aim_control < INPUT_PAD_CONTROL_COUNT
          && (s_cfg.aim_control < 8
                  ? (mk.wButtons >> s_cfg.aim_control) & 1
                  : mk.bAnalogButtons[s_cfg.aim_control - 8] != 0);
    wheel = wheel_pulse(aiming);
    for (i = 0; i < INPUT_CONTROL_COUNT; i++)
        if ((wheel >> i) & 1)
            press_control(&mk, i);

    /* The mouse owns its stick while it moves. */
    mouse_speed(&vx, &vy);
    if (s_cfg.mouse_stick != 2 && (vx != 0.0 || vy != 0.0)) {
        SHORT ax, ay;
        double fy = -vy / s_cfg.full_cps;
        if (s_cfg.invert_y)
            fy = -fy;
        mkb_aim_vector(vx / s_cfg.full_cps, fy, &ax, &ay);
        if (s_cfg.mouse_stick == 1) { mk.sThumbLX = ax; mk.sThumbLY = ay; }
        else                        { mk.sThumbRX = ax; mk.sThumbRY = ay; }
    }

    xbox_InputMap_PadMerge(&pState->Gamepad, &mk);

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move -- releases included. Counted in the high
     * half, so a pad's own number still moves the sum when it changes. */
    if (memcmp(&mk, &last, sizeof mk)) {
        last = mk;
        packet++;
    }
    pState->dwPacketNumber += packet << 16;

    if (mk.wButtons || mk.sThumbLX || mk.sThumbLY || mk.sThumbRX || mk.sThumbRY)
        return 1;
    for (i = 0; i < 8; i++)
        if (mk.bAnalogButtons[i])
            return 1;
    return 0;
}
