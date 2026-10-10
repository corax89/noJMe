/*
 * NOJME - MIDI subsystem (Sonivox backend, v36.89)
 *
 * This file replaces the old FM-synthesis engine with the open-source
 * Sonivox/EAS synthesizer from AOSP. Quality is dramatically better
 * (sample-based GM bank + DLS support + Schroeder reverb) while the
 * public API in midi.h stays byte-for-byte compatible with media.c.
 *
 * Architecture:
 *   midi_load()        -> EAS_Init + EAS_OpenFile(memory locator) +
 *                         EAS_Prepare + EAS_ParseMetaData
 *   midi_load_empty()  -> EAS_Init + EAS_OpenMIDIStream (for tone/MIDIControl)
 *   midi_play()        -> EAS_Resume
 *   midi_stop()        -> EAS_Pause  + sets playing=false, end_reached=true
 *   midi_generate_samples() -> EAS_Render (loop, drains src_buf first)
 *   midi_advance_time()    -> no-op (Sonivox advances inside EAS_Render)
 *   midi_note_on/off/CC/PC (_file) -> EAS_WriteMIDIStream (raw MIDI bytes)
 *   midi_set_position_ms() -> EAS_Locate
 *   midi_set_volume()      -> EAS_SetVolume (0..100)
 *   midi_get_duration_ms() -> cached from EAS_ParseMetaData
 *   midi_get_position_ms() -> maintained manually (sample clock)
 *
 * The internal sequencer struct (MidiSequencer) keeps the reverb/chorus
 * buffers from the old FM design so the layout matches media.c — those
 * buffers are unused now (Sonivox has its own reverb).
 *
 * Apache 2.0 for the Sonivox-derived parts; same NOJME license otherwise.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "midi.h"
#include "debug_macros.h" /* MEDIA_DEBUG macro */

/* v36.90 [MIDI-PERF]: internal AOSP API for runtime synth parameters.
 * PARSER_DATA_POLYPHONY lives in the E_PARSER_DATA enum (lib_src/eas_parser.h)
 * — the header is on the include path, pulling it in is safer than
 * hardcoding the enum position. EAS_IntSetStrmParam itself is defined in
 * lib_src/eas_public.c but was never declared in a public header. */
#include "eas_parser.h"
EAS_RESULT EAS_IntSetStrmParam (EAS_DATA_HANDLE pEASData, EAS_HANDLE pStream,
                                EAS_INT param, EAS_I32 value);

/* v36.90 [MIDI-PERF]: runtime synth performance gates (defined in
 * eas_host_nojme.c; read by eas_wtengine.c / eas_mixer.c).
 * 0 = fast defaults (no per-voice filter, dry mix). */
extern int g_nojme_voice_filter;
extern int g_nojme_reverb;

/* v36.89 [SONIVOX-DIAG]: trace of what Sonivox is doing.
 *
 * v36.90 [MIDI-PERF] GATE: the trace used to be UNCONDITIONAL. That is
 * acceptable on Switch (sw_trace is gated by the logging toggle there),
 * but on desktop/host builds it fprintf+fflush'd from the audio thread
 * (from under g_audio_mutex, inside the mix loop) and on Switch with
 * logging=on it did open(O_APPEND)+write+close SD I/O twice a second
 * while holding the audio mutex. Now it is gated by NOJME_SONIVOX_TRACE=1
 * (env) or the existing logging gate, resolved once at first use. The
 * per-100-calls peak scan runs only when the trace is enabled.
 */
static int sonivox_trace_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char* e = getenv("NOJME_SONIVOX_TRACE");
        s_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return s_enabled;
}

