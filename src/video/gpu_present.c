/*
 * D3D11 presentation for the framebuffer window (fb_present.c).
 *
 * GDI shows each frame with StretchDIBits on a 16 ms timer, scaled on the
 * CPU. This puts a D3D11 device and a swap chain on the same window and
 * presents the flipped frame through it, vsync-paced. The frame (the guest's
 * A8R8G8B8, i.e. B8G8R8A8 in memory order) is uploaded into its own texture
 * and drawn into a back buffer the size of the window's client area,
 * letterboxed to the frame's aspect ratio on black -- the window can be any
 * size (or borderless fullscreen) without stretching the picture.
 *
 * Opt-in with RECOMP_D3D_PRESENT=1; anything that fails falls back to GDI.
 * Linear filtering, RECOMP_PRESENT_FILTER=point for sharp pixels;
 * RECOMP_PRESENT_VSYNC=0 presents without waiting for the display, as the
 * SDL window does. One frame is queued at most (RECOMP_PRESENT_LATENCY).
 * The frame texture follows the frame's size.
 *
 * A render back end drawing at a multiple of the title's resolution hands
 * its frame over in a texture shared between the two devices
 * (gpu_present_shared): resolved into the frame texture by a pass of its own
 * -- the anti-aliasing filter fb_present.c applies on the CPU, done here --
 * and shown from there the same way. Nothing goes through the CPU.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_present.h"

static ID3D11Device             *s_dev;
static ID3D11DeviceContext      *s_ctx;
static IDXGISwapChain           *s_chain;
static ID3D11RenderTargetView   *s_rtv;
static ID3D11Texture2D          *s_frame;      /* the guest frame, w x h */
static ID3D11ShaderResourceView *s_srv;
static ID3D11RenderTargetView   *s_frame_rtv;  /* ... drawn into by the resolve */
static ID3D11PixelShader        *s_rps;        /* the resolve (gpu_present_shared) */
static ID3D11Buffer             *s_rcb;
static ID3D11SamplerState       *s_samp;
static ID3D11VertexShader       *s_vs;
static ID3D11PixelShader        *s_ps;
static ID3D11PixelShader        *s_fps;        /* s_ps with the flicker filter */
static ID3D11Buffer             *s_fcb;
static float                     s_flicker;    /* weight of each neighbour line */
static uint32_t                  s_lines;      /* the frame's own rows */
static HWND                      s_hwnd;
static uint32_t                  s_w, s_h;      /* frame size */
static uint32_t                  s_bw, s_bh;    /* back buffer (client) size */
static int                       s_vsync = 1;

/* A triangle covering the viewport, from the vertex id; uv 0..1. */
static const char s_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
    "  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
    "cbuffer F : register(b0) { float fw, fdy, f0, f1; };\n"
    "float4 pf(V i) : SV_Target {\n"
    "  float3 c = t.Sample(s, i.uv).rgb * (1 - 2 * fw)\n"
    "           + (t.Sample(s, i.uv - float2(0, fdy)).rgb\n"
    "              + t.Sample(s, i.uv + float2(0, fdy)).rgb) * fw;\n"
    "  return float4(c, 1); }\n";

/* A shared frame into the frame texture, pixel for pixel, as aa_resolve_row
 * (fb_present.c) filters a row: mode 0 copies, 1 averages the 2x1 samples
 * behind a pixel, 2 the 2x2, 3 is quincunx (the pixel's sample at 1/2, the
 * other column's samples in the rows above and below at 1/8 each), 4 takes
 * every other sample. */
static const char s_resolve_hlsl[] =
    "Texture2D<float4> raw : register(t0);\n"
    "cbuffer R : register(b0) { uint mode, aasy, rw, rh; };\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 px(int x, int y) {\n"
    "  return raw.Load(int3(clamp(x, 0, (int)rw - 1), clamp(y, 0, (int)rh - 1), 0));\n"
    "}\n"
    "float4 ps(V i) : SV_Target {\n"
    "  int x = (int)i.p.x, y = (int)i.p.y; float4 c;\n"
    "  if (mode == 0) c = px(x, y);\n"
    "  else if (mode == 4) c = px(2 * x, y * (int)aasy);\n"
    "  else if (mode == 2) c = (px(2 * x, 2 * y) + px(2 * x + 1, 2 * y)\n"
    "                         + px(2 * x, 2 * y + 1) + px(2 * x + 1, 2 * y + 1)) * 0.25;\n"
    "  else if (mode == 3) {\n"
    "    int l = x ? 2 * x - 1 : 1, r = 2 * x + 1;\n"
    "    c = (px(2 * x, y) * 4.0 + px(l, y - 1) + px(r, y - 1) + px(l, y + 1) + px(r, y + 1)) * 0.125;\n"
    "  } else c = (px(2 * x, y) + px(2 * x + 1, y)) * 0.5;\n"
    "  return float4(c.rgb, 1);\n"
    "}\n";

