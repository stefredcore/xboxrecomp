/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels -- unless the back end asks for
 * vertex programs (NV2A_BACKEND_CAP_PROGRAM), which then arrive untransformed
 * for it to run, as the NV2A's own vertex unit does. Blend, depth, alpha, cull
 * and stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* A swizzled surface (SURFACE_FORMAT type 2, render-to-texture): pixel
     * (x, y) lives at the Morton offset of a swizzled texture of swz_w x swz_h
     * (d3d8_swizzle.h swizzle_offset), not at y * pitch + x. A back end that
     * writes such a surface row-major hands the title a scrambled texture. */
    uint32_t swizzled;
    uint32_t swz_w, swz_h;
} Nv2aSurface;
#define NV2A_BACKEND_SURFACE_SWIZZLE 1   /* the three fields above exist */

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* Appended for the full pixel pipeline (Nv2aBatch.full): guest address
     * of the P8 palette, and whether the FORMAT names a cube map. A linear
     * texture (pitch != 0) is addressed in texels, a swizzled one in [0,1]
     * coordinates -- the same rule the CPU rasteriser's rc_texel applies. */
    uint32_t palette;
    uint32_t cube;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_POLY_OFFSET_FILL_ENABLE and SET_POLYGON_OFFSET_SCALE_FACTOR/_BIAS,
     * as glPolygonOffset takes them: depth moves by factor times the
     * triangle's depth slope plus units times the smallest step of the depth
     * buffer. A decal or shadow laid on a wall gets it so it is not drawn
     * at the wall's own depth; a back end that ignores it lets the two take
     * turns winning the depth test from frame to frame. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_units;
    /* SET_CONTROL0 Z_PERSPECTIVE_ENABLE (bit 16): a W-buffer. The depth
     * stored is the vertex's w (1/rhw), interpolated perspective-correctly
     * and mapped from [depth_min, depth_max] to the buffer's range, not z.
     * Linear in distance, so far away it still tells apart two surfaces a
     * z-buffer of the same width cannot. */
    uint32_t z_perspective;
} Nv2aRenderState;
#define NV2A_BACKEND_POLY_OFFSET 1   /* the three fields above exist */
#define NV2A_BACKEND_Z_PERSPECTIVE 1 /* z_perspective exists */

/* Everything the CPU rasteriser interpolates per pixel when the register
 * combiners are in use (xf_rows in nv2a_pb_exec.c): both vertex colours, the
 * fog factor (already through the fog equation) and all four texture
 * coordinates as the vertex program wrote them, before the divide by q. */
typedef struct {
    float x, y, z, rhw;             /* as in Nv2aVertex */
    float d0[4], d1[4];             /* r,g,b,a in [0,1] */
    float fog;                      /* fog factor */
    float pad[3];
    float tc[4][4];                 /* texture stage coordinates s,t,r,q */
} Nv2aVertexFull;

/* A vertex-program batch, for a back end that runs the program itself
 * (NV2A_BACKEND_CAP_PROGRAM). Nothing is transformed or clipped on the CPU:
 * the back end gets what the vertex unit would. Per vertex it must do what
 * the executor does with nv2a_vsh_run's output (nv2a_pb_exec.c,
 * transform_vertex and the near-plane clip): oPos.w of +0 reads as 1; x, y
 * times aa_sx, aa_sy; clip at w = 1e-3 and, with clip_near_z, at screen
 * z = near_z; the fog factor from oFog.x by fog_mode and fog_param, as
 * fog_factor. Sent only for batches that would go through the register
 * combiners (Nv2aBatch.full), never in shadow mode, and never for a program
 * that writes c[] (each vertex would see the one before). */
typedef struct {
    const uint32_t (*program)[4];   /* from the start slot */
    uint32_t       length;          /* instructions, the last one final */
    const float  (*constants)[4];   /* c[0..191] */
    uint32_t       input_mask;      /* v<n> the program reads */
    /* Per vertex, one float4 per bit of input_mask, lowest input first:
     * vertex_count * popcount(input_mask) float4s. */
    const float   *inputs;
    uint32_t       vertex_count;
    const uint32_t *indices;        /* triangle list into the vertices */
    uint32_t       index_count;
    float          aa_sx, aa_sy;
    float          near_z;          /* SET_CLIP_MIN, or 0 (see xf_set_viewport) */
    uint32_t       clip_near_z;
    uint32_t       fog_enable, fog_mode;
    float          fog_param[2];
} Nv2aProgramBatch;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;

    /* The full per-pixel pipeline, set when the batch would go through the
     * register combiners on the CPU rasteriser. NULL `full` means the batch
     * is drawn as before: texture stage 0 modulated by the diffuse colour. */
    const Nv2aVertexFull *full;     /* `count` entries, parallel to vertices */
    const Nv2aCombiner   *combiner;
    Nv2aTexture           stage[4]; /* valid where stage_valid bit n is set */
    uint32_t              stage_valid;
    float                 fog_color[4];      /* r,g,b of SET_FOG_COLOR, a 0 */
    uint32_t              clip_plane_mode;   /* SET_SHADER_CLIP_PLANE_MODE */

    /* Set instead of vertices/full (both NULL, count 0) for a back end with
     * NV2A_BACKEND_CAP_PROGRAM: combiner, stage and fog fields as for full. */
    const Nv2aProgramBatch *program;
} Nv2aBatch;
#define NV2A_BACKEND_HAS_PROGRAM 1   /* Nv2aBatch.program, Nv2aBackend.caps */

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Optional. Called before a semaphore release: whatever the back end has
     * drawn and keeps back from guest memory has to be there now, because the
     * title may read it as soon as it sees the release. */
    void (*sync)(void);
    /* NV2A_BACKEND_CAP_* the back end handles; 0 for none. */
    uint32_t caps;
} Nv2aBackend;

/* Vertex-program batches arrive untransformed (Nv2aProgramBatch). */
#define NV2A_BACKEND_CAP_PROGRAM 0x1u

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Nonzero with RECOMP_GPUBE_SHADOW set: the CPU rasteriser still draws into
 * guest memory and the back end gets the same clears and batches on top, so
 * it can compare its frames with the rasteriser's. A back end in this mode
 * must not write guest memory. */
int nv2a_backend_shadow(void);
#define NV2A_BACKEND_HAS_SHADOW 1

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Bytes between the faces of a cube texture (one face with all its mip
 * levels, 128-byte aligned): face f's level 0 starts at offset + f * this. */
uint32_t nv2a_backend_cube_face_stride(const Nv2aTexture *tex);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