#if defined(__SWITCH__) || defined(NOJME_SWITCH_TRACE)
#include "switch/switch_trace.h"
#define SONIVOX_TRACE(fmt, ...)  do { \
    if (sonivox_trace_enabled()) sw_trace("[SONIVOX] " fmt, ##__VA_ARGS__); \
} while (0)
#else
#include <stdio.h>
#define SONIVOX_TRACE(fmt, ...)  do { \
    if (sonivox_trace_enabled()) { \
        fprintf(stderr, "[SONIVOX] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } \
} while (0)
#endif

/* Sonivox public API */
#include "eas.h"
#include "eas_types.h"

/* ------------------------------------------------------------------
 * Memory-backed EAS_FILE_LOCATOR: wraps the SMF bytes already in
 * memory (MidiFile->data / size) into a callback struct Sonivox can
 * pull from. Each MidiFile owns one of these.
 * ------------------------------------------------------------------ */
typedef struct {
    void  *handle;            /* pointer back to MidiFile->data */
    int  (*readAt)(void *handle, void *buf, int offset, int size);
    int  (*size)(void *handle);
} nojme_eas_locator;

/* readAt callback: copy from the MIDI byte buffer at offset */
static int nojme_loc_readAt(void *handle, void *buf, int offset, int size) {
    const uint8_t *data = (const uint8_t *) handle;
    /* size/offset validated by caller (Sonivox passes file_pos in [0, length]) */
    if (!data || !buf || offset < 0 || size < 0) return 0;
    /* Note: we don't have direct access to file length here, but the
     * Sonivox parser bounds-checks via EAS_HWFileLength. We trust it. */
    memcpy(buf, data + offset, (size_t) size);
    return size;
}

/* size callback: return total MIDI byte count */
static int nojme_loc_size(void *handle) {
    if (!handle) return 0;
    /* The handle is a pointer to a struct holding {data,size}. We
     * actually need both pieces. Trick: store a small wrapper struct
     * that contains data + size, and pass THAT as the handle. */
    return 0; /* will be overridden by wrapper (see below) */
}

/* Real handle structure (kept here so callbacks can see it) */
typedef struct {
    const uint8_t *data;
    int             size;
} nojme_loc_handle;

static int nojme_loc_readAt_real(void *handle, void *buf, int offset, int size) {
    nojme_loc_handle *h = (nojme_loc_handle *) handle;
    if (!h || !h->data || offset < 0 || size < 0) return 0;
    if (offset >= h->size) return 0;
    if (offset + size > h->size) size = h->size - offset;
    memcpy(buf, h->data + offset, (size_t) size);
    return size;
}

static int nojme_loc_size_real(void *handle) {
    nojme_loc_handle *h = (nojme_loc_handle *) handle;
    return h ? h->size : 0;
}

/* ------------------------------------------------------------------
 * Globals (one global EAS instance shared across all MidiFiles? No —
 * Sonivox docs say each pEASData is independent; for J2ME's
 * "multiple players at once" feature, each MidiFile gets its own
 * EAS_DATA_HANDLE so two players can be rendered independently.)
 * ------------------------------------------------------------------ */
static uint32_t g_sample_rate = 44100;
static MidiSequencer* g_active_sequencer = NULL;

/* For midi_note_on() without a MidiFile (called from tone player when
 * there's no .midi target): we route to a global "default" EAS instance
 * with an open MIDI stream. Created on first midi_init(). */
static EAS_DATA_HANDLE g_default_eas = NULL;
static EAS_HANDLE      g_default_stream = NULL;

/* ------------------------------------------------------------------
 * v36.90 [MIDI-PERF]: runtime configuration + live-instance registry.
 *
 * Polyphony is applied per EAS instance via EAS_IntSetStrmParam(
 * PARSER_DATA_POLYPHONY) — the AOSP voice manager (VMSetPolyphony)
 * then caps the pool allocation, so the synth never runs more voices
 * than requested. Filter/reverb are plain global gates read by the
 * engine hot paths (see eas_host_nojme.c for the rationale).
 *
 * The registry lets midi_set_polyphony() retune ALREADY-LOADED files
 * (games keep players alive for the whole session). All registry
 * access is under its own tiny mutex: midi_load() is called from Java
 * threads without g_audio_mutex in createPlayer, so the caller's locks
 * cannot be relied on. Lock order is always g_audio_mutex (caller,
 * optional) -> g_midi_cfg_mutex; the reverse is never taken.
 * ------------------------------------------------------------------ */
#define MIDI_MAX_INSTANCES 16

#ifndef _WIN32
#include <pthread.h>
#else
#include "win_thread_shim.h"
#endif

static pthread_mutex_t g_midi_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;
static MidiFile*       g_midi_instances[MIDI_MAX_INSTANCES];
static int             g_midi_polyphony   = 24;  /* fast default (GoF needs <=16) */

/* v36.91 [MIDI-GAIN]: пост-рендерный цифровой гейн MIDI-выхода.
 *
 * ПОЧЕМУ: Sonivox по умолчанию рендерит заметно тише, чем PCM/WAV-путь.
 * Замер (scripts/measure_midi_level.c, GoF_InGame.MID, 20 с, x86):
 *   EAS vol 0.90 (дефолт v36.90): peak -8.8 dBFS, RMS -25.2 dBFS
 *   EAS vol 1.00               : ВНУТРЕННИЙ САТУРАТОР синтезатора
 *                                (peak в clamp на плотных аккордах GoF)
 *   EAS 0.90 + пост-гейн x2.0  : peak -2.8 dBFS, RMS -19.2 dBFS — +6 дБ
 *                                громкости БЕЗ клиппинга
 * Типичный игровой WAV у полной шкалы: RMS -12..-18 dBFS — поэтому MIDI
 * слышно «намного тише PCM». Гейн применяется в midi_generate_samples
 * (одна точка для всех потребителей: SMF-файлы, device://tone и
 * device://midi сырые потоки). Дефолт 2.0 (+6 дБ); настройки:
 * settings midi_gain (проценты, 100..400) и env NOJME_MIDI_GAIN. */
static float g_midi_gain = 2.0f;

static void midi_apply_polyphony(MidiFile* midi) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    /* 0 = "reset to build max" per VMSetPolyphony; both SMF (via the
     * synth-handle fallback in EAS_IntSetStrmParam) and raw MIDI
     * streams (NM_SetData) understand it. */
    EAS_IntSetStrmParam((EAS_DATA_HANDLE) midi->eas_data,
                        (EAS_HANDLE) midi->eas_stream,
                        PARSER_DATA_POLYPHONY,
                        (EAS_I32) g_midi_polyphony);
}

static void midi_registry_add(MidiFile* midi) {
    if (!midi) return;
    pthread_mutex_lock(&g_midi_cfg_mutex);
    for (int i = 0; i < MIDI_MAX_INSTANCES; i++) {
        if (!g_midi_instances[i]) {
            g_midi_instances[i] = midi;
            break;
        }
    }
    /* full registry: player simply runs at the build-default polyphony
     * until a midi_set_polyphony()/reload; not fatal */
    pthread_mutex_unlock(&g_midi_cfg_mutex);

    midi_apply_polyphony(midi);
}

static void midi_registry_remove(MidiFile* midi) {
    if (!midi) return;
    pthread_mutex_lock(&g_midi_cfg_mutex);
    for (int i = 0; i < MIDI_MAX_INSTANCES; i++) {
        if (g_midi_instances[i] == midi) {
            g_midi_instances[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_midi_cfg_mutex);
}

int midi_set_polyphony(int voices) {
    if (voices < 0)   voices = 0;      /* 0 = reset to build max */
    if (voices > 32)  voices = 32;     /* hard cap = MAX_SYNTH_VOICES (Makefile) */

    pthread_mutex_lock(&g_midi_cfg_mutex);
    g_midi_polyphony = voices;
    for (int i = 0; i < MIDI_MAX_INSTANCES; i++) {
        if (g_midi_instances[i])
            midi_apply_polyphony(g_midi_instances[i]);
    }
    pthread_mutex_unlock(&g_midi_cfg_mutex);

    /* also retune the global default stream (tone/MIDIControl players) */
    if (g_default_eas && g_default_stream) {
        EAS_IntSetStrmParam(g_default_eas, g_default_stream,
                            PARSER_DATA_POLYPHONY, (EAS_I32) voices);
    }
    MEDIA_DEBUG("midi_set_polyphony(%d)", voices);
    return g_midi_polyphony;
}

int midi_get_polyphony(void) {
    return g_midi_polyphony;
}

/* v36.91 [MIDI-GAIN]: 1.0 = не трогать выход Sonivox; 2.0 = +6 дБ (дефолт).
 * Диапазон 0.25..4.0: выше — повальный клиппинг плотных аккордов
 * (сатурация в generate-пути есть, но музыкально это уже мусор). */
void midi_set_gain(float gain) {
    if (gain < 0.25f) gain = 0.25f;
    if (gain > 4.0f)  gain = 4.0f;
    g_midi_gain = gain;
    MEDIA_DEBUG("midi_set_gain(%.2f)", gain);
}

float midi_get_gain(void) {
    return g_midi_gain;
}

void midi_set_filter_enabled(int on) {
    g_nojme_voice_filter = on ? 1 : 0;
    MEDIA_DEBUG("midi_set_filter_enabled(%d)", g_nojme_voice_filter);
}

int midi_get_filter_enabled(void) {
    return g_nojme_voice_filter;
}

void midi_set_reverb_enabled(int on) {
    g_nojme_reverb = on ? 1 : 0;
    MEDIA_DEBUG("midi_set_reverb_enabled(%d)", g_nojme_reverb);
}

int midi_get_reverb_enabled(void) {
    return g_nojme_reverb;
}

/* ------------------------------------------------------------------
 * Note frequency table — kept for ABI compatibility with any caller
 * that touches g_note_frequencies (none in current NOJME, but the
 * symbol is exported). Sonivox computes its own frequencies.
 * ------------------------------------------------------------------ */
#define MIDI_NOTE_COUNT 128
static float g_note_frequencies[MIDI_NOTE_COUNT];

static void init_note_frequencies(void) {
    for (int i = 0; i < MIDI_NOTE_COUNT; i++) {
        g_note_frequencies[i] = 440.0f * powf(2.0f, (i - 69) / 12.0f);
    }
}

/* ------------------------------------------------------------------
 * midi_init: called once from media.c at startup. Initializes a
 * default EAS instance used by midi_note_on() / midi_control_change()
 * when no MidiFile target is set (e.g. tone-only players).
 * ------------------------------------------------------------------ */
int midi_init(uint32_t sample_rate) {
    g_sample_rate = sample_rate;
    init_note_frequencies();

    /* v36.90 [MIDI-PERF]: one-time runtime config from the environment.
 * settings.ini goes through midi_set_* wrappers (switch frontend main.c
 * / libretro core options); the env vars are the headless/desktop path.
 * NOJME_MIDI_POLYPHONY=4..64, NOJME_MIDI_FILTER=0/1,
 * NOJME_MIDI_REVERB=0/1. */
    {
        static int s_cfg_done = 0;
        if (!s_cfg_done) {
            s_cfg_done = 1;
            const char* e;
            if ((e = getenv("NOJME_MIDI_POLYPHONY")) != NULL && e[0]) {
                int v = atoi(e);
                if (v >= 0 && v <= 32) midi_set_polyphony(v);
            }
            if ((e = getenv("NOJME_MIDI_FILTER")) != NULL && e[0])
                midi_set_filter_enabled(strcmp(e, "0") != 0);
            if ((e = getenv("NOJME_MIDI_REVERB")) != NULL && e[0])
                midi_set_reverb_enabled(strcmp(e, "0") != 0);
            /* v36.91 [MIDI-GAIN]: пост-гейн выход Sonivox, проценты
             * 25..400 (100 = как в v36.90, 200 = дефолт = +6 дБ). */
            if ((e = getenv("NOJME_MIDI_GAIN")) != NULL && e[0])
                midi_set_gain(atof(e) / 100.0f);
        }
    }

    SONIVOX_TRACE("midi_init sr=%u", (unsigned) sample_rate);

    /* Boot the default EAS instance for global MIDI channel. */
    if (!g_default_eas) {
        EAS_RESULT r = EAS_Init(&g_default_eas);
        SONIVOX_TRACE("midi_init EAS_Init result=%ld eas=%p",
                      (long) r, (void*) g_default_eas);
        if (r == EAS_SUCCESS && g_default_eas) {
            EAS_RESULT r2 = EAS_OpenMIDIStream(g_default_eas, &g_default_stream, NULL);
            SONIVOX_TRACE("midi_init EAS_OpenMIDIStream result=%ld stream=%p",
                          (long) r2, (void*) g_default_stream);
            if (r2 != EAS_SUCCESS) {
                g_default_stream = NULL;
            } else {
                /* v36.90 [MIDI-PERF]: settings/env may have been applied
                 * BEFORE this init (switch main.c pushes them at session
                 * start) — the fresh default stream must inherit the
                 * configured polyphony, not the build max. */
                EAS_IntSetStrmParam(g_default_eas, g_default_stream,
                                    PARSER_DATA_POLYPHONY,
                                    (EAS_I32) g_midi_polyphony);
            }
            /* Default volume 90 (matches Sonivox's own default) */
            EAS_SetVolume(g_default_eas, NULL, 90);
        }
    }
    return 0;
}

void midi_shutdown(void) {
    if (g_default_stream) {
        EAS_CloseMIDIStream(g_default_eas, g_default_stream);
        g_default_stream = NULL;
    }
    if (g_default_eas) {
        EAS_Shutdown(g_default_eas);
        g_default_eas = NULL;
    }
    g_active_sequencer = NULL;
}

/* ------------------------------------------------------------------
 * Helper: allocate & init a fresh MidiSequencer (sets default channel
 * state, sample_rate, volume=1.0). The reverb/chorus buffers are
 * zeroed but never used (Sonivox has its own effects).
 * ------------------------------------------------------------------ */
static MidiSequencer* alloc_sequencer(void) {
    MidiSequencer* seq = (MidiSequencer*) calloc(1, sizeof(MidiSequencer));
    if (!seq) return NULL;

    seq->max_active_notes = 64;            /* Sonivox's MAX_SYNTH_VOICES */
    seq->active_note_count = 0;
    seq->active_notes = (MidiNote*) calloc((size_t) seq->max_active_notes,
                                           sizeof(MidiNote));
    seq->ticks_per_beat = 480;
    seq->microseconds_per_beat = 500000;
    seq->current_tempo = 120.0f;
    seq->total_time_us = 0;
    seq->smpte = false;
    seq->us_per_tick = 500.0;
    seq->end_reached = false;
    seq->position = 0;
    seq->playing = false;
    seq->loop = false;
    seq->volume = 1.0f;
    seq->sample_rate = g_sample_rate;
    seq->samples_per_tick = g_sample_rate * 500.0 / 1000000.0;
    seq->sample_accumulator = 0.0;
    seq->last_real_time_us = 0;
    seq->tick_accumulator = 0.0;

    /* diag counters reset to 0 by calloc */
    for (int c = 0; c < MIDI_CHANNELS; c++) {
        seq->channels[c].program = 0;
        seq->channels[c].volume = 100;       /* GM default */
        seq->channels[c].pan = 64;           /* center */
        seq->channels[c].expression = 127;
        seq->channels[c].sustain = 0;
        seq->channels[c].pitch_bend = 1.0f;
        seq->channels[c].bank = 0;
        seq->channels[c].modulation = 0;
        seq->channels[c].reverb = 0;
        seq->channels[c].chorus = 0;
    }
    return seq;
}

/* ------------------------------------------------------------------
 * midi_load: parse SMF bytes via Sonivox's SMF parser.
 * We do NOT keep our own copy of the data; Sonivox reads through the
 * memory-backed locator. But media.c stores midi->data = NULL after
 * free, so the caller's contract is "data must remain valid until
 * midi_free".
 * ------------------------------------------------------------------ */
MidiFile* midi_load(const uint8_t* data, uint32_t size) {
    if (!data || size < 14) return NULL;

    SONIVOX_TRACE("midi_load enter size=%u first4=%02x%02x%02x%02x",
                  (unsigned) size,
                  data[0], data[1], data[2], data[3]);

    MidiFile* midi = (MidiFile*) calloc(1, sizeof(MidiFile));
    if (!midi) return NULL;
    midi->data = (uint8_t*) data;        /* borrowed, not owned */
    midi->size = size;

    midi->sequencer = alloc_sequencer();
    if (!midi->sequencer) { free(midi); return NULL; }

    /* Init Sonivox EAS instance */
    EAS_RESULT r_init = EAS_Init((EAS_DATA_HANDLE*) &midi->eas_data);
    if (r_init != EAS_SUCCESS || !midi->eas_data) {
        SONIVOX_TRACE("midi_load EAS_Init FAILED result=%ld", (long) r_init);
        free(midi->sequencer->active_notes);
        free(midi->sequencer);
        free(midi);
        return NULL;
    }

    /* Construct memory-backed EAS_FILE locator */
    nojme_loc_handle *lh = (nojme_loc_handle*) calloc(1, sizeof(nojme_loc_handle));
    nojme_eas_locator *loc = (nojme_eas_locator*) calloc(1, sizeof(nojme_eas_locator));
    if (!lh || !loc) {
        if (lh)  free(lh);
        if (loc) free(loc);
        EAS_Shutdown(midi->eas_data);
        free(midi->sequencer->active_notes);
        free(midi->sequencer);
        free(midi);
        return NULL;
    }
    lh->data = data;
    lh->size = (int) size;
    loc->handle = lh;
    loc->readAt = nojme_loc_readAt_real;
    loc->size   = nojme_loc_size_real;
    midi->eas_locator = loc;

    /* Open the SMF file via the locator */
    EAS_HANDLE stream = NULL;
    EAS_RESULT r = EAS_OpenFile((EAS_DATA_HANDLE) midi->eas_data,
                               (EAS_FILE_LOCATOR) loc, &stream);
    SONIVOX_TRACE("midi_load EAS_OpenFile result=%ld stream=%p",
                  (long) r, (void*) stream);
    if (r != EAS_SUCCESS || !stream) {
        SONIVOX_TRACE("midi_load FAILED at EAS_OpenFile (size=%u)", (unsigned) size);
        free(lh); free(loc);
        EAS_Shutdown(midi->eas_data);
        free(midi->sequencer->active_notes);
        free(midi->sequencer);
        free(midi);
        return NULL;
    }
    midi->eas_stream = stream;

    /* Prepare for playback + parse metadata (duration) */
    EAS_RESULT r_prep = EAS_Prepare((EAS_DATA_HANDLE) midi->eas_data, stream);
    SONIVOX_TRACE("midi_load EAS_Prepare result=%ld", (long) r_prep);

    EAS_I32 playLength = 0;
    EAS_RESULT r_meta = EAS_ParseMetaData((EAS_DATA_HANDLE) midi->eas_data, stream, &playLength);
    SONIVOX_TRACE("midi_load EAS_ParseMetaData result=%ld dur_ms=%ld",
                  (long) r_meta, (long) playLength);
    if (r_meta == EAS_SUCCESS && playLength > 0) {
        midi->sequencer->total_time_us = (uint64_t) playLength * 1000ULL;
    } else {
        /* Fallback: estimate by file size (~120 BPM, 480 TPB, 4 bytes/event) */
        midi->sequencer->total_time_us = (uint64_t) size * 4000ULL;
    }

    /* Default master volume */
    EAS_SetVolume((EAS_DATA_HANDLE) midi->eas_data, stream, 90);

    midi->is_midi_stream = false;
    midi->src_avail = 0; midi->src_pos = 0.0;

    midi_registry_add(midi);   /* v36.90 [MIDI-PERF]: apply runtime polyphony */

    SONIVOX_TRACE("midi_load OK size=%u dur_ms=%u",
                  (unsigned) size,
                  (unsigned)(midi->sequencer->total_time_us / 1000));
    return midi;
}

/* ------------------------------------------------------------------
 * midi_load_empty: used by device://tone and device://midi players.
 * Opens a raw MIDI stream (no SMF parser). midi_note_on_file() etc.
 * feed it bytes via EAS_WriteMIDIStream.
 * ------------------------------------------------------------------ */
MidiFile* midi_load_empty(void) {
    SONIVOX_TRACE("midi_load_empty enter");
    MidiFile* midi = (MidiFile*) calloc(1, sizeof(MidiFile));
    if (!midi) return NULL;

    midi->sequencer = alloc_sequencer();
    if (!midi->sequencer) { free(midi); return NULL; }

    EAS_RESULT r_init = EAS_Init((EAS_DATA_HANDLE*) &midi->eas_data);
    if (r_init != EAS_SUCCESS || !midi->eas_data) {
        SONIVOX_TRACE("midi_load_empty EAS_Init FAILED result=%ld", (long) r_init);
        free(midi->sequencer->active_notes);
        free(midi->sequencer);
        free(midi);
        return NULL;
    }

    EAS_HANDLE stream = NULL;
    EAS_RESULT r = EAS_OpenMIDIStream((EAS_DATA_HANDLE) midi->eas_data, &stream, NULL);
    SONIVOX_TRACE("midi_load_empty EAS_OpenMIDIStream result=%ld stream=%p",
                  (long) r, (void*) stream);
    if (r != EAS_SUCCESS || !stream) {
        EAS_Shutdown(midi->eas_data);
        free(midi->sequencer->active_notes);
        free(midi->sequencer);
        free(midi);
        return NULL;
    }
    midi->eas_stream = stream;
    midi->is_midi_stream = true;
    midi->src_avail = 0; midi->src_pos = 0.0;

    EAS_SetVolume((EAS_DATA_HANDLE) midi->eas_data, stream, 90);
    midi_registry_add(midi);   /* v36.90 [MIDI-PERF]: apply runtime polyphony */
    SONIVOX_TRACE("midi_load_empty OK");
    return midi;
}

/* ------------------------------------------------------------------
 * midi_free: shutdown EAS instance, free all per-file state.
 * ------------------------------------------------------------------ */
void midi_free(MidiFile* midi) {
    if (!midi) return;

    SONIVOX_TRACE("midi_free eas=%p stream=%p is_midi_stream=%d",
                  (void*) midi->eas_data, (void*) midi->eas_stream,
                  midi->is_midi_stream);
    midi_registry_remove(midi);   /* v36.90 [MIDI-PERF] */

    if (midi->eas_stream) {
        if (midi->is_midi_stream) {
            EAS_CloseMIDIStream((EAS_DATA_HANDLE) midi->eas_data,
                                 (EAS_HANDLE) midi->eas_stream);
        } else {
            EAS_CloseFile((EAS_DATA_HANDLE) midi->eas_data,
                          (EAS_HANDLE) midi->eas_stream);
        }
        midi->eas_stream = NULL;
    }
    if (midi->eas_data) {
        EAS_Shutdown((EAS_DATA_HANDLE) midi->eas_data);
        midi->eas_data = NULL;
    }
    if (midi->eas_locator) {
        nojme_eas_locator *loc = (nojme_eas_locator*) midi->eas_locator;
        if (loc->handle) free(loc->handle);
        free(loc);
        midi->eas_locator = NULL;
    }
    if (midi->sequencer) {
        if (midi->sequencer->active_notes) {
            free(midi->sequencer->active_notes);
            midi->sequencer->active_notes = NULL;
        }
        if (midi->sequencer->tracks) {
            free(midi->sequencer->tracks);
            midi->sequencer->tracks = NULL;
        }
        free(midi->sequencer);
        midi->sequencer = NULL;
    }
    free(midi);
}

/* ------------------------------------------------------------------
 * Playback control
 * ------------------------------------------------------------------ */
void midi_play(MidiFile* midi) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    SONIVOX_TRACE("midi_play eas=%p stream=%p is_midi_stream=%d end_reached=%d",
                  (void*) midi->eas_data, (void*) midi->eas_stream,
                  midi->is_midi_stream, midi->sequencer->end_reached);
    /* If we already reached the end, restart from the beginning */
    if (midi->sequencer->end_reached && !midi->is_midi_stream) {
        EAS_Locate((EAS_DATA_HANDLE) midi->eas_data,
                   (EAS_HANDLE) midi->eas_stream, 0, EAS_FALSE);
        midi->sequencer->end_reached = false;
        midi->sequencer->position = 0;
        midi->src_avail = 0; midi->src_pos = 0.0;
    }
    midi->sequencer->playing = true;
    EAS_RESULT r = EAS_Resume((EAS_DATA_HANDLE) midi->eas_data,
                              (EAS_HANDLE) midi->eas_stream);
    SONIVOX_TRACE("midi_play EAS_Resume result=%ld playing=%d",
                  (long) r, midi->sequencer->playing ? 1 : 0);
    g_active_sequencer = midi->sequencer;
}

void midi_stop(MidiFile* midi) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    SONIVOX_TRACE("midi_stop eas=%p stream=%p",
                  (void*) midi->eas_data, (void*) midi->eas_stream);
    midi->sequencer->playing = false;
    midi->sequencer->end_reached = true;
    EAS_Pause((EAS_DATA_HANDLE) midi->eas_data,
              (EAS_HANDLE) midi->eas_stream);
    /* Reset leftover buffer so we don't replay stale samples */
    midi->src_avail = 0; midi->src_pos = 0.0;
}

void midi_pause(MidiFile* midi) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    midi->sequencer->playing = false;
    EAS_Pause((EAS_DATA_HANDLE) midi->eas_data,
              (EAS_HANDLE) midi->eas_stream);
}

