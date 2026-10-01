/* ==========================================================================
 * core_options.c — v34.82: локализация настроек ядра (libretro core options)
 * ==========================================================================
 * Пользовательский запрос: «добавь для libretro-ядра перевод настроек на
 * русский». Фронтенд выбирает язык через RETRO_ENVIRONMENT_GET_LANGUAGE (39);
 * ядро регистрирует описания опций через официальный интернациональный путь:
 *
 *   1) GET_CORE_OPTIONS_VERSION (52) == 0  -> только legacy SET_VARIABLES(16)
 *      (MinArch и другие до-libretro-1.4 фронтенды);
 *   2) язык == RETRO_LANGUAGE_RUSSIAN      -> SET_CORE_OPTIONS_INTL (54)
 *      { us = английская таблица, local = русская таблица };
 *   3) иначе                                 -> SET_CORE_OPTIONS (53)
 *      (английская таблица, v1-структуры — официальная раскладка);
 *   4) если 54/53 отвергнуты фронтендом     -> legacy SET_VARIABLES
 *      (таблица g_core_variables из libretro.c).
 *
 * v34.82 ФИКС СЕГФОЛТА (m17): в v34.81 struct retro_core_option_definition в
 * include/libretro.h имел 'values' как УКАЗАТЕЛЬ на массив значений, тогда
 * как официальный v1-контракт (env 53/54) требует ВЛОЖЕННЫЙ массив
 * values[RETRO_NUM_CORE_OPTION_VALUES_MAX]. Фронтенд m17 читал поля по
 * официальным смещениям: key/desc/info совпали (лог «INIT Разрешение
 * экрана (j2me_resolution) TO auto (…мусор…)»), но на смещении values он
 * читал БАЙТЫ УКАЗАТЕЛЯ как массив value/label-пар, а следующий def — со
 * шагом ~1 КБ вместо 40 байт — уходил в чужую статическую память ->
 * SIGSEGV в OptionList_init. Теперь раскладка побитово официальная и
 * зафиксирована _Static_assert-ами ниже; lr_run (scripts/lr_run.c) ходит
 * по таблицам ЗАКОНОВ ФРОНТЕНДА (полный проход values с проверками
 * терминаторов и дефолтов) — рассинхрон раскладки больше не пройдёт
 * незаметно.
 *
 * КЛЮЧИ, машинные ЗНАЧЕНИЯ и дефолты опций не меняются (j2me_resolution /
 * "auto" / "240x320" ...), локализуются только desc/info/label — сохранённые
 * пресеты фронтендов продолжают работать. Дефолт всегда равен ПЕРВОМУ
 * значению (конвенция v1-фронтендов, игнорирующих default_value).
 * ========================================================================== */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libretro.h"

/* ------------------------------------------------------------------------
 * Компиляционная фиксация официальной v1-раскладки (страховка от повторения
 * бага v34.81 — 'values'-указатель вместо вложенного массива):
 *   struct retro_core_option_value      = ровно 2 указателя (value, label);
 *   struct retro_core_option_definition = key+desc+info+values[128]+default,
 *                                         без паддинга (все поля — указатели).
 * ------------------------------------------------------------------------ */
_Static_assert(sizeof(struct retro_core_option_value) ==
                   2 * sizeof(const char*),
               "v1 layout: retro_core_option_value = {value, label} pointers");
_Static_assert(sizeof(struct retro_core_option_definition) ==
                   (4 + 2 * RETRO_NUM_CORE_OPTION_VALUES_MAX) * sizeof(const char*),
               "v1 layout: key+desc+info+values[128]+default_value, no padding");
_Static_assert(offsetof(struct retro_core_option_definition, values) ==
                   3 * sizeof(const char*),
               "v1 layout: values array starts right after info");
_Static_assert(offsetof(struct retro_core_option_definition, default_value) ==
                   3 * sizeof(const char*) +
                       RETRO_NUM_CORE_OPTION_VALUES_MAX *
                       sizeof(struct retro_core_option_value),
               "v1 layout: default_value is the last field");

/* ------------------------------------------------------------------------
 * Английская таблица (us) — values ВЛОЖЕНЫ в каждое определение
 * ------------------------------------------------------------------------ */