/* Textures opened from a back end's shared handles. */
typedef struct {
    void                     *handle;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    IDXGIKeyedMutex          *km;
    uint32_t                  w, h, last;
} Shared;
static Shared   s_shared[4];
static uint32_t s_shared_use;

int gpu_present_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_D3D_PRESENT");
        on = e && *e && *e != '0';
    }
    return on;
}

/* The shape the frame is shown at: the frame's own, gpu_present_set_aspect's
 * when it gave one, else RECOMP_DISPLAY_ASPECT=<w>:<h> (16:9, say) -- a
 * title drawing for a widescreen TV squeezes a 16:9 picture into its
 * 640x480, and the TV stretched it back out -- else the frame's own. */
static float s_aspect_now;

void gpu_present_set_aspect(float aspect)
{
    s_aspect_now = aspect;
}

float gpu_present_aspect(uint32_t w, uint32_t h)
{
    static float a = -1.0f;
    if (s_aspect_now > 0.0f)
        return s_aspect_now;
    if (a < 0.0f) {
        const char *e = getenv("RECOMP_DISPLAY_ASPECT");
        float x = 0.0f, y = 0.0f;
        a = 0.0f;
        if (e && sscanf(e, "%f:%f", &x, &y) == 2 && x > 0.0f && y > 0.0f)
            a = x / y;
    }
    return a > 0.0f ? a : (float)w / (float)h;
}

static void client_size(uint32_t *w, uint32_t *h)
{
    RECT r;
    GetClientRect(s_hwnd, &r);
    *w = r.right > r.left ? (uint32_t)(r.right - r.left) : 1;
    *h = r.bottom > r.top ? (uint32_t)(r.bottom - r.top) : 1;
}

static int make_rtv(void)
{
    ID3D11Texture2D *back = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr))
        return 0;
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL, &s_rtv);
    ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr);
}

/* The frame texture and its view at w x h, replacing any other size. */
static int make_frame(uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_frame_rtv) { ID3D11RenderTargetView_Release(s_frame_rtv); s_frame_rtv = NULL; }
    if (s_srv)   { ID3D11ShaderResourceView_Release(s_srv); s_srv = NULL; }
    if (s_frame) { ID3D11Texture2D_Release(s_frame);        s_frame = NULL; }
    ZeroMemory(&td, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_frame))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_frame,
                                                        NULL, &s_srv))
        || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s_frame,
                                                      NULL, &s_frame_rtv)))
        return 0;
    s_w = w; s_h = h;
    return 1;
}

static ID3DBlob *compile_src(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "present", NULL, NULL, entry, target,
                            0, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] present shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_src(s_hlsl, entry, target);
}

int gpu_present_init(HWND hwnd, uint32_t width, uint32_t height)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_SAMPLER_DESC smp;
    ID3DBlob *vsb, *psb;
    HRESULT hr;
    const char *v = getenv("RECOMP_PRESENT_VSYNC"), *f = getenv("RECOMP_PRESENT_FILTER");

    if (v && v[0] == '0')
        s_vsync = 0;
    s_hwnd = hwnd;
    s_w = width; s_h = height;
    client_size(&s_bw, &s_bh);
    ZeroMemory(&sd, sizeof sd);
    sd.BufferDesc.Width  = s_bw;
    sd.BufferDesc.Height = s_bh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount       = 1;
    sd.OutputWindow      = hwnd;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd,
                                       &s_chain, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] device/swap chain failed (0x%08lX); staying on GDI\n",
                (unsigned long)hr);
        return 0;
    }
    {
        /* Alt+Enter is the window's (borderless fullscreen, fb_present.c),
         * not DXGI's exclusive mode at 640x480. */
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_chain, &IID_IDXGIFactory, (void **)&fac))) {
            IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
    }
    {
        /* One frame in flight, not DXGI's default of three. Nothing but
         * Present paces the window thread, so it kept that queue full and
         * every frame waited up to three refreshes before it was scanned out:
         * ~50 ms of input lag at 60 Hz that the console never had.
         * RECOMP_PRESENT_LATENCY=<n> sets another depth (1-16). */
        const char *l = getenv("RECOMP_PRESENT_LATENCY");
        UINT depth = l ? (UINT)atoi(l) : 1u;
        IDXGIDevice1 *d1 = NULL;
        if (depth >= 1 && depth <= 16
            && SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice1, (void **)&d1))) {
            IDXGIDevice1_SetMaximumFrameLatency(d1, depth);
            IDXGIDevice1_Release(d1);
        }
    }
    ZeroMemory(&smp, sizeof smp);
    smp.Filter = (f && (f[0] == 'p' || f[0] == 'P')) ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                                     : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    vsb = compile("vs", "vs_4_0");
    psb = compile("ps", "ps_4_0");
    if (!vsb || !psb || !make_rtv()
        || !make_frame(width, height)
        || FAILED(ID3D11Device_CreateSamplerState(s_dev, &smp, &s_samp))
        || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                                  ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs))
        || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                                 ID3D10Blob_GetBufferSize(psb), NULL, &s_ps))) {
        fprintf(stderr, "  [D3D] present resources failed; staying on GDI\n");
        if (vsb) ID3D10Blob_Release(vsb);
        if (psb) ID3D10Blob_Release(psb);
        gpu_present_shutdown();
        return 0;
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    fprintf(stderr, "  [D3D] presenting through D3D11 (%ux%u frame, vsync %s, %s filter)\n",
            width, height, s_vsync ? "on" : "off",
            smp.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT ? "point" : "linear");
    return 1;
}