void midi_resume(MidiFile* midi) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    midi->sequencer->playing = true;
    EAS_Resume((EAS_DATA_HANDLE) midi->eas_data,
               (EAS_HANDLE) midi->eas_stream);
}

void midi_set_loop(MidiFile* midi, bool loop) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->loop = loop;
}

void midi_set_volume(MidiFile* midi, float volume) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    midi->sequencer->volume = volume;
    /* EAS_SetVolume expects 0..100 (1 dB increments) */
    EAS_SetVolume((EAS_DATA_HANDLE) midi->eas_data,
                  (EAS_HANDLE) midi->eas_stream,
                  (EAS_I32)(volume * 100.0f));
}

/* ------------------------------------------------------------------
 * midi_generate_samples: render samples into user's stereo int16
 * buffer. Sonivox emits BUFFER_SIZE_IN_MONO_SAMPLES frames per call
 * (where each frame = NUM_OUTPUT_CHANNELS samples = 2 for stereo).
 * We loop until we've filled the request, draining src_buf first.
 * ------------------------------------------------------------------ */

/* Sonivox eas_audioconst.h (when -D _SAMPLE_RATE_22050) sets
 * BUFFER_SIZE_IN_MONO_SAMPLES = 128. Each EAS_Render call emits
 * exactly 128 mono frames = 256 stereo shorts. We hardcode this
 * because eas_audioconst.h pulls in too many other internals. */
