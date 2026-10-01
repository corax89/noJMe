/*
 * battery.c — чтение заряда аккумулятора: libnx PSM на Switch, мок из
 * окружения на хосте. См. battery.h за контрактом и историей ([BATFIX]
 * v36.41, сессия 83).
 *
 * Гвард REAL-ветки повторяет [PAD-INFO] из sdl_graphics.c: только настоящий
 * NRO-билд имеет <switch.h> — switchui-verify тоже определяет __SWITCH__,
 * но собирается хостовым gcc без libnx. __has_include обязан проверяться
 * вложенным #if (не одной строкой #if defined(__has_include) && ...) —
 * так уже сделано для PAD-INFO и это же требование GCC к __has_include.
 *
 * PSM-сессия открывается лениво, ОДИН раз, и НЕ закрывается до конца
 * процесса: это системная сессия, hbloader приберёт её при выходе, а
 * psmExit по пути аварийного выхода сам мог бы стать источником падения.
 * psmGetChargerType — [4.0.0+]; на более старомFW вернёт ошибку -> честный
 * -1 (заряд при этом по-прежнему читается отдельной командой).
 */
#include "utils/battery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined(__SWITCH__) && defined(__has_include)
#  if __has_include(<switch.h>)
#    include <switch.h>
#    define NOJME_BATT_LIBNX 1
#  endif
#endif

/* ---- общий кэш (обе ветки) ---- */
#define BATT_CACHE_MS_LIBNX 2000 /* IPC к PSM максимум раз в 2 c */
#define BATT_CACHE_MS_ENV    500 /* мок: тесты могут подменить значение */

static uint64_t batt_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct {
    int pct;        /* -1 = неизвестно */
    int charging;   /* -1 = неизвестно */
    uint64_t at_ms; /* время выборки; 0 = кэш пуст */
} BattSample;

static BattSample g_batt_cache = { -1, -1, 0 };

/* Выборка БЕЗ кэша (реальный источник данных). */
static BattSample batt_sample_once(void) {
    BattSample s;
    s.pct = -1;
    s.charging = -1;
    s.at_ms = 0;

#ifdef NOJME_BATT_LIBNX
    {
        /* ленивая инициализация PSM: -1 не пробовали, 0 ошибка, 1 готово */
        static int s_psm_state = -1;
        if (s_psm_state < 0)
            s_psm_state = (psmInitialize() == 0) ? 1 : 0;
        if (s_psm_state) {
            /* v36.42 [BATFIX2]: сигнатуры сверены с реальным libnx
             * psm.h (devkitPro, libnx Authors): процент приходит в u32*,
             * а "зарядки нет" в enum называется PsmChargerType_Unconnected
             * (=0); EnoughPower/LowPower/NotSupported (1..3) — зарядное
             * устройство подключено. */
            u32 pct = 0;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) {
                if (pct > 100) pct = 100;
                s.pct = (int)pct;
            }
            {
                PsmChargerType ct;
                if (R_SUCCEEDED(psmGetChargerType(&ct)))
                    s.charging = (ct != PsmChargerType_Unconnected) ? 1 : 0;
            }
        }
        return s;
    }
#else
    /* Хост: детерминированный мок из окружения (verify/headless-тесты).
     * Приоритет: NOJME_BATTERY_MOCK (полная пара одним ключом:
     * "85", "85:c", "85:1", "85:0"), затем отдельные
     * NOJME_BATTERY_LEVEL / NOJME_BATTERY_CHARGING. */
    {
        const char* m = getenv("NOJME_BATTERY_MOCK");
        if (m && m[0]) {
            int pct = -1, ch = -1;
            char tail[8];
            tail[0] = '\0'; /* "85" без суффикса -> sscanf вернёт 1 */
            if (sscanf(m, "%d:%7s", &pct, tail) >= 1) {
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                if (tail[0] == 'c' || tail[0] == 'C' || tail[0] == '1')
                    ch = 1;
                else if (tail[0] == '0')
                    ch = 0;
                s.pct = pct;
                s.charging = ch;
            }
        }
        if (s.pct < 0) {
            const char* l = getenv("NOJME_BATTERY_LEVEL");
            if (l && l[0]) {
                int pct = atoi(l);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                s.pct = pct;
            }
        }
        if (s.charging < 0) {
            const char* c = getenv("NOJME_BATTERY_CHARGING");
            if (c && c[0]) s.charging = (c[0] == '1' || c[0] == 'c' ||
                                          c[0] == 'C') ? 1 : 0;
        }
        return s;
    }
#endif
}

/* Кэшированная выборка. */
static const BattSample* batt_sample(void) {
#ifdef NOJME_BATT_LIBNX
    const uint64_t ttl = BATT_CACHE_MS_LIBNX;
#else
    const uint64_t ttl = BATT_CACHE_MS_ENV;
#endif
    uint64_t now = batt_now_ms();
    if (g_batt_cache.at_ms == 0 || now - g_batt_cache.at_ms >= ttl) {
        BattSample s = batt_sample_once();
        s.at_ms = now;
        /* атомарно enough: два int + счётчик времени пишутся без барьера,
         * но читатели (кадровый поток) перечитают на следующем кадре —
         * консистентность значений не критична для индикатора */
        g_batt_cache = s;
    }
    return &g_batt_cache;
}

int nojme_battery_percent(void) {
    return batt_sample()->pct;
}

int nojme_battery_charging(void) {
    return batt_sample()->charging;
}

const char* nojme_battery_source(void) {
#ifdef NOJME_BATT_LIBNX
    return "psm";
#else
    return (getenv("NOJME_BATTERY_MOCK") || getenv("NOJME_BATTERY_LEVEL") ||
            getenv("NOJME_BATTERY_CHARGING")) ? "env" : "none";
#endif
}
