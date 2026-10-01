/*
 * battery.h — единый источник данных о заряде аккумулятора для ВСЕХ сборок
 * (NRO / switchui-verify / headless / libretro).
 *
 * [BATFIX] v36.41 (сессия 83, запрос пользователя: «настройка с показом
 * заряда аккумулятора … и реализуй возможность чтения заряда внутри j2me
 * через один или несколько АПИ»):
 *   - Nintendo Switch (настоящий NRO): сервис PSM libnx —
 *     psmGetBatteryChargePercentage / psmGetChargerType;
 *   - хост-сборки (verify/headless/libretro): детерминированный мок из
 *     окружения NOJME_BATTERY_MOCK / NOJME_BATTERY_LEVEL — им же питаются
 *     автотесты;
 *   - данных нет -> -1 (J2ME-свойства тогда честно отдают NULL, как телефон
 *     без соответствующего API).
 *
 * Все вызовы кэшируются (2 c на Switch — IPC к PSM не нужен 60 раз в
 * секунду; 0.5 c у мока — чтобы тест мог подменить значение по ходуprocessa).
 * Потокобезопасно: только атомарные записи int-кэша (одинокие читатели в
 * потоке кадра / вызовы из VM).
 */
#ifndef NOJME_UTILS_BATTERY_H
#define NOJME_UTILS_BATTERY_H

/* Заряд в процентах 0..100; -1 = неизвестно. */
int nojme_battery_percent(void);

/* 1 = зарядное устройство подключено, 0 = от батареи, -1 = неизвестно. */
int nojme_battery_charging(void);

/* Откуда взяты данные: "psm" (libnx), "env" (мок окружения), "none". */
const char* nojme_battery_source(void);

#endif /* NOJME_UTILS_BATTERY_H */
