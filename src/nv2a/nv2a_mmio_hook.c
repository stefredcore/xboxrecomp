/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * Decodes x86-64 MOV instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include "../platform/mmio_decode.h"
#include <stdio.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */
#define NV2A_PVIDEO_MMIO 0x8000u            /* PVIDEO's 4 KB register block */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;
static int g_trace_pvideo_mmio = -1;

/* Global APU state pointer is declared in apu.h and referenced from main.c
 * (regardless of whether the MMIO hook is active). Keep its definition
 * outside the Win32 guard. */

#if defined(_WIN32)

/* PVIDEO's registers are written by the title's thread through the MMIO
 * hook and read by the window thread for scanout. */
static SRWLOCK g_pvideo_lock = SRWLOCK_INIT;

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{
    NV2AState *state = nv2a_get_state();
    unsigned i;
    if (!state) return false;
    AcquireSRWLockShared(&g_pvideo_lock);
    for (i = 0; i < 0x1000 / 4; ++i)
        regs[i] = state->pvideo.regs[i * 4];
    ReleaseSRWLockShared(&g_pvideo_lock);
    return true;
}

/* The scanout has taken the bank: clear its pending bit in BUFFER, which is
 * what the title polls before it reuses the memory. */
void nv2a_hook_pvideo_consume(unsigned bank)
{
    NV2AState *state = nv2a_get_state();
    uint64_t buffer;
    if (!state || bank > 1) return;
    AcquireSRWLockExclusive(&g_pvideo_lock);
    buffer = nv2a_mmio_read(state, NV2A_PVIDEO_MMIO + NV_PVIDEO_BUFFER, 4);
    nv2a_mmio_write(state, NV2A_PVIDEO_MMIO + NV_PVIDEO_BUFFER,
                    buffer & ~(uint64_t)(1u << (bank * 4)), 4);
    ReleaseSRWLockExclusive(&g_pvideo_lock);
}

/* ============================================================
 * The faulting instruction: src/platform/mmio_decode.h, which the APU and
 * OHCI hooks use too. This file had a copy of its own that knew the moves,
 * TEST, CMP and the read-modify-write OR/AND only -- what MSVC emits for the
 * MEM* macros. Clang folds a register read into the ALU op that uses it
 * (`and esi, [mem]`), which the copy did not decode and the shared decoder
 * does.
 * ============================================================ */

static uint64_t hook_read(void *dev, uint32_t off, int size)
{
    g_mmio_read_count++;
    return nv2a_mmio_read((NV2AState *)dev, off, size);
}

static void hook_write(void *dev, uint32_t off, uint64_t val, int size)
{
    g_mmio_write_count++;
    nv2a_mmio_write((NV2AState *)dev, off, val, size);
}

/*
 * Try to decode and handle the faulting instruction.
 * Returns true if successfully handled, false if unrecognized.
 */
static bool decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    NV2AState *nv2a = nv2a_get_state();
    (void)is_write;
    if (!nv2a) return false;

    if (mmio_emulate(ctx, mmio_offset, nv2a, hook_read, hook_write))
        return true;

    /* Unrecognized instruction */
    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

/* ============================================================
 * Public API
 * ============================================================ */

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

    /* Allocate NV2A VRAM (64MB) */
    g_nv2a_vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_nv2a_vram) {
        fprintf(stderr, "[NV2A] Failed to allocate %u MB for VRAM!\n",
                (NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE) / (1024*1024));
        return;
    }

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;

    /* Initialize NV2A state machine */
    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE,
                         ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void*)g_nv2a_vram, (void*)ramin_ptr);

    /* The register aperture is plain memory (xbox_memory_layout.c), so a
     * title's PVIDEO writes would never reach the model: STOP would not clear
     * BUFFER, and the scanout would never see a frame. Fault that one page so
     * its accesses come here; the title's handler routes them, which is why
     * this is done only once it has installed the hook. */
    {
        DWORD old_protect;
        void *page = (void *)((uintptr_t)(NV2A_MMIO_BASE + NV2A_PVIDEO_MMIO) +
                              g_mem_offset);
        if (!VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "[NV2A] PVIDEO registers not trapped (error %lu); "
                    "the video overlay will not show\n", GetLastError());
    }
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    /* Compute MMIO offset within NV2A register space */
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;
    bool pvideo = mmio_offset >= NV2A_PVIDEO_MMIO &&
                  mmio_offset < NV2A_PVIDEO_MMIO + 0x1000u;
    bool handled;

    if (g_trace_pvideo_mmio < 0)
        g_trace_pvideo_mmio = getenv("RECOMP_TRACE_PVIDEO_MMIO") != NULL;

    if (pvideo) AcquireSRWLockExclusive(&g_pvideo_lock);
    handled = decode_and_handle(ctx, mmio_offset, is_write);
    if (pvideo && handled) {
        static unsigned trace_count;
        if (trace_count < 128) {
            uint32_t value = nv2a_get_state()->pvideo.regs[mmio_offset / 4];
            if (is_write || g_trace_pvideo_mmio) {
                fprintf(stderr,
                        "[PVIDEO] %s off=0x%04X value=0x%08X va=0x%08X fault=%p rip=%p\n",
                        is_write ? "write" : "read",
                        mmio_offset - NV2A_PVIDEO_MMIO,
                        value,
                        fault_xbox_va,
                        (void *)fault_addr,
                        (void *)ctx->Rip);
                if (++trace_count == 128)
                    fprintf(stderr, "[PVIDEO] MMIO trace capped at 128 entries\n");
                fflush(stderr);
            }
        }
    }
    if (pvideo) ReleaseSRWLockExclusive(&g_pvideo_lock);
    return handled;
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    /* For VRAM range (0xF0000000-0xF3FFFFFF), allocate pages as before.
     * In the future, we can map these to NV2A VRAM for push buffer DMA.
     * For now, just allocate writable pages. */
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
}

#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */

bool nv2a_hook_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{ (void)regs; return false; }
void nv2a_hook_pvideo_consume(unsigned bank) { (void)bank; }

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{ (void)xbox_mem_offset; }

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }

#endif /* _WIN32 */
