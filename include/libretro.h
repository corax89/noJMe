/*
 * Libretro API - Core Interface for Emulators
 * Based on standard libretro.h specification
 */

#ifndef LIBRETRO_H
#define LIBRETRO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RetroAPI version */
#define RETRO_API_VERSION 1

/* Environment flags */
#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
#define RETRO_ENVIRONMENT_PRIVATE      0x20000

/* Environment commands - CORRECT VALUES from specification */
#define RETRO_ENVIRONMENT_SET_ROTATION          1
#define RETRO_ENVIRONMENT_GET_OVERSCAN          2
#define RETRO_ENVIRONMENT_GET_CAN_DUPE          3
#define RETRO_ENVIRONMENT_SET_MESSAGE           6
#define RETRO_ENVIRONMENT_SHUTDOWN              7
#define RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL 8
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY  9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT      10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS 11
#define RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK 12
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE 13
#define RETRO_ENVIRONMENT_SET_HW_RENDER         14
#define RETRO_ENVIRONMENT_GET_VARIABLE          15
#define RETRO_ENVIRONMENT_SET_VARIABLES         16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE   17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME   18
#define RETRO_ENVIRONMENT_GET_LIBRETRO_PATH     19
#define RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK 21
#define RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK    22
#define RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE  23
#define RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES 24
#define RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE      (25 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE      (26 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE     27
#define RETRO_ENVIRONMENT_GET_PERF_INTERFACE    28
#define RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE 29
#define RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY 30
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY    31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO    32
#define RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK 33
#define RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO    34
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO   35
#define RETRO_ENVIRONMENT_SET_MEMORY_MAPS       (36 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_GEOMETRY          37
#define RETRO_ENVIRONMENT_GET_USERNAME          38
#define RETRO_ENVIRONMENT_GET_LANGUAGE          39
#define RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER (40 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE (41 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS (42 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE (43 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS 44
#define RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT (44 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_VFS_INTERFACE    (45 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_LED_INTERFACE    (46 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_MIDI_INTERFACE   (48 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_FASTFORWARDING   (49 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE (50 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS   (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS      53
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL 54
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY 55
#define RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER 56
#define RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION 57
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE 58
#define RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
#define RETRO_ENVIRONMENT_SET_MESSAGE_EXT      60
#define RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS  61
#define RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK 62
#define RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY 63
#define RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE 64
#define RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE 65
#define RETRO_ENVIRONMENT_GET_GAME_INFO_EXT    66
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2  67
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL 68
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK 69
#define RETRO_ENVIRONMENT_SET_VARIABLE         70
#define RETRO_ENVIRONMENT_GET_THROTTLE_STATE   (71 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT (72 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT (73 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_JIT_CAPABLE      74
#define RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE (75 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_DEVICE_POWER     (77 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE 78
#define RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY 79
#define RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY 80
#define RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE (81 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/* Pixel formats */
typedef enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565   = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN  = INT_MAX
} retro_pixel_format_t;

/* Device types */
#define RETRO_DEVICE_NONE         0
#define RETRO_DEVICE_JOYPAD       1
#define RETRO_DEVICE_MOUSE        2
#define RETRO_DEVICE_KEYBOARD     3
#define RETRO_DEVICE_LIGHTGUN     4
#define RETRO_DEVICE_ANALOG       5
#define RETRO_DEVICE_POINTER      6

/* v34.34: mouse/pointer input IDs (missing in this bundled legacy header;
 * guarded so a newer system libretro.h takes precedence) */
#ifndef RETRO_DEVICE_ID_MOUSE_X
#define RETRO_DEVICE_ID_MOUSE_X                0
#define RETRO_DEVICE_ID_MOUSE_Y                1
#define RETRO_DEVICE_ID_MOUSE_LEFT             2
#define RETRO_DEVICE_ID_MOUSE_RIGHT            3
#define RETRO_DEVICE_ID_MOUSE_WHEELUP          4
#define RETRO_DEVICE_ID_MOUSE_WHEELDOWN        5
#define RETRO_DEVICE_ID_MOUSE_MIDDLE           6
#endif
#ifndef RETRO_DEVICE_ID_POINTER_X
#define RETRO_DEVICE_ID_POINTER_X              0
#define RETRO_DEVICE_ID_POINTER_Y              1
#define RETRO_DEVICE_ID_POINTER_PRESSED        2
#endif

