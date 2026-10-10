#ifndef GFXPERF_H
#define GFXPERF_H
/*
 * v36.100 [GFX-PERF] — always-on per-API wall-time telemetry for the
 * graphics natives + the TOTAL time spent inside VM native dispatches.
 *
 * WHY: the v36.99 Switch field log (Zombie Infection, heavy level) showed
 * the aggregate interpreter rate halving during gameplay (vi=8.4M/s vs
 * 17-19M/s while pure-parsing) while pss=0, gc=0, pv small — i.e. roughly
 * HALF the frame is spent inside NATIVE calls, but no counter could name
 * WHICH natives. The next 5-s diag must answer that in one field log.
 * Switch cannot pass env flags (user report) -> ALWAYS ON by design.
 *
 * v36.106 [PERF-GATE]: «always on» отменено полевым решением пользователя
 * (производительности достаточно) — теперь телеметрия за компиляционным
 * рубильником NOJME_PERF_DEBUG (include/perf_debug.h; make ... NOJME_PERF=1).
 * Боевые сборки: обёртки gfxp_begin/gfxp_end/gfxp_count выкидываются
 * препроцессором целиком — ноль чтений таймера и ноль атомиков с вызов.
 * Механика чтения счётчика (GFXP_TICK/…, v36.102 [GFXP-TICK-FIX]) остаётся
 * БЕЗУСЛОВНОЙ: scripts/gfxp_cross_test.c проверяет кодировки mrs обоих
 * путей (компилируется с -DNOJME_PERF_DEBUG=1).
 *
 * SEMANTICS of the ids (count is calls, us is accumulated wall time):
 *   DI    drawImage          (total; O = memcpy opaque sub-path, count only)
 *   DR    drawRegion         (total; ROT = rot90/270/general sub-path, count only)
 *   DRO   drawRGB processAlpha=0
 *   DRAS  drawRGB processAlpha=1 on SCREEN gfx (fast span, v36.100)
 *   DRAI  drawRGB processAlpha=1 on image gfx (exact scalar formula)
 *   GR    Image.getRGB
 *   CRGB  Image.createRGBImage
 *   CI    Image.createImage(w,h) mutable
 *   PRIM  fillRect/drawRect/drawLine/fillTriangle/drawString & friends
 *   FL    GameCanvas flushGraphics blits
 *   NAT   ALL VM native dispatches (depth-0 only; see opcodes.c)
 * Nested calls (a graphics native that re-enters Java and dispatches more
 * natives, e.g. LayerManager.paint) count once at the OUTER dispatch for
 * NAT and separately per API for the graphics ids — nat_ms can therefore
 * be slightly smaller than the sum of graphics ids in rare mixed cases.
 */
#include <stdint.h>
#include "perf_debug.h" /* v36.106 [PERF-GATE]: NOJME_PERF_DEBUG */

/* v36.101 [GFXP-TICK] + v36.102 [GFXP-TICK-FIX]: on aarch64 the timing
 * pair reads the generic timer counters directly. WHICH counter is
 * platform-dependent — this is the v36.102 field fix:
 *
 *   - Horizon (Switch, __SWITCH__): the PHYSICAL counter CNTPCT_EL0.
 *     This is byte-for-byte what libnx's armGetSystemTick() reads
 *     (nx/include/switch/arm/counter.h). The VIRTUAL counter CNTVCT_EL0
 *     is NOT EL0-accessible on Horizon (CNTKCTL_EL1.EL0VCTEN=0): the
 *     v36.101 build read it and the FIRST wrapped native dispatch of
 *     the FIRST midlet launch trapped — Atmosphere crash report
 *     0x4A8/2168-0002, fault PC byte-verified as "mrs x5, cntvct_el0"
 *     inside op_invokestatic's dispatch wrapper (native target
 *     native_display_getDisplay, strcmp leftovers of
 *     ".../lcdui/Display" still in x16/x17).
 *   - Generic aarch64 userspace (Linux/BSD, e.g. the libretro core):
 *     the VIRTUAL counter CNTVCT_EL0 — the one kernels expose to EL0
 *     there (the Linux vDSO reads CNTVCT_EL0; CNTPCT is EL0-gated).
 *
 * Rationale (unchanged): clock_gettime() on devkitA64 may cost a full
 * SVC round trip (~1 us); the always-on GFX-PERF telemetry wraps EVERY
 * VM native dispatch (field-observed up to ~31K dispatches/s in Zombie
 * gameplay, ~10K/s while loading) — 2 x 1 us per dispatch is up to ~6%
 * of the frame budget. Register reads cost ~2 x 10 ns. Deltas are
 * converted with 64-bit math AFTER subtraction and clamped to 68 s
 * worth of ticks so dt*1e6 can never overflow (saturation then matches
 * the original 32-bit clamp). Non-aarch64 builds keep clock_gettime
 * (vDSO ~25 ns).
 *
 * CNTFRQ_EL0 stays as-is on BOTH platforms: it is the same register
 * libnx's armGetSystemTickFreq() reads and it is not EL0-gated.
 * switchui-verify (host x86 + -D__SWITCH__) is unaffected: the outer
 * __aarch64__ gate is false there, so it keeps the clock_gettime path.
 */