static const struct retro_core_option_definition g_core_options_us[] = {
    {
        "j2me_resolution", "Screen Resolution",
        "Game canvas size. \"Auto\" takes the size from the JAR manifest hint "
        "(fixed-resolution builds start correctly); without a hint 240x320 "
        "is used. Changing it requires reloading the game.",
        {
            { "auto",    "Auto (from JAR manifest)" },
            { "240x320", NULL }, { "240x136", NULL }, { "480x272", NULL },
            { "320x480", NULL }, { "360x640", NULL }, { "480x800", NULL },
            { "176x220", NULL }, { "176x208", NULL }, { "132x176", NULL },
            { "128x160", NULL }, { "128x128", NULL }, { "320x240", NULL },
            { "640x360", NULL }, { "640x480", NULL }, { "800x480", NULL },
            { "240x160", NULL }, { "220x176", NULL }, { "208x176", NULL },
            { "176x132", NULL }, { "160x128", NULL }, { "208x208", NULL },
            { "176x176", NULL },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_fps", "Frame Rate",
        "Frames per second the frontend paces the core at. Also scales the "
        "per-frame VM instruction budget so the emulated CPU speed stays "
        "constant.",
        {
            { "30", "30 fps" }, { "60", "60 fps" }, { "15", "15 fps" },
            { "20", "20 fps" },
            { NULL, NULL }
        },
        "30"
    },
    {
        "j2me_audio_rate", "Audio Sample Rate",
        "Output sample rate of the software audio mixer (22050 Hz is the "
        "MIDP-classic default).",
        {
            { "22050", "22050 Hz" }, { "44100", "44100 Hz" },
            { "11025", "11025 Hz" },
            { NULL, NULL }
        },
        "22050"
    },
    {
        "j2me_scaling", "Screen Scaling",
        "Output picture scaling mode (presentation only, the game canvas "
        "size does not change).",
        {
            { "Aspect", "Keep aspect ratio" }, { "Integer", "Integer scale" },
            { "Stretch", "Stretch to screen" },
            { NULL, NULL }
        },
        "Aspect"
    },
    {
        "j2me_vm_speed", "VM Speed",
        "Emulated CPU speed: the instruction budget the Java VM gets per "
        "frontend frame. \"Fast\" approximates a mid-range 2005 phone.",
        {
            { "fast",     "Fast (~5.4M bytecodes/s)" },
            { "original", "Original (~0.9M, old timing)" },
            { "normal",   "Normal (~2.7M)" },
            { "turbo",    "Turbo (8 ms slices, no cap)" },
            { NULL, NULL }
        },
        "fast"
    },
    {
        "j2me_neon", "NEON Renderer (ARM)",
        "Master switch for the NEON (ARM) rasterizer and 2D helpers. Turn "
        "off to force scalar paths when debugging video issues on ARM "
        "devices.",
        {
            { "on", "Enabled" }, { "off", "Disabled (scalar paths)" },
            { NULL, NULL }
        },
        "on"
    },
    {
        "j2me_texture_smoothing", "Texture Smoothing (3D)",
        "M3G texture filtering override. \"Auto\" follows each texture's "
        "own request (v34.78 bit-exact behavior); \"Nearest\" forces crisp "
        "texels everywhere (fastest); \"Smooth\" forces bilinear filtering "
        "everywhere — softened magnified textures via the fixed-point "
        "sampler and the bit-exact NEON/SSE2 span path.",
        {
            { "auto",    "Auto (per-texture, game request)" },
            { "nearest", "Crisp texels (nearest, fastest)" },
            { "smooth",  "Smoothed (bilinear everywhere)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_touch_input", "Touch Input (stylus games)",
        "Pointer/touch event delivery for stylus-only midlets: frontend "
        "touchscreen and mouse, plus the virtual gamepad cursor.",
        {
            { "on", "Enabled" }, { "off", "Disabled" },
            { NULL, NULL }
        },
        "on"
    },
    {
        "j2me_rotation", "Screen Rotation",
        "Rotates the outgoing frame 90 degrees for portrait candybar games "
        "on landscape screens (and vice versa). Presentation only — the "
        "midlet canvas does not change; touch coordinates are mapped back.",
        {
            { "off", "Off" }, { "90 right", "90 degrees right (CW)" },
            { "90 left", "90 degrees left (CCW)" },
            { NULL, NULL }
        },
        "off"
    },
    {
        "j2me_pixel_format", "Pixel Format",
        "Output pixel format: RGB565 halves bus/memory traffic (default); "
        "RGB888 gives the full color range for dithering-sensitive screens "
        "and shaders. Applied on the next game load.",
        {
            { "RGB565", "RGB565 (16-bit)" },
            { "RGB888", "RGB888 / XRGB8888 (32-bit)" },
            { NULL, NULL }
        },
        "RGB565"
    },
    {
        "j2me_touch_mode", "Touch Cursor Mode",
        "Virtual on-screen cursor for touchscreen-only games, driven from "
        "the gamepad. \"Auto\" wakes the cursor when the analog stick moves "
        "and hides it after 1.5 s of inactivity; the two other modes keep "
        "the cursor always on — for touch-only games and for pads without "
        "an analog stick (D-Pad moves the cursor).",
        {
            { "auto",  "Auto (stick wakes the cursor)" },
            { "stick", "Analog stick — cursor always on" },
            { "dpad",  "D-Pad — cursor always on" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_touch_button", "Touch Tap Button",
        "Which gamepad button taps the virtual touch cursor (press-and-hold "
        "drags). While the cursor is active this button is not sent to the "
        "game as a key, so touch-only midlets do not receive double input.",
        {
            { "a", "A (Fire)" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
            { "l", "L" }, { "r", "R" }, { "l2", "L2" }, { "r2", "R2" },
            { NULL, NULL }
        },
        "a"
    },
    {
        "j2me_touch_speed", "Touch Cursor Speed",
        "Movement speed of the virtual touch cursor (pixels per frame at "
        "60 fps). Full analog deflection moves about twice as fast.",
        {
            { "slow",   "Slow" }, { "normal", "Normal (default)" },
            { "fast",   "Fast" },
            { NULL, NULL }
        },
        "normal"
    },
    { NULL, NULL, NULL, { { NULL, NULL } }, NULL }
};

/* ------------------------------------------------------------------------
 * Русская таблица (local) — ключи, машинные значения и дефолты те же;
 * values вложены, как требует официальный v1-контракт
 * ------------------------------------------------------------------------ */
static const struct retro_core_option_definition g_core_options_ru[] = {
    {
        "j2me_resolution", "Разрешение экрана",
        "Размер игрового канваса. «Авто» берёт размер из подсказки в "
        "манифесте JAR (игры с фиксированным разрешением стартуют "
        "корректно); без подсказки — 240x320. Смена разрешения требует "
        "перезагрузки игры.",
        {
            { "auto",    "Авто (по манифесту JAR)" },
            { "240x320", NULL }, { "240x136", NULL }, { "480x272", NULL },
            { "320x480", NULL }, { "360x640", NULL }, { "480x800", NULL },
            { "176x220", NULL }, { "176x208", NULL }, { "132x176", NULL },
            { "128x160", NULL }, { "128x128", NULL }, { "320x240", NULL },
            { "640x360", NULL }, { "640x480", NULL }, { "800x480", NULL },
            { "240x160", NULL }, { "220x176", NULL }, { "208x176", NULL },
            { "176x132", NULL }, { "160x128", NULL }, { "208x208", NULL },
            { "176x176", NULL },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_fps", "Частота кадров",
        "Количество кадров в секунду, с которым фронтенд вызывает ядро. "
        "Заодно масштабирует бюджет инструкций VM на кадр — скорость "
        "эмулируемого процессора остаётся постоянной.",
        {
            { "30", "30 к/с" }, { "60", "60 к/с" }, { "15", "15 к/с" },
            { "20", "20 к/с" },
            { NULL, NULL }
        },
        "30"
    },
    {
        "j2me_audio_rate", "Частота дискретизации звука",
        "Выходная частота программного аудио-микшера (22050 Гц — "
        "классика MIDP).",
        {
            { "22050", "22050 Гц" }, { "44100", "44100 Гц" },
            { "11025", "11025 Гц" },
            { NULL, NULL }
        },
        "22050"
    },
    {
        "j2me_scaling", "Масштабирование экрана",
        "Режим масштабирования выходной картинки (только показ — размер "
        "канваса игры не меняется).",
        {
            { "Aspect", "По пропорциям" }, { "Integer", "Целочисленный масштаб" },
            { "Stretch", "Растянуть на экран" },
            { NULL, NULL }
        },
        "Aspect"
    },
    {
        "j2me_vm_speed", "Скорость виртуальной машины",
        "Скорость эмулируемого процессора: бюджет инструкций Java-VM на "
        "кадр фронтенда. «Быстрая» примерно соответствует среднему "
        "телефону 2005 года.",
        {
            { "fast",     "Быстрая (~5,4 млн байткод/с)" },
            { "original", "Оригинальная (~0,9 млн, старый темп)" },
            { "normal",   "Обычная (~2,7 млн)" },
            { "turbo",    "Турбо (8 мс на кадр, без лимита)" },
            { NULL, NULL }
        },
        "fast"
    },
    {
        "j2me_neon", "NEON-рендер (ARM)",
        "Главный выключатель NEON-растеризатора (ARM) и 2D-помощников. "
        "Выключайте для принудительных скалярных путей при отладке "
        "видео-проблем на ARM-устройствах.",
        {
            { "on", "Включён" }, { "off", "Выключен (скалярные пути)" },
            { NULL, NULL }
        },
        "on"
    },
    {
        "j2me_texture_smoothing", "Сглаживание текстур (3D)",
        "Переопределение фильтрации текстур M3G. «Авто» следует запросу "
        "каждой текстуры (бит-в-бит поведение v34.78); «Резкие тексели» — "
        "принудительный nearest везде (быстрее всего); «Сглаженные» — "
        "принудительная билинейная фильтрация: размягчает увеличенные "
        "текстуры через табличный сэмплер и бит-точный NEON/SSE2-путь.",
        {
            { "auto",    "Авто (по запросу игры)" },
            { "nearest", "Резкие тексели (быстрее всех)" },
            { "smooth",  "Сглаженные (билинейно всегда)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_touch_input", "Сенсорный ввод (тач-игры)",
        "Доставка pointer/touch-событий для мидлетов, рассчитанных только "
        "на стилус: тачскрин и мышь фронтенда плюс виртуальный курсор "
        "геймпада.",
        {
            { "on", "Включён" }, { "off", "Выключен" },
            { NULL, NULL }
        },
        "on"
    },
    {
        "j2me_rotation", "Поворот экрана",
        "Поворачивает выходной кадр на 90° — для портретных «кнопочных» "
        "игр на альбомных экранах (и наоборот). Только показ: канвас "
        "мидлета не меняется, координаты касаний отображаются обратно.",
        {
            { "off", "Выключен" }, { "90 right", "90° по часовой стрелке" },
            { "90 left", "90° против часовой стрелки" },
            { NULL, NULL }
        },
        "off"
    },
    {
        "j2me_pixel_format", "Формат пикселей",
        "Формат выходных пикселей: RGB565 вдвое меньше грузит шину/память "
        "(по умолчанию); RGB888 — полный диапазон цвета для "
        "дизеринг-чувствительных экранов и шейдеров. Применяется при "
        "следующей загрузке игры.",
        {
            { "RGB565", "RGB565 (16 бит)" },
            { "RGB888", "RGB888 / XRGB8888 (32 бита)" },
            { NULL, NULL }
        },
        "RGB565"
    },
    {
        "j2me_touch_mode", "Режим сенсорного курсора",
        "Виртуальный курсор на экране для игр, рассчитанных только на "
        "тачскрин, — управляется геймпадом. «Авто» будит курсор движением "
        "аналогового стика и гасит его через 1,5 с бездействия; два других "
        "режима держат курсор всегда активным — для чисто тач-игр и для "
        "падов без стика (курсор двигает крестовина).",
        {
            { "auto",  "Авто (стик будит курсор)" },
            { "stick", "Аналоговый стик — курсор всегда активен" },
            { "dpad",  "Крестовина — курсор всегда активен" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        "j2me_touch_button", "Кнопка касания",
        "Какая кнопка геймпада «тапает» виртуальный сенсорный курсор "
        "(удержание = перетаскивание). Пока курсор активен, эта кнопка не "
        "отправляется в игру как клавиша — тач-мидлеты не получают "
        "двойной ввод.",
        {
            { "a", "A (Fire)" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
            { "l", "L" }, { "r", "R" }, { "l2", "L2" }, { "r2", "R2" },
            { NULL, NULL }
        },
        "a"
    },
    {
        "j2me_touch_speed", "Скорость сенсорного курсора",
        "Скорость перемещения виртуального курсора (пикселей за кадр при "
        "60 к/с). Полное отклонение стика ускоряет движение примерно "
        "вдвое.",
        {
            { "slow",   "Медленная" }, { "normal", "Обычная (по умолчанию)" },
            { "fast",   "Быстрая" },
            { NULL, NULL }
        },
        "normal"
    },
    { NULL, NULL, NULL, { { NULL, NULL } }, NULL }
};

/* ------------------------------------------------------------------------
 * Регистрация с фоллбэк-цепочкой
 * ------------------------------------------------------------------------ */
bool j2me_register_core_options(retro_environment_t cb,
                                 const struct retro_variable* legacy_vars) {
    if (!cb) return false;

    /* До-libretro-1.4 фронтенд: только legacy SET_VARIABLES */
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) &&
        version == 0) {
        return cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)legacy_vars);
    }

    /* Язык интерфейса фронтенда */
    unsigned lang = RETRO_LANGUAGE_ENGLISH;
    bool have_lang = cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &lang);
    bool russian = have_lang && (lang == (unsigned)RETRO_LANGUAGE_RUSSIAN);

    if (russian) {
        /* static: фронтенд может держать указатель после возврата из
         * коллбэка (ленивое построение списка опций) */
        static struct retro_core_options_intl intl;
        intl.us = (struct retro_core_option_definition*)g_core_options_us;
        intl.local = (struct retro_core_option_definition*)g_core_options_ru;
        if (cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &intl)) {
            return true;
        }
        /* INTL отвергнут — попробуем хотя бы английскую v1-таблицу */
    }

    if (cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,
           (void*)g_core_options_us)) {
        return true;
    }

    /* Последний рубеж — legacy-массив (MinArch) */
    return cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)legacy_vars);
}