/* Joypad buttons */
#define RETRO_DEVICE_ID_JOYPAD_B      0
#define RETRO_DEVICE_ID_JOYPAD_Y      1
#define RETRO_DEVICE_ID_JOYPAD_SELECT 2
#define RETRO_DEVICE_ID_JOYPAD_START  3
#define RETRO_DEVICE_ID_JOYPAD_UP     4
#define RETRO_DEVICE_ID_JOYPAD_DOWN   5
#define RETRO_DEVICE_ID_JOYPAD_LEFT   6
#define RETRO_DEVICE_ID_JOYPAD_RIGHT  7
#define RETRO_DEVICE_ID_JOYPAD_A      8
#define RETRO_DEVICE_ID_JOYPAD_X      9
#define RETRO_DEVICE_ID_JOYPAD_L      10
#define RETRO_DEVICE_ID_JOYPAD_R      11
#define RETRO_DEVICE_ID_JOYPAD_L2     12
#define RETRO_DEVICE_ID_JOYPAD_R2     13
#define RETRO_DEVICE_ID_JOYPAD_L3     14
#define RETRO_DEVICE_ID_JOYPAD_R3     15

/* v34.34: analog stick IDs (missing in this legacy header) */
#ifndef RETRO_DEVICE_ID_ANALOG_X
#define RETRO_DEVICE_ID_ANALOG_X    0
#define RETRO_DEVICE_ID_ANALOG_Y    1
#endif

/* Keyboard keys (subset) */
#define RETROK_UNKNOWN      0
#define RETROK_RETURN       13
#define RETROK_ESCAPE       27
#define RETROK_SPACE        32
#define RETROK_LEFT         1073741904
#define RETROK_RIGHT        1073741903
#define RETROK_UP           1073741906
#define RETROK_DOWN         1073741905
#define RETROK_0            48
#define RETROK_1            49
#define RETROK_2            50
#define RETROK_3            51
#define RETROK_4            52
#define RETROK_5            53
#define RETROK_6            54
#define RETROK_7            55
#define RETROK_8            56
#define RETROK_9            57
#define RETROK_a            97
#define RETROK_b            98
#define RETROK_c            99
#define RETROK_d            100
#define RETROK_e            101
#define RETROK_f            102
#define RETROK_g            103
#define RETROK_h            104
#define RETROK_i            105
#define RETROK_j            106
#define RETROK_k            107
#define RETROK_l            108
#define RETROK_m            109
#define RETROK_n            110
#define RETROK_o            111
#define RETROK_p            112
#define RETROK_q            113
#define RETROK_r            114
#define RETROK_s            115
#define RETROK_t            116
#define RETROK_u            117
#define RETROK_v            118
#define RETROK_w            119
#define RETROK_x            120
#define RETROK_y            121
#define RETROK_z            122
#define RETROK_ASTERISK     42
#define RETROK_HASH         35
#define RETROK_F1           1073741882
#define RETROK_F2           1073741883
#define RETROK_F3           1073741884
#define RETROK_F4           1073741885
#define RETROK_F5           1073741886

/* Key modifiers */
#define RETROKMOD_NONE      0x0000
#define RETROKMOD_SHIFT     0x01
#define RETROKMOD_CTRL      0x02
#define RETROKMOD_ALT       0x04
#define RETROKMOD_META      0x08
#define RETROKMOD_NUMLOCK   0x10
#define RETROKMOD_CAPSLOCK  0x20
#define RETROKMOD_SCROLLOCK 0x40

/* Game info structure */
typedef struct retro_game_info {
    const char* path;
    const void* data;
    size_t size;
    const char* meta;
} retro_game_info;

/* System AV info */
struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float aspect_ratio;
};

struct retro_system_timing {
    double fps;
    double sample_rate;
};

typedef struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
} retro_system_av_info;

/* System info */
typedef struct retro_system_info {
    const char* library_name;
    const char* library_version;
    const char* valid_extensions;
    bool need_fullpath;
    bool block_extract;
} retro_system_info;

/* Variable */
typedef struct retro_variable {
    const char* key;
    const char* value;
} retro_variable;

/* v34.81: RETRO_ENVIRONMENT_GET_LANGUAGE (39) return values.
 * Official libretro.h ordering (stable since libretro API 5); guard for
 * hosts that bundle their own complete header. */
