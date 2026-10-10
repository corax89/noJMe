#ifndef PERF_DEBUG_H
#define PERF_DEBUG_H
/*
 * v36.106 [PERF-GATE] — единый компиляционный рубильник ОТЛАДКИ
 * ПРОИЗВОДИТЕЛЬНОСТИ.
 *
 * Полевой итог v36.103..v36.105: производительности достаточно, вся
 * телеметрия, которая её искала ([VM-PERF] ivk=/fiv=/ifc=, [VM-OPS],
 * [VM-PAIRS], [GFX-PERF] nat=), больше не нужна в боевой сборке — но
 * остаётся в дереве НА БУДУЩЕЕ: следующий раунд ускорения начинается с
 * `make switchui NOJME_PERF=1` (аналогично headless/app/libretro/m17).
 *
 * Что гейтится (по умолчанию ВЫКЛ = нулевая цена горячего пути):
 *   - include/gfxperf.h: пары gfxp_begin/gfxp_end вокруг КАЖДОГО
 *     графического натива и КАЖДОЙ натив-диспетчеризации ВМ (opcodes.c
 *     gfxp_nat_enter/leave): 2 чтения счётчика + 2 атомика с вызова;
 *   - execute.c: выборка ops_hist[256] и g_vm_pairs_hist[256][256]
 *     (по инкременту на 64 команды в каденсе T_SLOW);
 *   - execute.c/opcodes.c: per-thread инкременты perf_ivk/perf_fiv/
 *     perf_ifc (по одному на каждый Java-вызов / IC-промах);
 *   - switch_trace.c: печать [VM-PERF]/[VM-OPS]/[VM-PAIRS]/[GFX-PERF]
 *     в 5-с такте диагностики.
 *
 * Что НЕ гейтится (это корректность, не профилирование): каденс T_SLOW
 * (сейфпоинты GC, EXIT-FENCE, спин-брейкер), STALL-RING, общая строка
 * «diag:» здоровья, счётчики ms-масштаба (fence/srep/budget/gcsp —
 * событийные, не на команду), [RMSSTAMP]/[RMS-SLOW].
 *
 * Значения: 0 — боевые сборки (по умолчанию), 1 — диагностика.
 * Makefile: переменная NOJME_PERF=1 добавляет -DNOJME_PERF_DEBUG=1
 * во все цели (switchui/switch/headless/app/libretro/m17).
 */
#ifndef NOJME_PERF_DEBUG
#define NOJME_PERF_DEBUG 0
#endif

#endif /* PERF_DEBUG_H */