void gpu_present_set_flicker(float weight)
{
    s_flicker = weight;
}

static int flicker_ready(void)
{
    if (!s_fps) {
        ID3DBlob *b = compile("pf", "ps_4_0");
        D3D11_BUFFER_DESC bd;
        HRESULT hr;
        if (!b)
            return 0;
        hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                            ID3D10Blob_GetBufferSize(b), NULL, &s_fps);
        ID3D10Blob_Release(b);
        if (FAILED(hr))
            return 0;
        ZeroMemory(&bd, sizeof bd);
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_fcb)))
            return 0;
    }
    return s_fcb != NULL;
}

/* Returns 0 when nothing was shown: the window is occluded or the display
 * is off, and Present does not wait for vsync then -- the caller must pace
 * itself instead of spinning. The pixels are read before this returns. */
/* The frame texture, letterboxed into the back buffer, and presented. */
static int show_frame(void)
{
    static const float black[4] = {0, 0, 0, 1};
    D3D11_VIEWPORT vp;
    uint32_t cw, ch;
    float a;

    client_size(&cw, &ch);
    if (cw != s_bw || ch != s_bh) {
        /* The window was resized (or went fullscreen): a back buffer its size. */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(s_rtv);
        s_rtv = NULL;
        if (FAILED(IDXGISwapChain_ResizeBuffers(s_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))
            || !make_rtv())
            return 0;
        s_bw = cw; s_bh = ch;
    }
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_rtv, black);
    /* The largest rectangle of the shown shape that fits, centred. */
    a = gpu_present_aspect(s_w, s_h);
    if ((float)s_bw > (float)s_bh * a) {
        vp.Height = (float)s_bh;
        vp.Width = (float)s_bh * a;
    } else {
        vp.Width = (float)s_bw;
        vp.Height = (float)s_bw / a;
    }
    vp.TopLeftX = ((float)s_bw - vp.Width) * 0.5f;
    vp.TopLeftY = ((float)s_bh - vp.Height) * 0.5f;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    if (s_flicker > 0.0f && s_lines && flicker_ready()) {
        float cb[4] = { s_flicker, 1.0f / (float)s_lines, 0.0f, 0.0f };
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_fcb, 0, NULL, cb, 0, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_fcb);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_fps, NULL, 0);
    } else {
        ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    /* Paced only when Present waited for the vblank: with RECOMP_PRESENT_VSYNC=0
     * the caller keeps its own 16 ms sleep instead of spinning a core. */
    return IDXGISwapChain_Present(s_chain, (UINT)s_vsync, 0) == S_OK && s_vsync;
}

/* Returns 0 when nothing was shown: the window is occluded or the display
 * is off, and Present does not wait for vsync then -- the caller must pace
 * itself instead of spinning. The pixels are read before this returns. */
int gpu_present_frame(const uint32_t *pixels, uint32_t width, uint32_t height)
{
    s_lines = height;
    if (!s_rtv || !pixels)
        return 0;
    if ((width != s_w || height != s_h) && !make_frame(width, height))
        return 0;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_frame, 0, NULL,
                                          pixels, s_w * 4, 0);
    return show_frame();
}