#if defined(__aarch64__)
#if defined(__SWITCH__)
#define GFXP_TICK_MRS "cntpct_el0"
#else
#define GFXP_TICK_MRS "cntvct_el0"
#endif
#define GFXP_TICK_CLOCK 1
#endif

#ifdef GFXP_TICK_CLOCK
static inline uint64_t gfxp_tick_raw(void) {
    uint64_t t;
    __asm__ volatile("mrs %0, " GFXP_TICK_MRS : "=r"(t));
    return t;
}
static inline uint64_t gfxp_tick_freq(void) {
    /* Cached once per DSO; CNTFRQ_EL0 is architecturally EL0-readable.
     * Fallback 19.2 MHz = the documented Horizon timer frequency if the
     * register reads implausible (0 or absurd) — only diag scale shifts. */
    static uint64_t s_freq;
    uint64_t f = s_freq;
    if (__builtin_expect(f == 0, 0)) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        if (f < 1000000ULL || f > 10000000000ULL) f = 19200000ULL;
        s_freq = f;
    }
    return f;
}
#endif

enum {
    GFXP_DI = 0,
    GFXP_DI_O,
    GFXP_DR,
    GFXP_DR_ROT,
    GFXP_DRO,
    GFXP_DRAS,
    GFXP_DRAI,
    GFXP_GR,
    GFXP_CRGB,
    GFXP_CI,
    GFXP_PRIM,
    GFXP_FL,
    GFXP_NAT,
    GFXP_COUNT
};

/* Consumers that only PRINT the counters (switch_trace.c and its whitebox
 * link variants without the core) define GFXPERF_NO_COUNTERS first: they
 * get the enum but not the strong externs (the diag re-declares them weak). */
#if NOJME_PERF_DEBUG
#ifndef GFXPERF_NO_COUNTERS

#include <time.h>

/* Defined in render.c (core). */
extern volatile uint32_t g_gfxp_n[GFXP_COUNT];
extern volatile uint32_t g_gfxp_us[GFXP_COUNT];

static inline uint64_t gfxp_now_us(void) {
#ifdef GFXP_TICK_CLOCK
    /* Tick domain — only meaningful as a begin/end DIFFERENCE (see
     * gfxp_end). Returned raw so the subtraction avoids the division. */
    return gfxp_tick_raw();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

/* Begin/end pair. */
static inline uint64_t gfxp_begin(void) { return gfxp_now_us(); }
static inline void gfxp_end(int id, uint64_t t0) {
    if (id < 0 || id >= GFXP_COUNT) return;
#ifdef GFXP_TICK_CLOCK
    {
        uint64_t dt = gfxp_tick_raw() - t0;          /* ticks (wrap-safe) */
        uint64_t f = gfxp_tick_freq();
        if (dt > f * 68ULL) dt = f * 68ULL;          /* keep dt*1e6 in u64 */
        uint32_t d = (uint32_t)((dt * 1000000ULL) / f);
        if (d > 0xFFFFFFFFu / 64u) d = 0xFFFFFFFFu / 64u; /* saturate */
        __sync_fetch_and_add(&g_gfxp_n[id], 1);
        __sync_fetch_and_add(&g_gfxp_us[id], d);
    }
#else
    uint32_t d = (uint32_t)(gfxp_now_us() - t0);
    if (d > 0xFFFFFFFFu / 64u) d = 0xFFFFFFFFu / 64u; /* saturate 32-bit acc */
    __sync_fetch_and_add(&g_gfxp_n[id], 1);
    __sync_fetch_and_add(&g_gfxp_us[id], d);
#endif
}

/* Count-only probe (sub-path markers inside shared pixel engines, where a
 * full timing pair would need hooks on every early-return). */
static inline void gfxp_count(int id) {
    if (id < 0 || id >= GFXP_COUNT) return;
    __sync_fetch_and_add(&g_gfxp_n[id], 1);
}

#endif /* GFXPERF_NO_COUNTERS */

#else /* !NOJME_PERF_DEBUG: боевые сборки — обёртки выкидываются целиком */

#ifndef GFXPERF_NO_COUNTERS
static inline uint64_t gfxp_begin(void) { return 0; }
static inline void gfxp_end(int id, uint64_t t0) { (void)id; (void)t0; }
static inline void gfxp_count(int id) { (void)id; }
#endif

#endif /* NOJME_PERF_DEBUG */
#endif /* GFXPERF_H */
