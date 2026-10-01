/*
 * wildguard.h — [WILDGUARD] v36.50 (yetifix7)
 *
 * Полевой детектор мусорных указателей класса Ryujinx "InvalidAccess".
 *
 * ИСТОРИЯ ВОПРОСА. Пользователь стабильно видит в Ryu/Ryujinx серию
 * "Cpu InvalidAccessHandler: Invalid memory access at virtual address
 * 0x...". Все замеченные адреса — ASCII-байты СТРОК ПУТЕЙ, прочитанные
 * как 64-битный указатель:
 *   0x20697465592F7365 = "es/Yeti "  (sdmc:/switch/j2me/games|saves/Yeti…, байты 21-28)
 *   0x636E756F422F7365 = "es/Bounc"  (…/games/Bounce Tales (240x320).jar, байты 21-28)
 *   0x6E692E73676E6974 = "tings.in"  (sdmc:/switch/j2me/settings.ini,     байты 21-28)
 * Ключевое полевое наблюдение: ошибки бывают ДО запуска любого мидлета
 * (браузер), при загрузке ДРУГОГО мидлета (адрес = фрагмент ЕГО пути) и
 * в игре. Значит, источник — не мидлет, а ФРОНТЕНД/микшер эмулятора, и
 * мусорное значение живёт короткими вспышками (3-6 чтений с шагом 1-3 мс).
 *
 * МЕТОД. Ryu подавляет такое чтение (возвращает 0 и пишет строку в лог),
 * поэтому на месте падения его не видно. Но МУСОРНОЕ ЗНАЧЕНИЕ проходит
 * через наш код как аргумент/поле ДО разыменования. WILDGUARD — дешёвая
 * проверка значения на входах ключевых точек разыменования (микшер,
 * вывод кадра, рисование текста, браузер, загрузка JAR). Порог выбран
 * так, что ЛОЖНЫХ СРАБАТЫВАНИЙ НЕТ ПО ПОСТРОЕНИЮ: пользовательский VA
 * на HOS — 39 бит, на Linux x86-64/aarch64 — 47 бит; любое значение
 * >= 2^48 корректным указателем быть не может (сюда же попадают
 * ASCII-пары и стековая/кучная полиция 0xFEFE…/0x5A5A…).
 *
 * Отчёт дублируется: sw_trace_force (log.txt при включённом «Лог»),
 * stderr (хост) и STDOUT (Ryu показывает stdout homebrew в своём логе —
 * [WILDGUARD]-строка окажется РЯДОМ со строкой InvalidAccess в том же
 * логе, даже когда «Лог» выключен). Кап 24 отчёта на процесс.
 *
 * ДЕФУЗ: проверка не только РЕПОРТИТ, но и запрещает разыменование
 * (мусорный указатель уходит в штатную NULL-ветку вызванного кода) —
 * тот же принцип, что у [ARGGUARD] v36.49 для JVM-моста.
 */
#ifndef NOJME_WILDGUARD_H
#define NOJME_WILDGUARD_H

#include <stdint.h>
#include <stddef.h>

/* Реализация отчёта — в switch_trace.c (все app-сборки). Слабая: сборки
 * без switch_trace.c (белый ящик input-теста) получают NULL-адрес и
 * просто не репортят — проверка значения всё равно бесплатна. */
extern void nojme_wg_report(const char* site, const void* val)
    __attribute__((weak));

static inline int nojme_wg_bad(const void* p) {
    /* >= 2^48 — вне пользовательского VA и HOS (39 бит), и любого
     * хост-Linux (47 бит). NULL и малые целые (хэндлы, коды) проходят. */
    return ((uintptr_t)p >= 0x0001000000000000ULL);
}

/* Возвращает 1 и репортит, если указатель мусорный; 0 — можно работать. */
static inline int nojme_wg_check(const void* p, const char* site) {
    if (p && nojme_wg_bad(p)) {
        /* -Waddress: в TU, где выше есть сильное определение nojme_wg_report
         * (switch_trace.c, тест), проверка адреса статически всегда true —
         * это ОЖИДАЕМО для слабого символа с локальным сильным дефом. */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Waddress"
#endif
        if (nojme_wg_report) nojme_wg_report(site, p);
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
        return 1;
    }
    return 0;
}

/* Только репорт (значение дальше не разыменовывается этим кодом). */
#define WILDGUARD(p, site) ((void)nojme_wg_check((const void*)(p), (site)))

/* Репорт + пропуск (ставится ПЕРЕД разыменованием — мусор уходит в
 * штатную безопасную ветку, как NULL). */
#define WILDGUARD_SKIP(p, site) nojme_wg_check((const void*)(p), (site))

#endif /* NOJME_WILDGUARD_H */
