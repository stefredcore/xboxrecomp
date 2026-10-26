/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars and an optional mkb.ini, so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1          master switch          (default off, opt-in)
 *   RECOMP_MKB_SENS=f       mouse sensitivity      (default 1.0)
 *   RECOMP_MKB_FULL_CPS=n   mouse counts per second for a full stick
 *                           throw                  (default 2000)
 *   RECOMP_MKB_DZ=f         assumed game deadzone  (default 0.18)
 *   RECOMP_MKB_ACC=f        aim acceleration       (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1   invert vertical aim    (default off)
 *   RECOMP_MKB_INI=path     the layout file (default mkb.ini beside the .exe)
 *
 * mkb.ini: "setting = value" lines, '#' or ';' comments. The settings above
 * by their short names (sens, full_cps, dz, acc, invert_y), plus
 * window_ms, wheel_ms, mouse_stick (right, left, none), aim_control (a
 * control name), diagonal (round, full), and one line per pad control
 * listing up to four keys:
 *
 *   right_trigger = mouse1
 *   left_stick_up = w, up
 *   dpad_right    = wheeldown          (wheelup/wheeldown: one press a step;
 *   dpad_up       = aim+wheelup         aim+...: while aim_control is held)
 *
 * Control names as input_map.c lists them (a, b, x, y, black, white,
 * left_trigger, dpad_up, start, back, left_thumb, left_stick_up, ...); key
 * names as input_map.c reads them (w, space, lctrl, escape, mouse1-5, f1).
 * A "[name]" line starts a section for the executable called name only
 * ("[*]" for all), so a title's second .exe can have its own layout. A file
 * replaces the built-in layout; without one it is WASD, mouse aim, mouse 1/2
 * on the triggers and Space, R, E, Q, F on A, X, B, White, Black.
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps. While it is
 * on, RECOMP_KEYBOARD's key map stands aside (xinput_device.c).
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and each stick
 * takes whichever source is deflected further -- the mouse's aim stick
 * while it moves. dwPacketNumber moves whenever the virtual pad changes,
 * releases included, so a title watching for edges sees them.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/*
 * Direct mouse look, the way PC shooters turn: the view angle changes by
 * the mouse counts since the last call, sensitivity * m_yaw (m_pitch)
 * degrees per count, with no stick, deadzone or speed curve in between.
 * For a title hook that adds the result to the view angles itself, once
 * a frame. Yaw is positive to the right, pitch positive looking up.
 * mkb.ini: sensitivity (default 2.0), m_yaw and m_pitch (0.022), invert_y.
 *
 * Returns 1 when the mouse is captured and the game in the foreground.
 * Counts that pile up between calls more than 100 ms apart are dropped.
 */
int mkb_mouse_look(double *yaw_deg, double *pitch_deg);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