static void shared_free(Shared *e)
{
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->km)  IDXGIKeyedMutex_Release(e->km);
    if (e->tex) ID3D11Texture2D_Release(e->tex);
    memset(e, 0, sizeof *e);
}

/* The texture behind a shared handle, opened once; the least recently used
 * of four makes room. */
static Shared *shared_get(void *handle)
{
    Shared *e, *victim = &s_shared[0];
    D3D11_TEXTURE2D_DESC td;
    int i;

    for (i = 0; i < 4; i++) {
        e = &s_shared[i];
        if (e->tex && e->handle == handle) {
            e->last = ++s_shared_use;
            return e;
        }
        if (!e->tex || e->last < victim->last)
            victim = e;
    }
    e = victim;
    shared_free(e);
    if (FAILED(ID3D11Device_OpenSharedResource(s_dev, (HANDLE)handle, &IID_ID3D11Texture2D,
                                               (void **)&e->tex))
        || FAILED(ID3D11Texture2D_QueryInterface(e->tex, &IID_IDXGIKeyedMutex, (void **)&e->km))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                                        NULL, &e->srv))) {
        shared_free(e);
        return NULL;
    }
    ID3D11Texture2D_GetDesc(e->tex, &td);
    e->handle = handle;
    e->w = td.Width;
    e->h = td.Height;
    e->last = ++s_shared_use;
    return e;
}

int gpu_present_shared(void *handle, uint32_t width, uint32_t height,
                       uint32_t lines, uint32_t mode, uint32_t aa_sy)
{
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    uint32_t cb[4];
    Shared *e;

    s_lines = lines;
    if (!s_rtv)
        return -1;
    if (!s_rps) {
        D3D11_BUFFER_DESC bd;
        ID3DBlob *b = compile_src(s_resolve_hlsl, "ps", "ps_4_0");
        ZeroMemory(&bd, sizeof bd);
        bd.ByteWidth = sizeof cb;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!b || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                                        ID3D10Blob_GetBufferSize(b), NULL, &s_rps))
               || FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rcb))) {
            if (b) ID3D10Blob_Release(b);
            return -1;
        }
        ID3D10Blob_Release(b);
    }
    if (!(e = shared_get(handle)))
        return -1;
    if ((width != s_w || height != s_h) && !make_frame(width, height))
        return -1;
    /* The back end holds the key while it copies the next frame in. */
    if (IDXGIKeyedMutex_AcquireSync(e->km, 0, 100) != S_OK)
        return -1;
    cb[0] = mode; cb[1] = aa_sy ? aa_sy : 1; cb[2] = e->w; cb[3] = e->h;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_rcb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_frame_rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)s_w; vp.Height = (float)s_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_rps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_rcb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &e->srv);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    IDXGIKeyedMutex_ReleaseSync(e->km, 0);
    return show_frame();
}

void gpu_present_shutdown(void)
{
    int i;
    for (i = 0; i < 4; i++)
        shared_free(&s_shared[i]);
    if (s_rcb)   { ID3D11Buffer_Release(s_rcb);               s_rcb = NULL; }
    if (s_fcb)   { ID3D11Buffer_Release(s_fcb);               s_fcb = NULL; }
    if (s_fps)   { ID3D11PixelShader_Release(s_fps);          s_fps = NULL; }
    if (s_rps)   { ID3D11PixelShader_Release(s_rps);          s_rps = NULL; }
    if (s_frame_rtv) { ID3D11RenderTargetView_Release(s_frame_rtv); s_frame_rtv = NULL; }
    if (s_ps)    { ID3D11PixelShader_Release(s_ps);           s_ps = NULL; }
    if (s_vs)    { ID3D11VertexShader_Release(s_vs);          s_vs = NULL; }
    if (s_samp)  { ID3D11SamplerState_Release(s_samp);        s_samp = NULL; }
    if (s_srv)   { ID3D11ShaderResourceView_Release(s_srv);   s_srv = NULL; }
    if (s_frame) { ID3D11Texture2D_Release(s_frame);          s_frame = NULL; }
    if (s_rtv)   { ID3D11RenderTargetView_Release(s_rtv);     s_rtv = NULL; }
    if (s_chain) { IDXGISwapChain_Release(s_chain);           s_chain = NULL; }
    if (s_ctx)   { ID3D11DeviceContext_Release(s_ctx);        s_ctx = NULL; }
    if (s_dev)   { ID3D11Device_Release(s_dev);               s_dev = NULL; }
}

int gpu_present_active(void)
{
    return s_rtv != NULL;
}
#endif