#define NOJME_EAS_BUFFER_FRAMES 128

/* Sonivox internal synth rate (matches wt_22khz.c bank). NOJME's
 * audio system runs at g_sample_rate (default 44100). We render at
 * the bank's native rate (22050) and linearly upsample 2:1 to
 * 44100 in midi_generate_samples — same approach as the WAV
 * resampler in media.c. */
#define SONIVOX_SRC_RATE 22050

void midi_generate_samples(MidiFile* midi, int16_t* buffer, int samples) {
    if (!midi || !midi->sequencer || !buffer) return;

    /* Always zero the output buffer so silence is true silence even
     * if EAS_Render is not called (e.g. paused or empty stream). */
    memset(buffer, 0, (size_t) samples * 2 * sizeof(int16_t));

    if (!midi->eas_data || !midi->eas_stream) {
        SONIVOX_TRACE("midi_generate_samples: NO eas_data/stream (eas=%p stream=%p)",
                      (void*) (midi ? midi->eas_data : NULL),
                      (void*) (midi ? midi->eas_stream : NULL));
        return;
    }

    /* v36.89 [SONIVOX-DIAG]: throttle to 1 log line per ~100 calls.
     * v36.90: the whole block (incl. the peak scan) runs only when the
     * trace is enabled — it used to scan the buffer unconditionally. */
    static uint32_t s_gen_seq = 0;
    static uint32_t s_gen_last = 0;
    int log_this = 0;
    if (sonivox_trace_enabled()) {
        s_gen_seq++;
        log_this = (s_gen_seq - s_gen_last >= 100);
        if (log_this) s_gen_last = s_gen_seq;
    }

    /* src_ratio = 22050 / 44100 = 0.5 (one dst sample advances src_pos by 0.5) */
    const double src_ratio = (double) SONIVOX_SRC_RATE / (double) g_sample_rate;
    int samples_filled = 0;

    /* Main render+upsample loop.
     * src_buf layout (129 stereo frames = 258 shorts):
     *   frame 0      = carryover sample from previous EAS_Render batch
     *                  (or 0 on first call, for clean interp start)
     *   frames 1..128 = current EAS_Render output
     * src_pos is the fractional read position (in src_buf frame units).
     * It advances by src_ratio (0.5) per dst sample filled. When
     * src_pos >= 128 (we've used all 128 new frames + carryover), we
     * carry the last frame to position 0 and render a new batch.
     */
    while (samples_filled < samples) {
        /* Refill src_buf if we don't have enough to interpolate (need >= 2 frames) */
        if (midi->src_avail < 2) {
            /* If we have a carryover frame (src_avail == 1), keep it at
             * position 0; render new batch into positions 1..128.
             * If src_avail == 0 (first call), zero out position 0 (silent
             * carryover) and render into positions 1..128. */
            int offset;
            if (midi->src_avail == 1) {
                /* Already at position 0 from previous carry */
                offset = 1;
            } else {
                /* First call: position 0 = silence */
                midi->src_buf[0] = 0;
                midi->src_buf[1] = 0;
                midi->src_avail = 1;
                offset = 1;
            }

            EAS_I32 generated = 0;
            EAS_RESULT r = EAS_Render(
                (EAS_DATA_HANDLE) midi->eas_data,
                (EAS_PCM*) (midi->src_buf + offset * 2),
                (EAS_I32) NOJME_EAS_BUFFER_FRAMES,
                &generated);

            if (log_this) {
                SONIVOX_TRACE("midi_generate_samples: EAS_Render r=%ld gen=%ld playing=%d",
                              (long) r, (long) generated,
                              midi->sequencer->playing ? 1 : 0);
            }

            if (r != EAS_SUCCESS || generated <= 0) {
                /* Render failed or stream ended */
                EAS_STATE state = EAS_STATE_STOPPED;
                EAS_State((EAS_DATA_HANDLE) midi->eas_data,
                          (EAS_HANDLE) midi->eas_stream, &state);
                if (log_this) {
                    SONIVOX_TRACE("midi_generate_samples: render returned r=%ld gen=%ld state=%ld",
                                  (long) r, (long) generated, (long) state);
                }

                if (state == EAS_STATE_STOPPED || state == EAS_STATE_EMPTY) {
                    midi->sequencer->end_reached = true;
                    midi->sequencer->playing = false;

                    /* Loop support: rewind and resume for next render call */
                    if (midi->sequencer->loop && !midi->is_midi_stream) {
                        EAS_Locate((EAS_DATA_HANDLE) midi->eas_data,
                                   (EAS_HANDLE) midi->eas_stream, 0, EAS_FALSE);
                        EAS_Resume((EAS_DATA_HANDLE) midi->eas_data,
                                   (EAS_HANDLE) midi->eas_stream);
                        midi->sequencer->end_reached = false;
                        midi->sequencer->playing = true;
                        /* Discard carryover, restart src buffer */
                        midi->src_avail = 0;
                        midi->src_pos = 0.0;
                        continue;
                    }
                }
                /* Fill rest with silence (already zeroed) and bail */
                break;
            }

            /* src_avail now = offset (carryover) + generated (new frames) */
            midi->src_avail = offset + (int) generated;
            /* src_pos unchanged: any fractional remainder past the previous
             * src_avail-1 is preserved across the carry+refill */

            /* [MIDI-LOOP-FIX] v36.95: natural end-of-file detection for
             * SMF file players. EAS_Render keeps returning EAS_SUCCESS
             * with a full buffer after the sequence has ended (silence +
             * voice tails), so the old EOF check - reachable only when
             * EAS_Render returns an error or zero samples - NEVER fired
             * at a normal end of file. The sequencer stayed "playing"
             * forever, mix_midi_player's loop-restart branch was dead
             * code, and EVERY game with setLoopCount(-1..N) MIDI music
             * faded into permanent silence after the first pass (Brick
             * Breaker Revolution 3D and friends). END_OF_MEDIA for
             * non-looping players was equally unreachable.
             * Poll the parser state once per refill batch (~5.8 ms of
             * audio): STOPPING = all tracks hit EndOfTrack (voices may
             * still ring), STOPPED = tracks done and voices drained,
             * EMPTY = parser reports nothing left. On detection mark
             * end_reached and drop "playing" so the mixer-side loop
             * policy (mix_midi_player) restarts the track or finishes
             * the player exactly as designed. Raw MIDI streams
             * (device://tone, MIDIControl) never end - skipped. */
            if (midi->sequencer->playing && !midi->is_midi_stream) {
                EAS_STATE eof_state = EAS_STATE_READY;
                if (EAS_State((EAS_DATA_HANDLE) midi->eas_data,
                              (EAS_HANDLE) midi->eas_stream,
                              &eof_state) == EAS_SUCCESS &&
                    (eof_state == EAS_STATE_STOPPING ||
                     eof_state == EAS_STATE_STOPPED ||
                     eof_state == EAS_STATE_EMPTY)) {
                    midi->sequencer->end_reached = true;
                    midi->sequencer->playing = false;
                    SONIVOX_TRACE("midi EOF: parser state=%ld pos_ms=%u dur_ms=%u",
                                  (long) eof_state,
                                  (unsigned) midi_get_position_ms(midi),
                                  (unsigned) (midi->sequencer->total_time_us / 1000));
                }
            }
        }

        /* Linear interpolation from src_buf (22050 Hz) to dst_buf (44100 Hz) */
        while (midi->src_pos < (double)(midi->src_avail - 1) &&
               samples_filled < samples) {
            int i0 = (int) midi->src_pos;
            double frac = midi->src_pos - (double) i0;
            int16_t L0 = midi->src_buf[i0 * 2];
            int16_t R0 = midi->src_buf[i0 * 2 + 1];
            int16_t L1 = midi->src_buf[(i0 + 1) * 2];
            int16_t R1 = midi->src_buf[(i0 + 1) * 2 + 1];
            int32_t L = (int32_t)L0 + (int32_t)(((double)(L1 - L0)) * frac);
            int32_t R = (int32_t)R0 + (int32_t)(((double)(R1 - R0)) * frac);
            /* v36.91 [MIDI-GAIN]: компенсация тихого выхода Sonivox
             * относительно PCM/WAV (замер и мотивация — у g_midi_gain).
             * Сатурация здесь же: mix-путь дальше добавляет player volume
             * к остальным источникам, и его sat16 остаётся честным. */
            if (g_midi_gain != 1.0f) {
                L = (int32_t)((float) L * g_midi_gain);
                R = (int32_t)((float) R * g_midi_gain);
                if (L < -32768) L = -32768; else if (L > 32767) L = 32767;
                if (R < -32768) R = -32768; else if (R > 32767) R = 32767;
            }
            buffer[samples_filled * 2]     = (int16_t) L;
            buffer[samples_filled * 2 + 1] = (int16_t) R;
            samples_filled++;
            midi->src_pos += src_ratio;
        }

        /* If we've consumed all interp-able frames in this batch, carry
         * the last one to position 0 and shift src_pos so the fractional
         * remainder is preserved across the batch boundary. */
        if (midi->src_pos >= (double)(midi->src_avail - 1)) {
            int last = midi->src_avail - 1;
            midi->src_buf[0] = midi->src_buf[last * 2];
            midi->src_buf[1] = midi->src_buf[last * 2 + 1];
            midi->src_pos -= (double) last;
            midi->src_avail = 1;
        }
    }

    /* Advance the song position (audio clock, in 44100 Hz frames) */
    midi->sequencer->position += (uint32_t) samples_filled;
    midi->sequencer->active_note_count =
        midi->sequencer->playing ? midi->sequencer->active_note_count : 0;

    /* v36.89 [SONIVOX-DIAG]: report peak amplitude every ~100 calls */
    if (log_this) {
        int32_t peak = 0;
        for (int i = 0; i < samples * 2; i++) {
            int32_t a = buffer[i] < 0 ? -buffer[i] : buffer[i];
            if (a > peak) peak = a;
        }
        SONIVOX_TRACE("midi_generate_samples: filled=%d/%d peak=%d pos_ms=%u playing=%d end=%d",
                      samples_filled, samples, (int) peak,
                      (unsigned) midi_get_position_ms(midi),
                      midi->sequencer->playing ? 1 : 0,
                      midi->sequencer->end_reached ? 1 : 0);
    }
}