#ifndef RETRO_LANGUAGE_ENGLISH
enum retro_language {
    RETRO_LANGUAGE_ENGLISH = 0,
    RETRO_LANGUAGE_JAPANESE,
    RETRO_LANGUAGE_FRENCH,
    RETRO_LANGUAGE_SPANISH,
    RETRO_LANGUAGE_GERMAN,
    RETRO_LANGUAGE_ITALIAN,
    RETRO_LANGUAGE_DUTCH,
    RETRO_LANGUAGE_PORTUGUESE_BRAZIL,
    RETRO_LANGUAGE_PORTUGUESE_PORTUGAL,
    RETRO_LANGUAGE_RUSSIAN,
    RETRO_LANGUAGE_KOREAN,
    RETRO_LANGUAGE_CHINESE_SIMPLIFIED,
    RETRO_LANGUAGE_CHINESE_TRADITIONAL,
    RETRO_LANGUAGE_ESPERANTO,
    RETRO_LANGUAGE_POLISH,
    RETRO_LANGUAGE_VIETNAMESE,
    RETRO_LANGUAGE_ARABIC,
    RETRO_LANGUAGE_GREEK,
    RETRO_LANGUAGE_TURKISH,
    RETRO_LANGUAGE_SLOVAK,
    RETRO_LANGUAGE_PERSIAN,
    RETRO_LANGUAGE_HEBREW,
    RETRO_LANGUAGE_ASTURIAN,
    RETRO_LANGUAGE_FINNISH,
    RETRO_LANGUAGE_DUMMY = INT_MAX /* guard against partial enums */
};
#endif /* RETRO_LANGUAGE_ENGLISH */

/* Modern core options API (V1/V2)
 *
 * v34.82 FIX: 'values' must be an INLINE array of
 * RETRO_NUM_CORE_OPTION_VALUES_MAX entries — the official v1 layout as
 * parsed by every v1-era frontend (env 53 SET_CORE_OPTIONS / env 54
 * SET_CORE_OPTIONS_INTL). v34.81 shipped a pointer here, which shifted
 * every following field and made frontends read the pointer bytes as a
 * values[] array (m17: garbage labels + SIGSEGV in OptionList_init).
 * sizeof(def) is locked by _Static_assert in core_options.c. */
#ifndef RETRO_NUM_CORE_OPTION_VALUES_MAX
#define RETRO_NUM_CORE_OPTION_VALUES_MAX 128
#endif

struct retro_core_option_value {
    const char* value;
    const char* label;
};

struct retro_core_option_definition {
    const char* key;
    const char* desc;
    const char* info;
    struct retro_core_option_value values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
    const char* default_value;
};

struct retro_core_options_intl {
    struct retro_core_option_definition* us;
    struct retro_core_option_definition* local;
};

struct retro_core_option_v2_category {
    const char* key;
    const char* desc;
    const char* info;
};

struct retro_core_options_v2 {
    struct retro_core_option_v2_category* categories;
    struct retro_core_option_definition* definitions;
    struct retro_core_options_intl* intl;
};

/* Controller description */
typedef struct retro_controller_description {
    const char* name;
    unsigned id;
} retro_controller_description;

/* Controller info */
typedef struct retro_controller_info {
    const struct retro_controller_description* types;
    unsigned num_types;
} retro_controller_info;

/* Input descriptor */
typedef struct retro_input_descriptor {
    unsigned port;
    unsigned device;
    unsigned index;
    unsigned id;
    const char* description;
} retro_input_descriptor;

/* Log levels */
typedef enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
    RETRO_LOG_DUMMY = INT_MAX
} retro_log_level;

/* Log callback */
typedef void (*retro_log_printf_t)(retro_log_level level, const char* fmt, ...);

typedef struct retro_log_callback {
    retro_log_printf_t log;
} retro_log_callback;

/* Environment callback type */
typedef bool (*retro_environment_t)(unsigned cmd, void* data);

/* Video refresh callback */
typedef void (*retro_video_refresh_t)(const void* data, unsigned width,
                                       unsigned height, size_t pitch);

/* Audio sample batch callback */
typedef size_t (*retro_audio_sample_batch_t)(const int16_t* data, size_t frames);

/* Audio sample callback */
typedef void (*retro_audio_sample_t)(int16_t left, int16_t right);

/* Input poll callback */
typedef void (*retro_input_poll_t)(void);

/* Input state callback */
typedef int16_t (*retro_input_state_t)(unsigned port, unsigned device,
                                        unsigned index, unsigned id);

/* Core API functions */
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);

void retro_init(void);
void retro_deinit(void);

unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info* info);
void retro_get_system_av_info(struct retro_system_av_info* info);

void retro_set_controller_port_device(unsigned port, unsigned device);

void retro_reset(void);
void retro_run(void);

bool retro_load_game(const struct retro_game_info* game);
bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num);
void retro_unload_game(void);

size_t retro_serialize_size(void);
bool retro_serialize(void* data, size_t size);
bool retro_unserialize(const void* data, size_t size);

void retro_cheat_reset(void);
void retro_cheat_set(unsigned index, bool enabled, const char* code);

void* retro_get_memory_data(unsigned id);
size_t retro_get_memory_size(unsigned id);

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_H */
