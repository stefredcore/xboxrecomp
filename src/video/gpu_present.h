/* D3D11 presentation for the framebuffer window (see gpu_present.c). */
#ifndef XBOX_GPU_PRESENT_H
#define XBOX_GPU_PRESENT_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

int  gpu_present_enabled(void);                 /* RECOMP_D3D_PRESENT=1 */
int  gpu_present_init(HWND hwnd, uint32_t width, uint32_t height);
/* width*height B8G8R8A8 (any size; the frame texture follows it); returns 1
 * only when the present waited for vsync (so the caller need not sleep), 0
 * when unpaced or nothing was shown. */
int  gpu_present_frame(const uint32_t *pixels, uint32_t width, uint32_t height);
/* A back end's frame from a shared texture (fb_present.c,
 * xbox_FramebufferWindowSetGpu): resolved by mode (fb_present.c aa_mode) into
 * a width x height frame, then shown as gpu_present_frame shows one. -1 when
 * it cannot be (the caller shows guest memory's copy), else as
 * gpu_present_frame. */
int  gpu_present_shared(void *handle, uint32_t width, uint32_t height,
                        uint32_t lines, uint32_t mode, uint32_t aa_sy);
int  gpu_present_active(void);
/* Width / height the frame is shown at: gpu_present_set_aspect's (> 0),
 * else RECOMP_DISPLAY_ASPECT, else w / h. */
float gpu_present_aspect(uint32_t w, uint32_t h);
void  gpu_present_set_aspect(float aspect);
/* Weight of each neighbouring line of the frame's own rows in what is shown,
 * as the TV encoder's flicker filter; 0 is off. A shared frame's rows are the
 * lines of gpu_present_shared, however much larger it is drawn. */
void  gpu_present_set_flicker(float weight);
void gpu_present_shutdown(void);
#endif
#endif