/* ------------------------------------------------------------------
 * midi_advance_time: with Sonivox, the audio clock IS the song clock
 * (EAS_Render both advances and renders). So this is a no-op —
 * mix_midi_player in media.c still calls it, but we don't need to do
 * anything. Kept for API compatibility.
 * ------------------------------------------------------------------ */
void midi_advance_time(MidiFile* midi, uint64_t elapsed_us) {
    (void) midi;
    (void) elapsed_us;
}

/* ------------------------------------------------------------------
 * Position queries
 * ------------------------------------------------------------------ */
float midi_get_position(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    return (float) ((double) midi->sequencer->position / (double) g_sample_rate);
}

float midi_get_length(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    return (float) ((double) midi->sequencer->total_time_us / 1000000.0);
}

uint32_t midi_get_duration_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    return (uint32_t) (midi->sequencer->total_time_us / 1000ULL);
}

uint32_t midi_get_position_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    uint64_t us = (uint64_t) midi->sequencer->position * 1000000ULL / g_sample_rate;
    return (uint32_t) (us / 1000ULL);
}

void midi_set_position_ms(MidiFile* midi, uint32_t position_ms) {
    if (!midi || !midi->eas_data || !midi->eas_stream || midi->is_midi_stream)
        return;
    /* Locate quantizes to ~5.9ms resolution — acceptable for J2ME */
    EAS_Locate((EAS_DATA_HANDLE) midi->eas_data,
               (EAS_HANDLE) midi->eas_stream,
               (EAS_I32) position_ms, EAS_FALSE);
    /* Reset sample clock to match (so subsequent position queries are
     * consistent until the next EAS_Render) */
    midi->sequencer->position =
        (uint32_t) ((uint64_t) position_ms * g_sample_rate / 1000ULL);
    midi->src_avail = 0; midi->src_pos = 0.0;
}

/* ------------------------------------------------------------------
 * Direct MIDI commands: feed raw MIDI bytes into the synth via
 * EAS_WriteMIDIStream. Both the global (g_active_sequencer) and the
 * per-file variants build a 2- or 3-byte MIDI message and ship it.
 * ------------------------------------------------------------------ */

/* Forward declaration for the global variant target (defined below) */
static void eas_send_message(EAS_DATA_HANDLE eas, EAS_HANDLE stream,
                             uint8_t status, uint8_t data1, uint8_t data2,
                             int data_len);

static void send_message_file(MidiFile* midi, uint8_t status,
                              uint8_t data1, uint8_t data2, int data_len) {
    if (!midi || !midi->eas_data || !midi->eas_stream) return;
    eas_send_message((EAS_DATA_HANDLE) midi->eas_data,
                     (EAS_HANDLE) midi->eas_stream,
                     status, data1, data2, data_len);
}

static void eas_send_message(EAS_DATA_HANDLE eas, EAS_HANDLE stream,
                             uint8_t status, uint8_t data1, uint8_t data2,
                             int data_len) {
    if (!eas || !stream) return;
    uint8_t msg[3];
    msg[0] = status;
    if (data_len >= 2) msg[1] = data1;
    if (data_len >= 3) msg[2] = data2;
    EAS_WriteMIDIStream(eas, stream, msg, (EAS_I32) data_len);
}

/* Per-file variants — route to a specific player's EAS stream.
 * For SMF-loaded MidiFiles (midi_load), this writes into the synth
 * owned by the SMF parser's stream — Sonivox accepts live MIDI on top
 * of an SMF stream. For midi_load_empty(), this is the primary path. */
void midi_note_on_file(MidiFile* midi, uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) {
        send_message_file(midi, (uint8_t)(0x80 | (channel & 0x0F)),
                          note, 0, 2);
    } else {
        send_message_file(midi, (uint8_t)(0x90 | (channel & 0x0F)),
                          note, velocity, 3);
    }
    if (midi && midi->sequencer) {
        midi->sequencer->channels[channel & 0x0F].program =
            midi->sequencer->channels[channel & 0x0F].program;
    }
}

void midi_note_off_file(MidiFile* midi, uint8_t channel, uint8_t note) {
    send_message_file(midi, (uint8_t)(0x80 | (channel & 0x0F)),
                      note, 0, 2);
}

void midi_control_change_file(MidiFile* midi, uint8_t channel,
                              uint8_t controller, uint8_t value) {
    send_message_file(midi, (uint8_t)(0xB0 | (channel & 0x0F)),
                      controller, value, 3);
    if (midi && midi->sequencer) {
        MidiChannel* ch = &midi->sequencer->channels[channel & 0x0F];
        switch (controller) {
            case MIDI_CTRL_VOLUME:      ch->volume = value; break;
            case MIDI_CTRL_PAN:         ch->pan = value; break;
            case MIDI_CTRL_EXPRESSION:  ch->expression = value; break;
            case MIDI_CTRL_SUSTAIN:     ch->sustain = value; break;
            case MIDI_CTRL_BANK_SELECT: ch->bank = value; break;
            case MIDI_CTRL_MODULATION:  ch->modulation = value; break;
            case MIDI_CTRL_REVERB:      ch->reverb = value; break;
            case MIDI_CTRL_CHORUS:      ch->chorus = value; break;
            default: break;
        }
    }
}

void midi_program_change_file(MidiFile* midi, uint8_t channel, uint8_t program) {
    send_message_file(midi, (uint8_t)(0xC0 | (channel & 0x0F)),
                      program, 0, 2);
    if (midi && midi->sequencer) {
        midi->sequencer->channels[channel & 0x0F].program = program;
    }
}

int midi_get_channel_volume_file(MidiFile* midi, int channel) {
    if (!midi || !midi->sequencer) return 0;
    if (channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return midi->sequencer->channels[channel].volume;
}

int midi_get_program_file(MidiFile* midi, int channel) {
    if (!midi || !midi->sequencer) return 0;
    if (channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return midi->sequencer->channels[channel].program;
}

/* Global variants — for tone players that have no specific MidiFile
 * target. Routes to the global default MIDI stream (g_default_stream). */
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) {
        eas_send_message(g_default_eas, g_default_stream,
                         (uint8_t)(0x80 | (channel & 0x0F)), note, 0, 2);
    } else {
        eas_send_message(g_default_eas, g_default_stream,
                         (uint8_t)(0x90 | (channel & 0x0F)), note, velocity, 3);
    }
}

void midi_note_off(uint8_t channel, uint8_t note) {
    eas_send_message(g_default_eas, g_default_stream,
                     (uint8_t)(0x80 | (channel & 0x0F)), note, 0, 2);
}

void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value) {
    eas_send_message(g_default_eas, g_default_stream,
                     (uint8_t)(0xB0 | (channel & 0x0F)), controller, value, 3);
}

void midi_program_change(uint8_t channel, uint8_t program) {
    eas_send_message(g_default_eas, g_default_stream,
                     (uint8_t)(0xC0 | (channel & 0x0F)), program, 0, 2);
}

void midi_pitch_bend(uint8_t channel, int16_t value) {
    /* MIDI pitch bend: status 0xEn, 14-bit value split into LSB/MSB */
    uint8_t lsb = (uint8_t)(value & 0x7F);
    uint8_t msb = (uint8_t)((value >> 7) & 0x7F);
    eas_send_message(g_default_eas, g_default_stream,
                     (uint8_t)(0xE0 | (channel & 0x0F)), lsb, msb, 3);
}

int midi_get_channel_volume(int channel) {
    if (channel < 0 || channel >= MIDI_CHANNELS) return 0;
    if (!g_active_sequencer) return 100;
    return g_active_sequencer->channels[channel].volume;
}

int midi_get_program(int channel) {
    if (channel < 0 || channel >= MIDI_CHANNELS) return 0;
    if (!g_active_sequencer) return 0;
    return g_active_sequencer->channels[channel].program;
}

MidiSequencer* midi_get_active_sequencer(void) {
    return g_active_sequencer;
}

/* ------------------------------------------------------------------
 * midi_process_tick: NOJME's media.c calls this in some legacy paths
 * (e.g. when the wall-clock driven approach was used). With Sonivox
 * the song clock is driven by EAS_Render, so this is a no-op.
 * ------------------------------------------------------------------ */
void midi_process_tick(MidiSequencer* seq) {
    (void) seq;
}
