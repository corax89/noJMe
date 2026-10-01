/*
 * J2ME Emulator - Complete JSR-135 Mobile Media API Implementation
 * javax.microedition.media native methods
 * 
 * Full implementation with:
 * - Asynchronous audio playback via SDL callback
 * - Real-time state tracking
 * - MIDI, WAV, Tone sequence support
 * - VolumeControl, MIDIControl implementations
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>
#if defined(_WIN32)
/* No <pthread.h> on plain MinGW targets — use the Win32 shim */
#include "win_thread_shim.h"
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "jvm.h"
#include "native.h"
#include "midi.h"
#include "heap.h"
#include "opcodes.h"

/* v34.69: AMR-NB support via the vendored opencore-amrnb decoder
 * (Apache License 2.0, src/amr/opencore-amr-LICENSE). The decoder tree is
 * plain C since v34.69 (was C-style C++ until v34.68), so it is always
 * compiled in — no C++ toolchain is needed anywhere in the build. */
#ifndef NO_AMR_DECODER
#include "amr/amr_nb_dec.h"
#endif

/* v36.34: MP3 (MPEG-1/2/2.5 Layer III) via minimp3 - single-header,
 * public-domain, pure C (77 KB header, no stdio, no per-frame allocation).
 * MINIMP3_NO_SIMD keeps one deterministic scalar code path across the three
 * toolchains (devkitA64 / host gcc / MinGW syntax audit); scalar minimp3
 * still decodes far faster than realtime on Switch silicon. */
#ifndef NO_MP3_DECODER
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#endif

/* Forward declarations from SDL audio */
extern int sdl_audio_init_simple(uint32_t sample_rate);
extern void sdl_audio_shutdown(void);
extern void sdl_audio_queue_samples(const int16_t* samples, size_t count);
extern size_t sdl_audio_get_queued_size(void);
extern int sdl_audio_device_alive(void); /* v34.94: no-device backoff */

/* [WILDGUARD] v36.50: детектор мусорных указателей микшера (класс Ryujinx
 * InvalidAccess "es/Yeti " на аудио-потоке, вспышки с шагом 2-3 мс). */
#include "wildguard.h"

/* ============================================
 * JSR-135 Constants
 * ============================================ */

#define PLAYER_UNREALIZED   100
#define PLAYER_REALIZED     200
#define PLAYER_PREFETCHED   300
#define PLAYER_STARTED      400
#define PLAYER_CLOSED       0

#define TIME_UNKNOWN        -1L

#define TONE_MIN_NOTE    0
#define TONE_MAX_NOTE    127
#define TONE_MIN_VOLUME  0
#define TONE_MAX_VOLUME  100
#define TONE_MAX_DURATION 30000

/* ToneControl sequence tokens */
#define TONE_CONTROL_VERSION       -2
#define TONE_CONTROL_TEMPO         -3
#define TONE_CONTROL_RESOLUTION    -4
#define TONE_CONTROL_BLOCK_START   -5
#define TONE_CONTROL_BLOCK_END     -6
#define TONE_CONTROL_PLAY_BLOCK    -7
#define TONE_CONTROL_SET_VOLUME    -8
#define TONE_CONTROL_REPEAT        -9
#define TONE_CONTROL_SILENCE       -1

/* Audio buffer constants */
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 4096
#define MAX_PLAYERS 32

/* v34.27: saturating mix. The old `int16 += int16` wrapped around on loud
 * passages and when several players/notes overlapped (32767+1 -> -32768) -
 * the wrap is heard as harsh crackle. All mixers now sum in int32 and
 * clamp. */
static inline int16_t sat16(int32_t v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

/* v34.27: one frame of audio at the 44100 source rate is mixed into this
 * BSS scratch (was malloc/memset/free per call, 200x/s). Safe: mixers only
 * run under g_audio_mutex, and in each build exactly one thread generates. */
#define MAX_MIX_SAMPLES 4096
static int16_t g_mix_scratch[MAX_MIX_SAMPLES * 2];
static int16_t g_midi_scratch[MAX_MIX_SAMPLES * 2];

/* ============================================
 * WAV File Support
 * ============================================ */

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint8_t* data;
    uint32_t data_size;
    uint32_t duration_ms;
} WavFile;

/* ============================================
 * IMA ADPCM Decoder
 * ============================================ */

static const int ima_step_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int ima_step_size_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767
};

/* Decode IMA ADPCM data to 16-bit little-endian interleaved PCM.
 * Uses per-channel block layout: for stereo, left block then right block.
 * Each channel block: 4 header bytes (int16 predictor, uint8 index, uint8 reserved)
 * followed by data bytes containing nibble pairs.
 * Returns 0 on success, -1 on failure. Caller must free *out_pcm. */
static int decode_ima_adpcm(const uint8_t* adpcm_data, uint32_t adpcm_size,
                             int channels, int block_align,
                             uint8_t** out_pcm, uint32_t* out_pcm_size) {
    if (!adpcm_data || adpcm_size == 0 || channels < 1 || channels > 2 || block_align == 0)
        return -1;

    int block_size_per_ch = block_align / channels;
    if (block_size_per_ch < 6)
        return -1; /* Need at least 4 header + 2 data bytes per channel */

    int samples_per_block = (block_size_per_ch - 4) * 2;
    if (samples_per_block <= 0)
        return -1;

    int num_blocks = (int)(adpcm_size / (uint32_t)block_align);
    if (num_blocks == 0)
        return -1;

    uint32_t total_samples_per_ch = (uint32_t)num_blocks * (uint32_t)samples_per_block;
    *out_pcm_size = total_samples_per_ch * (uint32_t)channels * 2;
    *out_pcm = (uint8_t*)malloc(*out_pcm_size);
    if (!*out_pcm)
        return -1;

    int16_t* pcm = (int16_t*)(*out_pcm);

    for (int blk = 0; blk < num_blocks; blk++) {
        uint32_t block_start = (uint32_t)blk * (uint32_t)block_align;
        uint32_t base_sample = (uint32_t)blk * (uint32_t)samples_per_block;

        /* Decode each channel's sub-block independently */
        for (int ch = 0; ch < channels; ch++) {
            uint32_t pos = block_start + (uint32_t)ch * (uint32_t)block_size_per_ch;

            if (pos + 3 >= adpcm_size) break;

            /* Read block header: predictor (int16 LE), index (uint8), reserved (1 byte) */
            int predictor = (int16_t)(adpcm_data[pos] | (adpcm_data[pos + 1] << 8));
            int index = adpcm_data[pos + 2] & 0xFF;
            if (index > 88) index = 88;
            pos += 4; /* skip header */

            /* Decode nibbles from data bytes */
            int data_bytes = block_size_per_ch - 4;
            int sample_idx = 0;

            for (int b = 0; b < data_bytes && sample_idx < samples_per_block; b++) {
                if (pos >= adpcm_size) break;
                uint8_t byte_val = adpcm_data[pos++];

                for (int nib = 0; nib < 2 && sample_idx < samples_per_block; nib++) {
                    int nibble = (nib == 0) ? (byte_val & 0x0F) : ((byte_val >> 4) & 0x0F);

                    int step = ima_step_size_table[index];
                    int diff = step >> 3;
                    if (nibble & 1) diff += step >> 2;
                    if (nibble & 2) diff += step >> 1;
                    if (nibble & 4) diff += step;

                    if (nibble & 8)
                        predictor -= diff;
                    else
                        predictor += diff;

                    if (predictor < -32768) predictor = -32768;
                    if (predictor > 32767) predictor = 32767;

                    index += ima_step_index_table[nibble];
                    if (index < 0) index = 0;
                    if (index > 88) index = 88;

                    /* Write sample to interleaved PCM buffer */
                    uint32_t out_idx = (base_sample + (uint32_t)sample_idx) * (uint32_t)channels + (uint32_t)ch;
                    pcm[out_idx] = (int16_t)predictor;
                    sample_idx++;
                }
            }
        }
    }

    return 0;
}

/* v23: G.711 expand to 16-bit PCM, classic Sun g711 reference implementation.
 * v34.73 FIXES (A-law):
 *  a) segment extraction was `(a & 0x70) ^ 0x55` — a byte value that never
 *     equals any switch case, so every sample took `default` and decoded
 *     into segment 0 (near-silence);
 *  b) the per-segment left shift was missing entirely (the old switch form
 *     `t += 0x108..0x708` tops out at 2040 — a ~16x attenuation). The
 *     correct A-law ladder is (mantissa+0x108) << (seg-1), reaching 32256 —
 *     the same full scale as mu-law.
 * v34.73 FIX (format map): WAVE_FORMAT 6 = A-law, 7 = mu-law per mmreg.h
 * (the old code had them swapped, so a standard mu-law WAV decoded through
 * the A-law table and vice versa). */
static int16_t mulaw_to_pcm16(uint8_t u_val) {
    const int BIAS = 0x84;
    u_val = (uint8_t)(~u_val);
    int t = ((u_val & 0x0F) << 3) + BIAS;
    t <<= (u_val & 0x70) >> 4;
    return (int16_t)((u_val & 0x80) ? (BIAS - t) : (t - BIAS));
}

static int16_t alaw_to_pcm16(uint8_t a_val) {
    int t, seg;
    a_val ^= 0x55;                        /* flip sign/segment/mantissa bits */
    t = (a_val & 0x0F) << 4;
    seg = (a_val & 0x70) >> 4;
    if (seg == 0)
        t += 8;                           /* segment 0: 8..248, step 16 */
    else
        t = (t + 0x108) << (seg - 1);     /* v34.73 FIX: per-segment shift was
                                           * missing — correct ladder: 264..504,
                                           * 528..1008, 1056..2016, ... up to
                                           * 32256, same full scale as mu-law */
    return (int16_t)((a_val & 0x80) ? t : -t);
}

static WavFile* wav_parse(const uint8_t* data, uint32_t size) {
    if (size < 44) return NULL;
    if (memcmp(data, "RIFF", 4) != 0) return NULL;
    if (memcmp(data + 8, "WAVE", 4) != 0) return NULL;
    
    WavFile* wav = (WavFile*)calloc(1, sizeof(WavFile));
    if (!wav) return NULL;
    
    uint32_t pos = 12;
    while (pos + 8 <= size) {
        uint32_t chunk_size = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | ((uint32_t)(data[pos+7]) <<  24);
        
        if (pos + 8 + chunk_size > size) break;
        
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            pos += 8;
            if (chunk_size >= 16) {
                wav->audio_format = data[pos] | (data[pos+1] << 8);
                wav->num_channels = data[pos+2] | (data[pos+3] << 8);
                wav->sample_rate = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | ((uint32_t)(data[pos+7]) <<  24);
                wav->byte_rate = data[pos+8] | (data[pos+9] << 8) | (data[pos+10] << 16) | ((uint32_t)(data[pos+11]) <<  24);
                wav->block_align = data[pos+12] | (data[pos+13] << 8);
                wav->bits_per_sample = data[pos+14] | (data[pos+15] << 8);
            }
            pos += chunk_size;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            pos += 8;
            wav->data = (uint8_t*)malloc(chunk_size);
            if (wav->data) {
                memcpy(wav->data, data + pos, chunk_size);
                wav->data_size = chunk_size;
                if (wav->byte_rate > 0) {
                    wav->duration_ms = (chunk_size * 1000) / wav->byte_rate;
                }
            }
            pos += chunk_size;
        } else {
            pos += 8 + chunk_size;
        }
        if (chunk_size & 1) pos++;
    }
    
    /* Decode IMA ADPCM (format 17) to PCM */
    if (wav->audio_format == 17) {
        uint8_t* pcm_data = NULL;
        uint32_t pcm_size = 0;

        uint32_t adpcm_raw_size = wav->data_size;
        if (decode_ima_adpcm(wav->data, wav->data_size, wav->num_channels,
                             wav->block_align, &pcm_data, &pcm_size) == 0 && pcm_data) {
            free(wav->data);
            wav->data = pcm_data;
            wav->data_size = pcm_size;
            wav->audio_format = 1;
            wav->bits_per_sample = 16;
            wav->block_align = wav->num_channels * 2;
            wav->byte_rate = wav->sample_rate * wav->block_align;
            wav->duration_ms = (wav->data_size * 1000) / wav->byte_rate;
            MEDIA_DEBUG("IMA ADPCM decoded: %u -> %u bytes PCM", adpcm_raw_size, pcm_size);
        } else {
            MEDIA_DEBUG("IMA ADPCM decode failed");
            free(wav->data);
            free(wav);
            return NULL;
        }
    }

    /* v34.73 FIX: G.711 expand — WAVE_FORMAT 6 = A-law, 7 = mu-law (mmreg.h;
     * the old code had the two swapped). Standard-conforming mu-law WAVs now
     * decode through the mu-law table instead of A-law. */
    if (wav->audio_format == 6 || wav->audio_format == 7) {
        uint16_t g711_fmt = wav->audio_format;
        if (wav->data && wav->data_size > 0) {
            uint32_t n = wav->data_size;
            int16_t* pcm = (int16_t*)malloc((size_t)n * 2);
            if (pcm) {
                const uint8_t* src = wav->data;
                for (uint32_t i = 0; i < n; i++) {
                    pcm[i] = (g711_fmt == 7) ? mulaw_to_pcm16(src[i])
                                             : alaw_to_pcm16(src[i]);
                }
                free(wav->data);
                wav->data = (uint8_t*)pcm;
                wav->data_size = n * 2;
                wav->audio_format = 1;
                wav->bits_per_sample = 16;
                wav->block_align = wav->num_channels * 2;
                wav->byte_rate = wav->sample_rate * wav->block_align;
                if (wav->byte_rate > 0) {
                    wav->duration_ms = (wav->data_size * 1000) / wav->byte_rate;
                }
                MEDIA_DEBUG("G.711 %s decoded: %u bytes PCM",
                            g711_fmt == 7 ? "mu-law" : "A-law", wav->data_size);
            } else {
                MEDIA_DEBUG("G.711 decode: out of memory");
                free(wav->data);
                free(wav);
                return NULL;
            }
        }
    }

    if (wav->audio_format != 1 || !wav->data) {
        free(wav->data);
        free(wav);
        return NULL;
    }
    
    MEDIA_DEBUG("WAV parsed: %d ch, %d Hz, %d bits, %u ms",
            wav->num_channels, wav->sample_rate, wav->bits_per_sample, wav->duration_ms);
    
    return wav;
}

static void wav_free(WavFile* wav) {
    if (wav) {
        free(wav->data);
        free(wav);
    }
}

/* ============================================
 * v34.53: AMR-NB Decoding (3GPP storage format, "#!AMR" magic)
 *
 * Root cause of the 3D Ferrari Experience "individual squeaks":
 * the game plays its engine sound via
 *     Manager.createPlayer(is, "audio/amr")
 * and the old auto-detect only knew MThd/RIFF magics, so the AMR
 * payload fell through to the tone-sequence fallback. tone_sequence_parse
 * accepts ANY two bytes, so 16 KB of compressed speech was rendered as
 * hundreds of random short notes - exactly the "squeaks" the user heard.
 *
 * The decoded stream is wrapped in a WavFile (16-bit PCM, mono, 8000 Hz)
 * so the existing WAV mixer (which already handles arbitrary source rates
 * via fractional resampling) plays it with zero new mixer code.
 * ============================================ */

/* AMR-NB storage frame sizes: payload bytes AFTER the TOC byte per FT.
 * Matches opencore's WmfDecBytesPerFrame (TS 26.101): speech bits/8,
 * rounded up. FT 0..7 speech modes (4.75..12.2 kbps), 8 = AMR SID,
 * 9..11 = EFR SID variants (GSM/TDMA/PDC - payloads 6/5/5, decoder
 * conceals them), 12..14 invalid (payload treated as 0 = skip byte),
 * 15 = NO_DATA / transmission-off.
 * v34.53 FIX: the first draft of this table had 21/25 bytes for FT6/FT7
 * (those are NOT the IETF sizes) - files containing 10.2/12.2 kbps frames
 * (the common voice/ring modes, e.g. every BlackShark .amr) desynced by
 * +5/+6 bytes per frame and decoded as noise. Ferrari's engine loop is
 * all-FT0 (4.75), which is why it alone survived the wrong table. */
static const uint8_t amrnb_ft_payload[16] = {
    12, 13, 15, 17, 19, 20, 26, 31,
    5,                              /* AMR SID            */
    6, 5, 5,                        /* EFR SIDs (GSM/TDMA/PDC) */
    0, 0,                           /* invalid            */
    0                               /* 15: NO_DATA        */
};

#ifndef NO_AMR_DECODER
static WavFile* amr_decode_to_wav(const uint8_t* data, uint32_t size) {
    if (!data || size < 7 || memcmp(data, "#!AMR", 5) != 0) return NULL;

    uint32_t pos = 6; /* skip "#!AMR\n" */
    /* tolerate stray CR/LF some encoders emit after the magic */
    while (pos < size && (data[pos] == '\n' || data[pos] == '\r')) pos++;

    void* dec = nojme_amrnb_dec_init();
    if (!dec) return NULL;

    /* First pass: count frames (bounded: AMR-NB is 50 frames/s) */
    uint32_t frames = 0;
    uint32_t p = pos;
    while (p < size) {
        uint8_t ft = (data[p] >> 3) & 0x0F;
        uint32_t payload = (ft <= 8) ? amrnb_ft_payload[ft] : 0;
        if (p + 1 + payload > size) break; /* truncated tail */
        frames++;
        p += 1 + payload;
    }
    if (frames == 0) {
        nojme_amrnb_dec_exit(dec);
        return NULL;
    }

    /* Sanity cap: ~10 minutes at 50 frames/s */
    if (frames > 30000) frames = 30000;

    WavFile* wav = (WavFile*)calloc(1, sizeof(WavFile));
    if (!wav) {
        nojme_amrnb_dec_exit(dec);
        return NULL;
    }
    wav->audio_format   = 1;    /* PCM */
    wav->num_channels   = 1;    /* AMR-NB is mono */
    wav->sample_rate    = NOJME_AMRNB_PCM_RATE;
    wav->bits_per_sample = 16;
    wav->block_align    = 2;
    wav->byte_rate      = NOJME_AMRNB_PCM_RATE * 2;

    uint32_t samples = frames * (uint32_t)NOJME_AMRNB_PCMDIM;
    wav->data = (uint8_t*)malloc((size_t)samples * 2);
    if (!wav->data) {
        free(wav);
        nojme_amrnb_dec_exit(dec);
        return NULL;
    }
    wav->data_size = samples * 2;
    wav->duration_ms = (uint32_t)((uint64_t)samples * 1000 / NOJME_AMRNB_PCM_RATE);

    /* Second pass: decode */
    short* out = (short*)wav->data;
    p = pos;
    while (p < size && frames > 0) {
        uint8_t toc = data[p];
        uint8_t ft = (toc >> 3) & 0x0F;
        uint32_t payload = (ft <= 8) ? amrnb_ft_payload[ft] : 0;
        if (p + 1 + payload > size) break;

        /* FT 9..14 are invalid: pass with erasure so the decoder
         * conceals instead of exploding; FT 15 = NO_DATA is valid. */
        int bfi = (ft >= 9 && ft <= 14) ? 1 : 0;
        nojme_amrnb_decode(dec, &data[p], out, bfi);
        out += NOJME_AMRNB_PCMDIM;
        p += 1 + payload;
        frames--;
    }

    nojme_amrnb_dec_exit(dec);
    MEDIA_DEBUG("AMR-NB decoded: %u bytes -> %u ms PCM",
                size, wav->duration_ms);
    return wav;
}
#endif /* !NO_AMR_DECODER */

/* ============================================
 * v36.34: MP3 decoding (MPEG-1/2/2.5 Layer III) via minimp3
 *
 * Field reality: a growing set of games ship background music as
 * createPlayer(is, "audio/mpeg") MP3s. The old dispatcher returned null
 * for those (v22 contract: games null-check and skip), so whole audio
 * tracks were silent. minimp3 decodes the stream once at realize time
 * into the very same WavFile container the AMR path uses, so playback,
 * volume, looping, fractional resampling and END_OF_MEDIA events all
 * come from the existing WAV mixer with zero new mixer code.
 *
 * Budget: MP3 -> PCM is ~10:1, so a 5 MB track becomes ~50 MB of native
 * (NOT Java-heap) memory - fine on Switch; a 256 MB hard cap guards
 * against corrupt/hostile streams. ID3v1 trailing tags need no handling
 * (no sync), ID3v2 is skipped explicitly.
 * ============================================ */

#ifndef NO_MP3_DECODER
static bool mp3_has_magic(const uint8_t* data, uint32_t len) {
    if (!data || len < 4) return false;
    /* ID3v2 tag (the common MP3 container prefix) */
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') return true;
    /* MPEG audio frame sync: 11111111 111VVLLG (V=version, L=layer) */
    if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        uint8_t version = (uint8_t)((data[1] >> 3) & 0x03);
        uint8_t layer   = (uint8_t)((data[1] >> 1) & 0x03);
        if (version != 0x01 && layer != 0x00) return true; /* not reserved */
    }
    return false;
}

static WavFile* mp3_decode_to_wav(const uint8_t* data, uint32_t size) {
    if (!data || size < 4 || !mp3_has_magic(data, size)) return NULL;

    uint32_t pos = 0;
    /* skip ID3v2 (syncsafe size, optional footer) */
    if (size >= 10 && memcmp(data, "ID3", 3) == 0) {
        uint32_t tag = ((uint32_t)(data[6] & 0x7F) << 21) |
                       ((uint32_t)(data[7] & 0x7F) << 14) |
                       ((uint32_t)(data[8] & 0x7F) <<  7) |
                       (uint32_t)(data[9] & 0x7F);
        pos = 10 + tag + ((data[5] & 0x10) ? 10u : 0u);
        if (pos >= size) return NULL;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);

    /* Growable interleaved-PCM buffer; ~10x the stream size at typical
     * bitrates, hard-capped at 256 MB of output. */
    size_t cap = 1u << 20;
    size_t used = 0;
    uint8_t* pcm = (uint8_t*)malloc(cap);
    if (!pcm) return NULL;
    int channels = 0, hz = 0;
    int frames_ok = 0, resyncs = 0;

    while (pos < size) {
        mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int n = mp3dec_decode_frame(&dec, data + pos, (int)(size - pos),
                                    frame, &info);
        if (info.frame_bytes > 0) {
            pos += (uint32_t)info.frame_bytes;
        } else {
            /* no frame at this offset: resync byte-by-byte (bounded) */
            if (++resyncs > 16384) break;
            pos++;
            continue;
        }
        if (n > 0 && info.channels >= 1 && info.channels <= 2 && info.hz > 0) {
            size_t need = used + (size_t)n * (size_t)info.channels * 2;
            if (need > (256u << 20)) {
                MEDIA_DEBUG("MP3: output cap hit, truncated");
                break;
            }
            if (need > cap) {
                while (cap < need) cap <<= 1;
                uint8_t* np = (uint8_t*)realloc(pcm, cap);
                if (!np) break;
                pcm = np;
            }
            memcpy(pcm + used, frame, (size_t)n * (size_t)info.channels * 2);
            used += (size_t)n * (size_t)info.channels * 2;
            channels = info.channels;
            hz = info.hz;
            frames_ok++;
        }
    }

    if (frames_ok == 0 || used == 0 || !channels || !hz) {
        free(pcm);
        MEDIA_DEBUG("MP3: no decodable frames (%u bytes)", size);
        return NULL;
    }

    WavFile* wav = (WavFile*)calloc(1, sizeof(WavFile));
    if (!wav) { free(pcm); return NULL; }
    wav->audio_format    = 1;
    wav->num_channels    = (uint16_t)channels;
    wav->sample_rate     = (uint32_t)hz;
    wav->bits_per_sample = 16;
    wav->block_align     = (uint16_t)(channels * 2);
    wav->byte_rate       = (uint32_t)hz * (uint32_t)(channels * 2);
    wav->data            = pcm;
    wav->data_size       = (uint32_t)used;
    wav->duration_ms     = (uint32_t)(((uint64_t)used /
                            (uint32_t)(channels * 2)) * 1000u / (uint32_t)hz);
    MEDIA_DEBUG("MP3 decoded: %u bytes -> %u ms PCM (%d ch, %d Hz, resyncs %d)",
                size, wav->duration_ms, channels, hz, resyncs);
    return wav;
}
#endif /* !NO_MP3_DECODER */

/* v34.53: does this data start with the AMR (NB or WB) magic? */
static bool amr_has_magic(const uint8_t* data, uint32_t len, bool* is_wb) {
    if (!data || len < 6 || memcmp(data, "#!AMR", 5) != 0) return false;
    if (is_wb) {
        *is_wb = (len >= 8 && memcmp(data + 5, "-WB", 3) == 0);
    }
    return true;
}

/* ============================================
 * Tone Sequence Support
 * ============================================ */

typedef struct ToneSequence {
    uint8_t* sequence;
    int length;
    int position;
    int tempo;
    int resolution;
    bool playing;
    bool loop;
    int loop_count;
    int loops_remaining;
    uint32_t start_time_ms;
    int current_note;
    int current_duration;
    int current_volume;
    /* v36.48 [TONE-POS-FIX]: длина заголовка (VERSION/TEMPO/RESOLUTION) в
     * байтах — точка, с которой начинаются ноты. tone_sequence_parse
     * оставляла её в ts->position, но native_player_start сбрасывала
     * position=0, и микшер читал БАЙТЫ ЗАГОЛОВКА как ноты: маркер версии
     * (-2) пропускался, а следующий байт (значение версии 1) становился
     * нотой 1 длительностью 253 тика (байт темпо как unsigned) — ЛОЖНАЯ
     * нота на ~8 с до настоящей мелодии и отложенный END_OF_MEDIA.
     * Страдали все игры с ToneControl-джинглами. */
    int header_len;
    /* v34.27: Manager.playTone(note, duration, volume) specifies the
     * duration in MILLISECONDS, not tempo ticks. The old code ran it
     * through the tick formula (60000/tempo/resolution with defaults
     * 120/64) and one-shot tones rang ~7.8x longer than requested. */
    bool one_shot_ms;
} ToneSequence;

static ToneSequence* tone_sequence_parse(const uint8_t* data, int length) {
    if (length < 2) return NULL;
    
    ToneSequence* ts = (ToneSequence*)calloc(1, sizeof(ToneSequence));
    if (!ts) return NULL;
    
    ts->sequence = (uint8_t*)malloc(length);
    if (!ts->sequence) {
        free(ts);
        return NULL;
    }
    memcpy(ts->sequence, data, length);
    ts->length = length;
    ts->position = 0;
    ts->tempo = 120;
    ts->resolution = 64;
    ts->playing = false;
    ts->current_volume = 100;
    
    /* Parse header */
    int pos = 0;
    while (pos < length - 1) {
        int cmd = (int8_t)data[pos];
        
        if (cmd == TONE_CONTROL_VERSION) {
            pos += 2;
        } else if (cmd == TONE_CONTROL_TEMPO) {
            if (pos + 1 < length) {
                ts->tempo = data[pos + 1] & 0xFF;
                if (ts->tempo < 5) ts->tempo = 5;
                if (ts->tempo > 127) ts->tempo = 127;
            }
            pos += 2;
        } else if (cmd == TONE_CONTROL_RESOLUTION) {
            if (pos + 1 < length) {
                ts->resolution = data[pos + 1] & 0xFF;
                if (ts->resolution == 0) ts->resolution = 64;
            }
            pos += 2;
        } else {
            break;
        }
    }
    
    ts->position = pos;
    ts->header_len = pos;   /* [TONE-POS-FIX] v36.48: старт перезапуска здесь, не 0 */
    return ts;
}

static void tone_sequence_free(ToneSequence* ts) {
    if (ts) {
        free(ts->sequence);
        free(ts);
    }
}

/* ============================================
 * Audio Mixer State
 * ============================================ */

typedef enum {
    PLAYER_TYPE_NONE = 0,
    PLAYER_TYPE_MIDI,
    PLAYER_TYPE_WAV,
    PLAYER_TYPE_TONE,
    PLAYER_TYPE_TONE_DEVICE
} PlayerType;

typedef struct MediaPlayer {
    int id;
    PlayerType type;
    volatile int state;
    bool playing;
    bool looping;
    int loop_count;
    int loops_remaining;
    bool muted;
    float volume;
    
    /* WAV playback */
    WavFile* wav;
    uint32_t wav_position;
    uint32_t wav_samples_played;
    double wav_read_pos;   /* v23: fractional source-frame read position */
    
    /* MIDI playback */
    MidiFile* midi;
    uint32_t midi_start_time;
    uint32_t midi_current_time;
    int midi_current_event;
    
    /* Tone playback */
    ToneSequence* tone_seq;
    uint32_t tone_start_time;
    uint32_t tone_note_end_time;
    /* v34.27 tone synth state: continuous phase accumulator (see
     * mix_tone_player) and time-into-current-note in nanoseconds. */
    double tone_phase;
    uint64_t tone_note_time_ns;
    /* v36.27: per-player MIDI mix clock (wall-clock) УДАЛЁН — секвенсор
     * теперь двигается на время рендеренных сэмплов (audio clock),
     * см. mix_midi_player. Поля-часов больше не нужно: продвижение
     * детерминировано объёмом рендера и не зависит от пейсинга потока. */
    
    /* Original data */
    uint8_t* data;
    uint32_t size;
    
    char* content_type;

    /* v23: PlayerListener support. java_ref/listener are GC-rooted. */
    JavaObject* java_ref;   /* the PlayerImpl object handed to Java */
    JavaObject* listener;   /* registered PlayerListener or NULL */
} MediaPlayer;

static MediaPlayer g_players[MAX_PLAYERS];
static int g_next_player_id = 1;
static bool g_audio_initialized = false;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;
/* v34.95: hoisted to file scope so media_shutdown_full() can join safely —
 * the flag is set exactly once (process lifetime), never cleared. */
static int g_audio_thread_created = 0;
/* v35.13: the mixer thread sets this right before returning, so the exit
 * path can bound its wait instead of an unbounded pthread_join (a wedged
 * mixer must never hang the exit — _exit(0) right after kills it anyway). */
static volatile int g_audio_thread_ack = 0;

/* v17: Nokia Sound mixer (defined at end of file) */
void nokia_sound_mix_all(int16_t* buffer, int samples);

/* Libretro mode flag - when true, audio is generated synchronously by
 * libretro_process_audio() (frontend thread, once per retro_run).
 * v34.27 FIX: the guard used to be `#ifdef J2ME_LIBRETRO`, but NOTHING
 * ever defined that macro - the libretro Makefile passes -DLIBRETRO - so
 * every real libretro build silently ran the DESKTOP path: media.c spun up
 * its own 5 ms background thread that mixed a FULL 4096-sample buffer per
 * cycle (93 ms of audio every 5 ms - 18x realtime overproduction). That
 * thread fed sdl_audio_queue_samples, which under the old stubs wrote raw
 * 44100 Hz samples into a ring the frontend drained at 22050: permanent
 * overflow, 10 ms blocking writes, dropped chunks, doubled-speed audio.
 * This was the actual "crackle + performance drop" root cause. */
#if defined(J2ME_LIBRETRO) || defined(LIBRETRO)
static bool g_libretro_mode = true;
#else
static bool g_libretro_mode = false;
#endif

/* Global time base */
static uint64_t g_start_time_us = 0;

/* ============================================
 * v23: PlayerListener event queue.
 * Playback completion happens on the audio thread (mixers) while listener
 * callbacks must run on the JVM thread, so events are queued here and
 * delivered by midp_media_poll_events() from the interpreter yield hook.
 * ============================================ */

#define MEDIA_EV_STARTED      0
#define MEDIA_EV_STOPPED      1
#define MEDIA_EV_END_OF_MEDIA 2

typedef struct {
    int player_id;
    uint8_t event;
} MediaPendingEvent;

#define MEDIA_EVQ_SIZE 32
static MediaPendingEvent g_media_evq[MEDIA_EVQ_SIZE];
static volatile int g_media_evq_count = 0;
static pthread_mutex_t g_media_ev_mutex = PTHREAD_MUTEX_INITIALIZER;

static void media_push_event(int player_id, uint8_t event) {
    pthread_mutex_lock(&g_media_ev_mutex);
    if (g_media_evq_count < MEDIA_EVQ_SIZE) {
        g_media_evq[g_media_evq_count].player_id = player_id;
        g_media_evq[g_media_evq_count].event = event;
        g_media_evq_count++;
    }
    pthread_mutex_unlock(&g_media_ev_mutex);
}

/* ============================================
 * Time Utilities
 * ============================================ */

static uint64_t get_time_us(void) {
#if defined(_WIN32)
    /* No clock_gettime on plain MinGW — use the high-res Win32 counter */
    #include <windows.h>
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

static uint32_t get_time_ms(void) {
    return (uint32_t)(get_time_us() / 1000ULL);
}

/* ============================================
 * Audio Mixer - Generates samples for SDL
 * ============================================ */

/* Mix one player's audio into buffer */

/* v23: read one frame at a fractional source-frame position with linear
 * interpolation (replaces the old nearest-neighbor resampler that caused
 * aliasing artifacts for 8/11/22.05 kHz content). */
static void wav_read_frame_lerp(const WavFile* wav, double pos, int16_t* l, int16_t* r) {
    int frame_bytes = wav->num_channels * (wav->bits_per_sample / 8);
    uint32_t frames = frame_bytes > 0 ? wav->data_size / (uint32_t)frame_bytes : 0;
    if (frames == 0) { *l = 0; *r = 0; return; }
    if (pos < 0.0) pos = 0.0;

    uint32_t i0 = (uint32_t)pos;
    if (i0 >= frames) { *l = 0; *r = 0; return; }
    uint32_t i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
    float frac = (float)(pos - (double)i0);

    int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
    const uint8_t* d = wav->data;

    if (wav->bits_per_sample == 8) {
        l0 = (int16_t)((d[i0 * (uint32_t)frame_bytes] - 128) * 256);
        r0 = (wav->num_channels >= 2)
             ? (int16_t)((d[i0 * (uint32_t)frame_bytes + 1] - 128) * 256) : l0;
        if (i1 != i0) {
            l1 = (int16_t)((d[i1 * (uint32_t)frame_bytes] - 128) * 256);
            r1 = (wav->num_channels >= 2)
                 ? (int16_t)((d[i1 * (uint32_t)frame_bytes + 1] - 128) * 256) : l1;
        }
    } else { /* 16-bit PCM */
        uint32_t o0 = i0 * (uint32_t)frame_bytes;
        if (o0 + 1 < wav->data_size)
            l0 = (int16_t)(d[o0] | (d[o0 + 1] << 8));
        if (wav->num_channels >= 2 && o0 + 3 < wav->data_size)
            r0 = (int16_t)(d[o0 + 2] | (d[o0 + 3] << 8));
        else if (wav->num_channels < 2)
            r0 = l0;  /* v34.73 FIX: 16-bit MONO was silent in the right
                       * channel (r0 stayed 0) - every mono PCM WAV and the
                       * AMR-NB decode (mono 16-bit) played LEFT-ONLY on
                       * stereo frontends. The 8-bit branch already copied
                       * l0 into r0; now 16-bit does too. */
        if (i1 != i0) {
            uint32_t o1 = i1 * (uint32_t)frame_bytes;
            if (o1 + 1 < wav->data_size)
                l1 = (int16_t)(d[o1] | (d[o1 + 1] << 8));
            if (wav->num_channels >= 2 && o1 + 3 < wav->data_size)
                r1 = (int16_t)(d[o1 + 2] | (d[o1 + 3] << 8));
            else if (wav->num_channels < 2)
                r1 = l1;
        }
    }

    *l = (int16_t)(l0 + (l1 - l0) * frac);
    *r = (int16_t)(r0 + (r1 - r0) * frac);
}

static void mix_wav_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->wav || !player->playing) return;
    /* [WILDGUARD] v36.50: мусорный wav-указатель (класс Ryujinx InvalidAccess
     * "es/Yeti ") не разыменовывается — репорт + тихий скип такта. */
    if (WILDGUARD_SKIP(player->wav, "mix:wav")) return;

    WavFile* wav = player->wav;
    float vol = player->muted ? 0.0f : player->volume;
    int frame_bytes = wav->num_channels * (wav->bits_per_sample / 8);
    uint32_t total_frames = frame_bytes > 0 ? wav->data_size / (uint32_t)frame_bytes : 0;
    double src_ratio = (double)wav->sample_rate / AUDIO_SAMPLE_RATE;
    
    for (int i = 0; i < samples && player->playing; i++) {
        /* v23: fractional source position; end-of-media detection on it */
        if (player->wav_read_pos >= (double)total_frames) {
            if (player->looping && player->loop_count != 1) {
                player->wav_read_pos = 0.0;
                player->wav_position = 0;
                player->wav_samples_played = 0;
                if (player->loop_count > 0) {
                    player->loops_remaining--;
                    if (player->loops_remaining <= 0) {
                        player->playing = false;
                        player->state = PLAYER_PREFETCHED;
                        media_push_event(player->id, MEDIA_EV_END_OF_MEDIA);
                        break;
                    }
                }
            } else {
                player->playing = false;
                player->state = PLAYER_PREFETCHED;
                media_push_event(player->id, MEDIA_EV_END_OF_MEDIA);
                break;
            }
        }
        
        int16_t left = 0, right = 0;
        wav_read_frame_lerp(wav, player->wav_read_pos, &left, &right);

        /* Apply volume and mix (v34.27: saturating) */
        buffer[i * 2]     = sat16((int32_t)buffer[i * 2]     + (int32_t)(left * vol));
        buffer[i * 2 + 1] = sat16((int32_t)buffer[i * 2 + 1] + (int32_t)(right * vol));
        
        player->wav_read_pos += src_ratio;
        player->wav_position = (uint32_t)(player->wav_read_pos * frame_bytes);
        player->wav_samples_played++;
    }
}

static void mix_tone_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->tone_seq || !player->playing) return;
    if (WILDGUARD_SKIP(player->tone_seq, "mix:tone")) return; /* [WILDGUARD] */

    ToneSequence* ts = player->tone_seq;

    /* Guard against division by zero in duration calculation */
    if (ts->tempo == 0) ts->tempo = 120;
    if (ts->resolution == 0) ts->resolution = 64;

    float vol = player->muted ? 0.0f : player->volume;

    /* v34.27 REWRITE. The old implementation had three audible/visible
     * defects:
     *  1. The tone phase was computed from integer-MILLISECOND wall-clock
     *     stamps (get_time_ms() per sample, and the in-sample offset was
     *     1000000/44100 = 22us by integer division instead of 22.68us), so
     *     the sine phase jumped every millisecond -> constant ~1kHz
     *     clicking, on top of ~51k clock syscalls per second of audio on
     *     armv7 (a large share of the reported performance drop).
     *  2. powf(440*2^((n-69)/12)) ran PER SAMPLE; it is now cached per note.
     *  3. Note advancement compared ACCUMULATED elapsed time against each
     *     note's own duration, so any note shorter than the total elapsed
     *     time was skipped instantly (fast/short melodies collapsed into
     *     a blur). Timing is now a per-note sample counter with remainder
     *     carry, driven by the audio clock, not the wall clock. */
    float freq = 0.0f;
    int freq_note = -999;
    const uint64_t sample_ns = 1000000000ULL / AUDIO_SAMPLE_RATE;

    for (int i = 0; i < samples && player->playing; i++) {
        /* Generate audio for current note (if any) */
        if (ts->current_note >= 0 && ts->current_note < 128) {
            if (ts->current_note != freq_note) {
                freq_note = ts->current_note;
                freq = 440.0f * powf(2.0f, (ts->current_note - 69) / 12.0f);
            }
            float amplitude = ((float)ts->current_volume / 100.0f) * vol;

            player->tone_phase += (float)(2.0 * M_PI * (double)freq /
                                          (double)AUDIO_SAMPLE_RATE);
            if (player->tone_phase >= (float)(2.0 * M_PI)) {
                player->tone_phase -= (float)(2.0 * M_PI);
            }

            float sample = amplitude * sinf(player->tone_phase);
            int16_t pcm = (int16_t)(sample * 16000.0f); /* reasonable volume */
            buffer[i * 2]     = sat16((int32_t)buffer[i * 2]     + pcm);
            buffer[i * 2 + 1] = sat16((int32_t)buffer[i * 2 + 1] + pcm);
        }

        player->tone_note_time_ns += sample_ns;

        /* Check if we need to advance to next note.
         * Duration math order matches the original ms formula:
         * duration * 60000 / tempo / resolution (here in ns).
         * one_shot_ms: Manager.playTone duration is milliseconds. */
        uint64_t note_end_ns = ts->one_shot_ms
            ? (uint64_t)(uint32_t)ts->current_duration * 1000000ULL
            : (uint64_t)(uint32_t)ts->current_duration * 60000000000ULL /
              (uint64_t)(uint32_t)ts->tempo /
              (uint64_t)(uint32_t)ts->resolution;
        if (player->tone_note_time_ns >= note_end_ns) {
            player->tone_note_time_ns -= note_end_ns; /* carry remainder */

            /* Parse next note from sequence */
            if (ts->position >= ts->length - 1) {
                /* End of sequence */
                if (player->looping && player->loop_count != 1) {
                    ts->position = 0;
                    if (player->loop_count > 0) {
                        player->loops_remaining--;
                        if (player->loops_remaining <= 0) {
                            player->playing = false;
                            player->state = PLAYER_PREFETCHED;
                            ts->current_note = -1;
                            break;
                        }
                    }
                } else {
                    player->playing = false;
                    player->state = PLAYER_PREFETCHED;
                    ts->current_note = -1;
                    break;
                }
            }

            /* Read next note command */
            int8_t cmd = (int8_t)ts->sequence[ts->position];

            if (cmd == TONE_CONTROL_SET_VOLUME && ts->position + 1 < ts->length) {
                ts->current_volume = ts->sequence[ts->position + 1] & 0xFF;
                ts->position += 2;
            } else if (cmd == TONE_CONTROL_REPEAT && ts->position + 1 < ts->length) {
                int repeat_count = ts->sequence[ts->position + 1] & 0xFF;
                ts->loop_count = repeat_count;
                ts->position += 2;
            } else if (cmd == TONE_CONTROL_SILENCE && ts->position + 1 < ts->length) {
                int duration = ts->sequence[ts->position + 1] & 0xFF;
                ts->current_note = -1;
                ts->current_duration = duration;
                ts->position += 2;
            } else if (cmd >= 0 && ts->position + 1 < ts->length) {
                /* Note: note, duration */
                int note = cmd;
                int duration = ts->sequence[ts->position + 1] & 0xFF;

                ts->current_note = note;
                ts->current_duration = duration;
                ts->position += 2;
            } else {
                ts->position++;
            }
        }
    }

    /* v23: natural end of a tone sequence -> END_OF_MEDIA */
    if (!player->playing && player->tone_seq && ts->current_note == -1) {
        MEDIA_DEBUG("tone end: player id=%d -> END_OF_MEDIA", player->id);
        media_push_event(player->id, MEDIA_EV_END_OF_MEDIA);
    }
}

/* Mix MIDI player - generates samples using FM synthesis */
static void mix_midi_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->midi || !player->playing) return;
    if (WILDGUARD_SKIP(player->midi, "mix:midi")) return; /* [WILDGUARD] v36.50 */
    if (WILDGUARD_SKIP(player->midi->sequencer, "mix:seq")) return;

    if (samples > MAX_MIX_SAMPLES) samples = MAX_MIX_SAMPLES; /* v34.27 */

    int16_t* temp_buf = g_midi_scratch;
    memset(temp_buf, 0, (size_t)samples * 2 * sizeof(int16_t));

    /* v36.27 TEMPO FIX («музыка в Asphalt 3 3D замедлена»): секвенсор
     * двигается на время РЕНДЕРЕННЫХ СЭМПЛОВ (аудио-часы), а НЕ по
     * настенным часам. Прежний wall-clock advance был корректен только
     * для непрерывного продюсера: после v34.94 продюсер пейсится
     * очередью SDL — стеновые промежутки между микс-вызовами НЕ равны
     * длительности рендеренного звука (5 мс звука за вызов при
     * промежутках 2-5 мс сна+overhead, плюс простои «очередь полна —
     * не производим», когда song time вообще не двигался, а буфер
     * играл). Итог: song time набегал МЕДЛЕННЕЕ слышимого звука —
     * музыка звучала растянутой (темп = доля времени производства от
     * общего времени). Это та же болезнь, что v23 лечил у tone-плеера
     * («driven by the audio clock, not the wall clock»), только у MIDI.
     * Продвижение ровно на samples/44100 секунд синхронно с рендером:
     * темп точен при любом пейсинге, недогруз/бурсты/паузы очереди на
     * темп не влияют вовсе; при underrun песня честно «стоит» вместе
     * со звуком. */
    if (player->midi->sequencer->playing) {
        uint64_t audio_us =
            (uint64_t)samples * 1000000ULL / AUDIO_SAMPLE_RATE;
        midi_advance_time(player->midi, audio_us);
    }

    /* Generate samples using the MIDI synthesizer into temp buffer */
    midi_generate_samples(player->midi, temp_buf, samples);

    /* v36.28 DIAG (ПОСЛЕ генерации): раз в ~1 c — жизненный пульс
     * MIDI-плеера: сэмплы реально рендерятся? время идёт? ноты живы? */
    {
        static uint32_t s_midi_diag_seq = 0;
        static uint32_t s_midi_diag_last[16];
        int slot = player->id < 16 ? player->id : 0;
        s_midi_diag_seq++;
        if (s_midi_diag_seq - s_midi_diag_last[slot] >= 200) {
            s_midi_diag_last[slot] = s_midi_diag_seq;
            int32_t peak = 0;
            for (int i = 0; i < samples * 2; i++) {
                int32_t a = temp_buf[i] < 0 ? -temp_buf[i] : temp_buf[i];
                if (a > peak) peak = a;
            }
            MEDIA_DEBUG("mix id=%d pos_ms=%u dur_ms=%u playing=%d "
                        "peak=%d vol=%d muted=%d ev=%u notes=%u "
                        "active=%d/%d prog0=%d chvol0=%d",
                        player->id,
                        (unsigned)midi_get_position_ms(player->midi),
                        (unsigned)(player->midi->sequencer->total_time_us / 1000),
                        player->midi->sequencer->playing ? 1 : 0,
                        (int)peak, (int)(player->volume * 100.0f),
                        player->muted ? 1 : 0,
                        player->midi->sequencer->diag_events,
                        player->midi->sequencer->diag_notes,
                        player->midi->sequencer->active_note_count,
                        player->midi->sequencer->max_active_notes,
                        player->midi->sequencer->channels[0].program,
                        player->midi->sequencer->channels[0].volume);
        }
    }

    /* Apply player volume and mix into shared buffer (v34.27: saturating) */
    float vol = player->muted ? 0.0f : player->volume;
    for (int i = 0; i < samples * 2; i++) {
        buffer[i] = sat16((int32_t)buffer[i] + (int32_t)(temp_buf[i] * vol));
    }

    /* Check if MIDI playback finished */
    if (!player->midi->sequencer->playing) {
        if (player->looping && player->loop_count != 1) {
            /* Restart MIDI */
            midi_play(player->midi);
            if (player->loop_count > 0) {
                player->loops_remaining--;
                if (player->loops_remaining <= 0) {
                    player->playing = false;
                    player->state = PLAYER_PREFETCHED;
                    media_push_event(player->id, MEDIA_EV_END_OF_MEDIA);
                }
            }
        } else {
            player->playing = false;
            player->state = PLAYER_PREFETCHED;
            /* v23: non-loop MIDI reached the end (or was stopped) - only
             * push END_OF_MEDIA when the sequencer itself ran through */
            if (player->midi->sequencer->end_reached) {
                media_push_event(player->id, MEDIA_EV_END_OF_MEDIA);
                player->midi->sequencer->end_reached = false;
            }
        }
    }
}

/* Audio thread - continuously generates samples (DESKTOP/SDL build only;
 * the libretro build generates synchronously in libretro_process_audio).
 *
 * v34.94 PACING FIX: the old loop produced a 5 ms slice then slept 5 ms —
 * the production rate was purely a function of usleep precision. ANY drift
 * accumulated in the SDL queue forever (measured on Switch: backlog grew
 * ~66 samples/s, stall time climbed in lockstep, then the frontend froze;
 * the user's diag showed sb 0 -> 156 -> 486 -> 812). The loop now targets
 * a QUEUE LEVEL: produce only while the queue is below the target band,
 * otherwise just sleep — production is slave to consumption, drift cannot
 * accumulate, and the queue cap in sdl_audio_queue_samples is the last
 * line of defence. */
#define AUDIO_QUEUE_TARGET_BYTES  (24 * 1024)  /* ~68 ms @44.1 kHz stereo S16 */
#define AUDIO_QUEUE_FLOOR_BYTES   (8 * 1024)   /* ~45 ms — refill below this */
static void* audio_thread_func(void* arg) {
    (void)arg;

    int16_t* mix_buffer = (int16_t*)malloc(AUDIO_BUFFER_SIZE * 2 * sizeof(int16_t));
    if (!mix_buffer) return NULL;

    const int samples_per_cycle = AUDIO_SAMPLE_RATE / 200 + 1; /* ~5 ms */

    while (g_audio_running) {
        /* v34.94: no device (audio off / between sessions) — do not burn
         * CPU mixing into a dropped queue. */
        if (!sdl_audio_device_alive()) {
            usleep(20000);
            continue;
        }

        /* v34.94: wait until the queue needs a refill (backpressure). */
        size_t queued = (size_t)sdl_audio_get_queued_size() * 2 * sizeof(int16_t);
        if (queued > AUDIO_QUEUE_TARGET_BYTES) {
            /* Over target: idle ~5 ms and re-check. Costs no CPU, keeps
             * latency bounded, and NEVER queues more than the target. */
            usleep(5000);
            continue;
        }

        /* Clear buffer */
        memset(mix_buffer, 0, (size_t)samples_per_cycle * 2 * sizeof(int16_t));

        pthread_mutex_lock(&g_audio_mutex);

        /* Mix all active players */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!g_players[i].playing) continue;

            switch (g_players[i].type) {
                case PLAYER_TYPE_WAV:
                    mix_wav_player(&g_players[i], mix_buffer, samples_per_cycle);
                    break;
                case PLAYER_TYPE_TONE:
                case PLAYER_TYPE_TONE_DEVICE:
                    mix_tone_player(&g_players[i], mix_buffer, samples_per_cycle);
                    break;
                case PLAYER_TYPE_MIDI:
                    mix_midi_player(&g_players[i], mix_buffer, samples_per_cycle);
                    break;
                default:
                    break;
            }
        }

        /* v17: mix Nokia Sound (com.nokia.mid.sound) channels */
        nokia_sound_mix_all(mix_buffer, samples_per_cycle);

        pthread_mutex_unlock(&g_audio_mutex);

        /* Queue samples for SDL (drops internally when over the hard cap) */
        sdl_audio_queue_samples(mix_buffer, (size_t)samples_per_cycle * 2);

        /* v34.94: pace to the queue level instead of a blind 5 ms: if we
         * are still below the floor after queuing, short sleep (fast
         * refill); otherwise the next loop's over-target check sleeps. */
        queued = (size_t)sdl_audio_get_queued_size() * 2 * sizeof(int16_t);
        if (queued < AUDIO_QUEUE_FLOOR_BYTES) {
            usleep(2000); /* queue nearly empty: top up quickly */
        } else {
            usleep(4000); /* near target: gentle pacing */
        }
    }

    g_audio_thread_ack = 1; /* v35.13: shutdown wait sees this */
    free(mix_buffer);
    return NULL;
}

/* ============================================
 * Audio Initialization
 * ============================================ */
static void ensure_audio_init(void) {
    if (!g_audio_initialized) {
        sdl_audio_init_simple(AUDIO_SAMPLE_RATE);
        midi_init(AUDIO_SAMPLE_RATE);

        g_start_time_us = get_time_us();

        /* In libretro mode, don't create audio thread - generate synchronously */
        /* [FL-CHK] corruption hunt: allow disabling the background audio thread */
        static int no_audio_thread = -1;
        if (no_audio_thread < 0) {
            const char* e = getenv("NOJME_NO_AUDIO_THREAD");
            no_audio_thread = (e && e[0] == '1') ? 1 : 0;
        }
        /* v34.94: guard against a SECOND thread after media_session_reset()
         * cleared g_audio_initialized (the first thread keeps running). */
        if (!g_libretro_mode && !no_audio_thread && !g_audio_thread_created) {
            g_audio_running = true;
            g_audio_thread_ack = 0; /* v35.13: rearm for the shutdown wait */
            if (pthread_create(&g_audio_thread, NULL, audio_thread_func, NULL) == 0) {
                g_audio_thread_created = 1;
            }
        }

        g_audio_initialized = true;
        MEDIA_DEBUG("Audio initialized (libretro_mode=%d)", g_libretro_mode);
    }
}

/* ========================================================================
 * v34.94 SESSION LIFECYCLE FIX (exit crash + midlet-switch freeze).
 * g_players[] is PROCESS-GLOBAL and used to SURVIVE jvm_destroy:
 *  - slots kept playing=true and dangling java_ref/listener roots pointing
 *    into the destroyed heap (the audio thread kept "mixing" them and the
 *    NEXT session's find_player() could match a stale slot by a reused
 *    object address);
 *  - the media event queue carried events for dead players;
 *  - session 2 then raced a corrupted mix state -> random crash on exit
 *    and freeze on "close midlet -> open another".
 * The Switch frontend calls this between sessions (main.c teardown). The
 * audio THREAD keeps running (it only touches session-independent state:
 * g_players + native buffers + the SDL queue) — resetting under the same
 * mutex the mixer holds makes the reset race-free without join timing. */
void media_session_reset(void) {
    pthread_mutex_lock(&g_audio_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        MediaPlayer* p = &g_players[i];
        if (p->type == PLAYER_TYPE_NONE) continue;
        /* Release NATIVE media buffers; do NOT touch java_ref/listener:
         * the JVM (and its root table) is being torn down by the caller. */
        if (p->midi)  { midi_stop(p->midi); midi_free(p->midi); p->midi = NULL; }
        if (p->wav)   { wav_free(p->wav); p->wav = NULL; }
        if (p->tone_seq) { tone_sequence_free(p->tone_seq); p->tone_seq = NULL; }
        free(p->data); p->data = NULL;
        free(p->content_type); p->content_type = NULL;
        p->java_ref = NULL;
        p->listener = NULL;
        p->playing = false;
        p->state = PLAYER_UNREALIZED;
        p->type = PLAYER_TYPE_NONE;
        memset(p, 0, sizeof(MediaPlayer));
    }
    pthread_mutex_unlock(&g_audio_mutex);

    /* Drop queued PlayerListener events for dead players. */
    pthread_mutex_lock(&g_media_ev_mutex);
    g_media_evq_count = 0;
    pthread_mutex_unlock(&g_media_ev_mutex);

    /* Re-arm init so the next session re-opens the audio device (the
     * frontend closes it in sdl_switch_game_end) and re-inits cleanly. */
    g_audio_initialized = false;
    MEDIA_DEBUG("media session reset done");
}

/* v34.95: FINAL media teardown at process exit. media_session_reset()
 * deliberately keeps the audio thread alive for cross-session reuse; at
 * APP EXIT nothing may outlive main(): a lingering thread that still
 * touches SDL (queueing into a device SDL_Quit is tearing down, or
 * re-arming the audio subsystem) is the prime suspect for the
 * exit-hang/crash class. Order: stop the thread, JOIN it, only then
 * close the device — no queue-vs-close race by construction. */
void media_shutdown_full(void) {
    g_audio_running = false;
    if (g_audio_thread_created) {
        /* v35.13 BOUNDED SHUTDOWN: the mixer exits within ~20 ms of
         * g_audio_running=false; wait for its ack (up to 1 s) and only
         * then join. If it never acks (mutex wedged by a dead VM thread —
         * the "hung midlet" exit-crash class), detach instead of hanging.
         * v36.02 CORRECTION: _exit(0) does NOT kill threads atomically on
         * HOS (it returns to HBmenu through the loader unmap), so the
         * detach path is a last resort that logs loudly — the normal path
         * joins the mixer right here, before main() continues teardown. */
        for (int i = 0; i < 100 && !g_audio_thread_ack; i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
            nanosleep(&ts, NULL);
        }
        if (g_audio_thread_ack) {
            pthread_join(g_audio_thread, NULL);
        } else {
            pthread_detach(g_audio_thread);
            LOG_SAFE("[EXIT] audio mixer did not ack in 1 s — detached "
                     "(will die with the process)\n");
        }
        g_audio_thread_created = 0;
    }
    {
        extern void sdl_audio_close(void* ctx);
        sdl_audio_close(NULL);
    }
    MEDIA_DEBUG("media shutdown full done");
}

/* Generate audio samples synchronously - called from libretro_process_audio() */
void media_generate_audio_samples(int samples) {
    /* v34.73 DIAG: NOJME_AUDDBG=1 — one-shot trace of the generator guards
     * (was the audio subsystem initialized? is the libretro sync path on?). */
    {
        static int s_adbg = -1;
        if (s_adbg < 0) {
            const char* e = getenv("NOJME_AUDDBG");
            s_adbg = (e && e[0] == '1') ? 1 : 0;
            if (s_adbg)
                fprintf(stderr, "[AUDDBG] gen_samples: init=%d libretro=%d samples=%d\n",
                        (int)g_audio_initialized, (int)g_libretro_mode, samples);
        }
    }
    if (!g_audio_initialized || g_libretro_mode == false) return;
    if (samples <= 0) return;
    if (samples > MAX_MIX_SAMPLES) samples = MAX_MIX_SAMPLES; /* v34.27 */

    /* v34.45 DIAG: NOJME_AUDIOPROF=1 — per-player-class sub-phase timing
     * (mutex wait vs WAV vs tone vs MIDI vs nokia). Silent when off. */
    static int s_aprof = -1;
    if (s_aprof < 0) {
        const char* e = getenv("NOJME_AUDIOPROF");
        s_aprof = (e && e[0] == '1') ? 1 : 0;
    }
    /* v34.65 MinGW: zero-init both stamps — ap_t0 is only read inside
     * APROF_STAMP after the s_aprof-gated seeding below, but MinGW's flow
     * analysis cannot prove it and flags may-be-uninitialized. */
    struct timespec ap_t0 = {0, 0}, ap_t1 = {0, 0};
    double ap_lock_ms = 0, ap_wav_ms = 0, ap_tone_ms = 0, ap_midi_ms = 0, ap_nokia_ms = 0;
    #define APROF_STAMP(var) do { if (s_aprof) { \
        clock_gettime(CLOCK_MONOTONIC, &ap_t1); \
        var += (ap_t1.tv_sec - ap_t0.tv_sec) * 1000.0 + (ap_t1.tv_nsec - ap_t0.tv_nsec) / 1e6; \
        ap_t0 = ap_t1; } } while (0)

    /* v34.27: BSS scratch instead of malloc/memset/free per call. */
    int16_t* mix_buffer = g_mix_scratch;
    memset(mix_buffer, 0, (size_t)samples * 2 * sizeof(int16_t));

    if (s_aprof) clock_gettime(CLOCK_MONOTONIC, &ap_t0);
    pthread_mutex_lock(&g_audio_mutex);
    APROF_STAMP(ap_lock_ms);

    /* Mix all active players */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].playing) continue;

        switch (g_players[i].type) {
            case PLAYER_TYPE_WAV:
                mix_wav_player(&g_players[i], mix_buffer, samples);
                APROF_STAMP(ap_wav_ms);
                break;
            case PLAYER_TYPE_TONE:
            case PLAYER_TYPE_TONE_DEVICE:
                mix_tone_player(&g_players[i], mix_buffer, samples);
                APROF_STAMP(ap_tone_ms);
                break;
            case PLAYER_TYPE_MIDI:
                mix_midi_player(&g_players[i], mix_buffer, samples);
                APROF_STAMP(ap_midi_ms);
                break;
            default:
                break;
        }
    }

    /* v17: mix Nokia Sound (com.nokia.mid.sound) channels */
    nokia_sound_mix_all(mix_buffer, samples);
    APROF_STAMP(ap_nokia_ms);

    pthread_mutex_unlock(&g_audio_mutex);

    if (s_aprof) {
        static uint32_t s_aprof_n = 0;
        if ((++s_aprof_n % 300) == 1) {
            char l[192];
            int ln = snprintf(l, sizeof(l),
                "[AUDIOPROF] n=%u samples=%d lock=%.2fms wav=%.2fms tone=%.2fms midi=%.2fms nokia=%.2fms\n",
                s_aprof_n, samples, ap_lock_ms, ap_wav_ms, ap_tone_ms, ap_midi_ms, ap_nokia_ms);
            if (ln > 0) fwrite(l, 1, (size_t)ln, stderr);
        }
    }

    /* Queue samples for libretro */
    sdl_audio_queue_samples(mix_buffer, (size_t)samples * 2);
    #undef APROF_STAMP
}

/* ============================================
 * Player Management
 * ============================================ */

static int find_free_player_slot(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].type == PLAYER_TYPE_NONE) {
            return i;
        }
    }
    return -1;
}

static MediaPlayer* find_player(JavaObject* obj) {
    if (!obj) return NULL;
    int id = obj->fields[0].i;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].id == id && g_players[i].type != PLAYER_TYPE_NONE) {
            return &g_players[i];
        }
    }
    return NULL;
}

static MediaPlayer* find_player_by_id(int id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].id == id && g_players[i].type != PLAYER_TYPE_NONE) {
            return &g_players[i];
        }
    }
    return NULL;
}

/* ============================================
 * Manager Native Methods
 * ============================================ */

static JavaValue native_manager_createPlayer_locator(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* locator_str = (JavaString*)args[0].ref;
    
    ensure_audio_init();
    
    const char* locator = locator_str ? string_utf8(jvm, locator_str) : NULL;
    MEDIA_DEBUG("Manager.createPlayer: locator=%s", locator ? locator : "null");
    
    int slot = find_free_player_slot();
    if (slot < 0) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Too many open players");
        return NATIVE_RETURN_NULL();
    }
    
    MediaPlayer* player = &g_players[slot];
    /* v23: defensively drop stale GC roots from a previous player in this slot */
    if (player->java_ref) jvm_remove_root(jvm, player->java_ref);
    if (player->listener) jvm_remove_root(jvm, player->listener);
    memset(player, 0, sizeof(MediaPlayer));
    player->id = g_next_player_id++;
    player->volume = 1.0f;
    player->state = PLAYER_UNREALIZED;
    
    if (locator && strcmp(locator, "device://tone") == 0) {
        player->type = PLAYER_TYPE_TONE_DEVICE;
        player->tone_seq = (ToneSequence*)calloc(1, sizeof(ToneSequence));
        player->tone_seq->tempo = 120;
        player->tone_seq->resolution = 64;
        player->tone_seq->current_volume = 100;
        player->content_type = strdup("audio/x-tone-seq");
    } else if (locator && strcmp(locator, "device://midi") == 0) {
        /* v23: attach a live (empty, looping) synth to the player so that
         * MIDIControl drives THIS player's synthesizer, not just the global
         * active sequencer. */
        player->type = PLAYER_TYPE_MIDI;
        player->midi = midi_load_empty();
        player->looping = true;
        player->loop_count = -1;
        player->loops_remaining = -1;
        player->content_type = strdup("audio/midi");
        if (!player->midi) {
            jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                              "Cannot create MIDI device player");
            return NATIVE_RETURN_NULL();
        }
    } else {
        /* v23 JSR-135: unsupported/invalid locator must raise MediaException
         * (Java used to receive NULL and crash with an NPE instead). */
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Cannot create player for locator");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create PlayerImpl object */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Player implementation unavailable");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Cannot allocate player");
        return NATIVE_RETURN_NULL();
    }
    
    player_obj->fields[0].i = player->id;
    if (player_class->fields_count >= 2) {
        player_obj->fields[1].i = player->state;
    }
    
    /* v23: keep the Java-side object alive for listener delivery */
    player->java_ref = player_obj;
    jvm_add_root(jvm, player_obj);
    
    return NATIVE_RETURN_OBJECT(player_obj);
}

static JavaValue native_manager_createPlayer_stream(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* stream = (JavaObject*)args[0].ref;
    JavaString* type_str = (JavaString*)args[1].ref;

    ensure_audio_init();
    
    if (!stream) {
        jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", "null stream");
        return NATIVE_RETURN_NULL();
    }
    
    /* Read data from the stream.
     * v34.3 FIX (user reports: "Manager.createPlayer threw exception!" for
     * AMR/MIDI resources): old code read buf/pos/count ONLY from the
     * immediate stream object. Games routinely pass a DataInputStream (or
     * another wrapper) to Manager.createPlayer - a wrapper has no buf field,
     * so every call threw MediaException("Empty or unreadable media stream")
     * even though the wrapped ByteArrayInputStream held valid data. Walk the
     * "in" chain (max 5 hops) to the ByteArrayInputStream, same as the
     * DataInputStream read path does (v32 SU-30 fix). */
    JavaArray* buf = NULL;
    int pos = 0, count = 0;
    {
        JavaObject* s = stream;
        for (int hop = 0; hop < 5 && s; hop++) {
            JavaClass* sc = s->header.clazz;
            const char* sname = (sc && sc->class_name) ? sc->class_name : NULL;
            if (!sname) break;
            if (strcmp(sname, "java/io/ByteArrayInputStream") == 0) {
                buf = (JavaArray*)native_get_field_value(s, "buf").ref;
                pos = native_get_field_value(s, "pos").i;
                count = native_get_field_value(s, "count").i;
                /* Self-heal: a BAIS built through a path that never ran its
                 * field initializer may have buf set but count==0; trust the
                 * array length in that case (the stream was just created and
                 * not yet read). */
                if (buf && count <= 0 && buf->length > 0) count = (int)buf->length;
                break;
            }
            s = (JavaObject*)native_get_field_value(s, "in").ref;
        }
    }
    
    if (!buf || count <= 0) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Empty or unreadable media stream");
        return NATIVE_RETURN_NULL();
    }
    
    int data_len = count - pos;
    if (data_len <= 0) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Empty media stream");
        return NATIVE_RETURN_NULL();
    }
    
    uint8_t* data = (uint8_t*)array_data(buf) + pos;
    
    int slot = find_free_player_slot();
    if (slot < 0) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Too many open players");
        return NATIVE_RETURN_NULL();
    }
    
    MediaPlayer* player = &g_players[slot];
    if (player->java_ref) jvm_remove_root(jvm, player->java_ref);
    if (player->listener) jvm_remove_root(jvm, player->listener);
    memset(player, 0, sizeof(MediaPlayer));
    player->id = g_next_player_id++;
    player->data = (uint8_t*)malloc(data_len);
    if (!player->data) {
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Out of memory");
        return NATIVE_RETURN_NULL();
    }
    memcpy(player->data, data, data_len);
    player->size = data_len;
    player->volume = 1.0f;
    player->state = PLAYER_UNREALIZED;

    /* v34.53: content type from the Java caller, normalized to lowercase
     * with any ";params" suffix stripped. NULL when the game passed null
     * or an empty string. */
    char* ctype = NULL;
    if (type_str) {
        ctype = jvm_string_to_utf8(jvm, type_str);
        if (ctype) {
            char* p = ctype;
            for (; *p; p++) *p = (char)tolower((unsigned char)*p);
            char* semi = strchr(ctype, ';');
            if (semi) *semi = '\0';
            if (!*ctype) { free(ctype); ctype = NULL; }
        }
    }

    /* v34.53: format routing. The caller-declared content type takes
     * priority (that is what JSR-135 specifies); if it names a format the
     * data does not parse as, fall back to magic sniffing so sloppy games
     * that pass a wrong type string keep working (v22/v23b behavior).
     *
     * This is the actual fix for 3D Ferrari Experience: it plays its
     * engine sound as createPlayer(is, "audio/amr"). The old code ignored
     * the type, found neither MThd nor RIFF, and fed the compressed AMR
     * bytes to the tone-sequence fallback - random short "squeaks". */
    bool parsed = false;

    if (ctype && (strcmp(ctype, "audio/midi") == 0 ||
                  strcmp(ctype, "audio/sp-midi") == 0 ||
                  strcmp(ctype, "audio/x-midi") == 0)) {
        if (data_len >= 4 && memcmp(data, "MThd", 4) == 0) {
            player->type = PLAYER_TYPE_MIDI;
            player->midi = midi_load(player->data, player->size);
            if (player->midi) {
                player->content_type = strdup("audio/midi");
                parsed = true;
            }
        }
    } else if (ctype && strcmp(ctype, "audio/amr") == 0) {
        bool is_wb = false;
        if (amr_has_magic(data, (uint32_t)data_len, &is_wb)) {
            if (!is_wb) {
#ifndef NO_AMR_DECODER
                player->type = PLAYER_TYPE_WAV;
                player->wav = amr_decode_to_wav(player->data, player->size);
                if (player->wav) {
                    player->content_type = strdup("audio/amr");
                    parsed = true;
                }
#else
                /* No AMR decoder compiled in (no C++ toolchain at build
                 * time): skip cleanly rather than squeak. */
                MEDIA_DEBUG("createPlayer: AMR data but decoder not built, returning null");
                goto amr_unsupported_null;
#endif
            } else {
                /* AMR-WB: no decoder - return null so the game can skip
                 * the sound instead of mis-playing it. */
                MEDIA_DEBUG("createPlayer: AMR-WB unsupported, returning null");
                goto amr_unsupported_null;
            }
        }
    } else if (ctype && (strcmp(ctype, "audio/x-wav") == 0 ||
                         strcmp(ctype, "audio/wav") == 0 ||
                         strcmp(ctype, "audio/wave") == 0)) {
        if (data_len >= 4 && memcmp(data, "RIFF", 4) == 0) {
            player->type = PLAYER_TYPE_WAV;
            player->wav = wav_parse(player->data, player->size);
            if (player->wav) {
                player->content_type = strdup("audio/x-wav");
                parsed = true;
            }
        }
    } else if (ctype && (strcmp(ctype, "audio/x-tone-seq") == 0 ||
                         strcmp(ctype, "audio/tone") == 0)) {
        player->type = PLAYER_TYPE_TONE;
        player->tone_seq = tone_sequence_parse(player->data, player->size);
        if (player->tone_seq) {
            player->content_type = strdup("audio/x-tone-seq");
            parsed = true;
        }
    } else if (ctype && (strcmp(ctype, "audio/mpeg") == 0 ||
                         strcmp(ctype, "audio/mp3") == 0 ||
                         strcmp(ctype, "audio/x-mp3") == 0)) {
        /* v36.34: MP3 decodes via minimp3 into the shared WavFile PCM
         * path (playback/loop/volume/resampling all inherited). Data that
         * is not actually MP3 keeps the old contract: null player, the
         * game skips the sound (never a throw - that killed loading
         * threads in Brick Breaker Revolution 3D and friends). */
#ifndef NO_MP3_DECODER
        if (mp3_has_magic(data, (uint32_t)data_len)) {
            player->type = PLAYER_TYPE_WAV;
            player->wav = mp3_decode_to_wav(player->data, player->size);
            if (player->wav) {
                player->content_type = strdup("audio/mpeg");
                parsed = true;
            }
        }
#endif
        if (!parsed) {
            MEDIA_DEBUG("createPlayer: MP3 requested (%s) but data is not decodable MP3, returning null", ctype);
            free(ctype);
            return NATIVE_RETURN_NULL();
        }
    } else if (ctype && (strcmp(ctype, "audio/aac") == 0 ||
                         strcmp(ctype, "audio/mp4") == 0 ||
                         strcmp(ctype, "video/mp4") == 0)) {
        /* Compressed formats we do not decode: return null (the v22/v23b
         * contract - games null-check createPlayer and skip the sound;
         * throwing killed loading threads). */
        MEDIA_DEBUG("createPlayer: unsupported type '%s', returning null", ctype);
        free(ctype);
        return NATIVE_RETURN_NULL();
    }

    /* Fallback: sniff the data magic (also the path for callers that
     * passed a null/unknown type). */
    if (!parsed) {
        bool amr_wb = false;
        if (data_len >= 4 && memcmp(data, "MThd", 4) == 0) {
            player->type = PLAYER_TYPE_MIDI;
            player->midi = midi_load(player->data, player->size);
            if (player->midi) {
                player->content_type = strdup("audio/midi");
                parsed = true;
            }
        } else if (amr_has_magic(data, (uint32_t)data_len, &amr_wb)) {
#ifndef NO_AMR_DECODER
            if (!amr_wb) {
                player->type = PLAYER_TYPE_WAV;
                player->wav = amr_decode_to_wav(player->data, player->size);
                if (player->wav) {
                    player->content_type = strdup("audio/amr");
                    parsed = true;
                }
            } else {
                MEDIA_DEBUG("createPlayer: AMR-WB sniffed, unsupported");
            }
#else
            MEDIA_DEBUG("createPlayer: AMR sniffed (WB=%d), decoder not built", (int)amr_wb);
#endif
        } else if (data_len >= 4 && memcmp(data, "RIFF", 4) == 0) {
            player->type = PLAYER_TYPE_WAV;
            player->wav = wav_parse(player->data, player->size);
            if (player->wav) {
                player->content_type = strdup("audio/x-wav");
                parsed = true;
            }
        }
#ifndef NO_MP3_DECODER
        else if (mp3_has_magic(data, (uint32_t)data_len)) {
            /* v36.34: undeclared MP3 data used to fall into the
             * tone-sequence fallback (which accepts ANY two bytes) and
             * rendered compressed frames as random short notes - the exact
             * failure mode AMR had before v34.53. Sniff MP3 before tones. */
            player->type = PLAYER_TYPE_WAV;
            player->wav = mp3_decode_to_wav(player->data, player->size);
            if (player->wav) {
                player->content_type = strdup("audio/mpeg");
                parsed = true;
            }
        }
#endif
        else {
            /* Assume tone sequence */
            player->type = PLAYER_TYPE_TONE;
            player->tone_seq = tone_sequence_parse(player->data, player->size);
            if (player->tone_seq) {
                player->content_type = strdup("audio/x-tone-seq");
                parsed = true;
            }
        }
    }

    free(ctype);

    if (!parsed) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        /* v23b: MediaException only for data that LOOKS like a known format
         * but failed to decode (corrupt MIDI/WAV). Truly unknown byte streams
         * (MP3 raw, AMR, custom codecs...) return NULL again — v22 behavior
         * that Brick Breaker Revolution 3D and friends rely on: they null-
         * check createPlayer() and skip the sound. Throwing here killed the
         * loading thread mid-bar. */
        if ((data_len >= 4 && memcmp(data, "MThd", 4) == 0) ||
            (data_len >= 4 && memcmp(data, "RIFF", 4) == 0)) {
            jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                              "Corrupt or unsupported media data");
        }
        MEDIA_DEBUG("createPlayer: unknown media format, returning null");
        return NATIVE_RETURN_NULL();
    }

    /* v34.53: shared early-exit for AMR variants we cannot decode. */
    goto amr_parse_done;
amr_unsupported_null:
    {
        free(ctype);
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        return NATIVE_RETURN_NULL();
    }
amr_parse_done:
    ;
    
    /* Create PlayerImpl object */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Player implementation unavailable");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        jvm_throw_by_name(jvm, "javax/microedition/media/MediaException",
                          "Cannot allocate player");
        return NATIVE_RETURN_NULL();
    }
    
    player_obj->fields[0].i = player->id;
    if (player_class->fields_count >= 2) {
        player_obj->fields[1].i = player->state;
    }
    
    /* v23: keep the Java-side object alive for listener delivery */
    player->java_ref = player_obj;
    jvm_add_root(jvm, player_obj);
    
    return NATIVE_RETURN_OBJECT(player_obj);
}

/* Manager.playTone - NON-BLOCKING implementation */
static JavaValue native_manager_playTone(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint note = args[0].i;
    jint duration = args[1].i;
    jint volume = args[2].i;
    
    ensure_audio_init();
    
    if (note < TONE_MIN_NOTE || note > TONE_MAX_NOTE) return NATIVE_RETURN_VOID();
    if (duration <= 0 || duration > TONE_MAX_DURATION) return NATIVE_RETURN_VOID();
    
    volume = volume < TONE_MIN_VOLUME ? TONE_MIN_VOLUME : (volume > TONE_MAX_VOLUME ? TONE_MAX_VOLUME : volume);
    
    /* Use async tone playback */
    int velocity = (volume * 127) / 100;
    
    /* Create a temporary tone player for async playback */
    int slot = find_free_player_slot();
    if (slot >= 0) {
        MediaPlayer* player = &g_players[slot];
        memset(player, 0, sizeof(MediaPlayer));
        player->id = g_next_player_id++;
        player->type = PLAYER_TYPE_TONE_DEVICE;
        player->volume = volume / 100.0f;
        player->playing = true;
        player->state = PLAYER_STARTED;
        
        player->tone_seq = (ToneSequence*)calloc(1, sizeof(ToneSequence));
        player->tone_seq->current_note = note;
        player->tone_seq->current_duration = duration;
        player->tone_seq->current_volume = volume;
        player->tone_seq->start_time_ms = get_time_ms();
        player->tone_seq->playing = true;
        player->tone_seq->one_shot_ms = true;  /* v34.27: duration is ms */
        
        midi_note_on(0, note, velocity);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_manager_getSupportedContentTypes(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)args;
    (void)thread; (void)arg_count;
    
#ifdef NO_MP3_DECODER
    const char* types[] = {"audio/midi", "audio/x-wav", "audio/x-tone-seq"};
    int num = 3;
#else
    /* v36.34: audio/mpeg advertised now that MP3 decodes via minimp3 */
    const char* types[] = {"audio/midi", "audio/x-wav", "audio/x-tone-seq", "audio/mpeg"};
    int num = 4;
#endif
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, num, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaString** elems = (JavaString**)array_data(array);
    for (int i = 0; i < num; i++) {
        elems[i] = jvm_new_string(jvm, types[i]);
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

static JavaValue native_manager_getSupportedProtocols(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)args;
    (void)thread; (void)arg_count;
    
    const char* protocols[] = {"file", "http", "device"};
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, 3, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaString** elems = (JavaString**)array_data(array);
    for (int i = 0; i < 3; i++) {
        elems[i] = jvm_new_string(jvm, protocols[i]);
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

/* ============================================
 * Player Native Methods
 * ============================================ */

static JavaValue native_player_realize(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    if (player->state < PLAYER_REALIZED) {
        player->state = PLAYER_REALIZED;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_prefetch(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    if (player->state < PLAYER_REALIZED) player->state = PLAYER_REALIZED;
    if (player->state < PLAYER_PREFETCHED) player->state = PLAYER_PREFETCHED;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_start(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    MEDIA_DEBUG("Player.start: id=%d type=%d", player->id, player->type);
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->state < PLAYER_PREFETCHED) player->state = PLAYER_PREFETCHED;
    
    player->playing = true;
    player->state = PLAYER_STARTED;
    
    if (player->type == PLAYER_TYPE_WAV && player->wav) {
        player->wav_position = 0;
        player->wav_samples_played = 0;
        player->wav_read_pos = 0.0;
    } else if (player->type == PLAYER_TYPE_MIDI && player->midi) {
        midi_set_loop(player->midi, player->looping);
        if (player->midi->sequencer) {
            player->midi->sequencer->end_reached = false;
        }
        midi_play(player->midi);
    } else if ((player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE) && player->tone_seq) {
        player->tone_seq->playing = true;
        player->tone_seq->start_time_ms = get_time_ms();
        /* [TONE-POS-FIX] v36.48: рестарт с ПЕРВОЙ НОТЫ (после заголовка
         * VERSION/TEMPO/RESOLUTION), а не с байта 0 — раньше байты
         * заголовка читались как ноты (см. ToneSequence.header_len). */
        player->tone_seq->position = player->tone_seq->header_len;
        player->tone_phase = 0.0;               /* v34.27: fresh synth state */
        player->tone_note_time_ns = 0;
        player->tone_seq->current_note = -1;    /* silence until first token parsed */
        player->tone_seq->current_duration = 0;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    /* v23: JSR-135 STARTED listener event */
    media_push_event(player->id, MEDIA_EV_STARTED);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_stop(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->type == PLAYER_TYPE_MIDI && player->midi) {
        midi_stop(player->midi);
    } else if (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE) {
        if (player->tone_seq && player->tone_seq->current_note >= 0) {
            midi_note_off(0, player->tone_seq->current_note);
        }
    }
    
    player->playing = false;
    if (player->state == PLAYER_STARTED) {
        player->state = PLAYER_PREFETCHED;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    /* v23: JSR-135 STOPPED listener event (manual stop) */
    media_push_event(player->id, MEDIA_EV_STOPPED);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_deallocate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    player->playing = false;
    if (player->midi) { midi_stop(player->midi); midi_free(player->midi); player->midi = NULL; }
    if (player->wav) { wav_free(player->wav); player->wav = NULL; }
    if (player->tone_seq) { tone_sequence_free(player->tone_seq); player->tone_seq = NULL; }
    player->state = PLAYER_UNREALIZED;
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_close(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    player->playing = false;
    if (player->midi) { midi_stop(player->midi); midi_free(player->midi); player->midi = NULL; }
    if (player->wav) { wav_free(player->wav); player->wav = NULL; }
    if (player->tone_seq) { tone_sequence_free(player->tone_seq); player->tone_seq = NULL; }
    free(player->data);
    free(player->content_type);
    
    /* v23: drop the GC roots before wiping the slot */
    if (player->java_ref) { jvm_remove_root(jvm, player->java_ref); player->java_ref = NULL; }
    if (player->listener) { jvm_remove_root(jvm, player->listener); player->listener = NULL; }
    
    memset(player, 0, sizeof(MediaPlayer));
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_setLoopCount(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    int count = args[1].i;
    
    pthread_mutex_lock(&g_audio_mutex);
    player->loop_count = count;
    player->loops_remaining = count;
    player->looping = (count != 1);
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

/* Player.getDuration - returns duration in microseconds */
static JavaValue native_player_getDuration(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(TIME_UNKNOWN);
    
    jlong duration = TIME_UNKNOWN;
    
    if (player->wav) {
        duration = (jlong)player->wav->duration_ms * 1000LL;
    } else if (player->midi) {
        duration = (jlong)midi_get_duration_ms(player->midi) * 1000LL;
    }
    
    return NATIVE_RETURN_LONG(duration);
}

/* Player.getMediaTime - returns current position in microseconds */
static JavaValue native_player_getMediaTime(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(TIME_UNKNOWN);
    
    jlong time = 0;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->wav && player->wav->sample_rate > 0) {
        /* v23: wav_samples_played counts OUTPUT frames (44.1 kHz mixer rate) */
        time = (jlong)(player->wav_samples_played * 1000000LL / AUDIO_SAMPLE_RATE);
    } else if (player->midi) {
        time = (jlong)midi_get_position_ms(player->midi) * 1000LL;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_LONG(time);
}

/* Player.setMediaTime - seeks to position in microseconds */
static JavaValue native_player_setMediaTime(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(-1);
    
    jlong time_us = args[1].j;
    jlong result = time_us;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->wav && player->wav->sample_rate > 0) {
        /* v23: seek in source frames for the interpolated resampler; report
         * the output-frame position consistently with getMediaTime() */
        double src_pos = (double)time_us * player->wav->sample_rate / 1000000.0;
        int frame_bytes = player->wav->num_channels * (player->wav->bits_per_sample / 8);
        player->wav_read_pos = src_pos;
        player->wav_position = (uint32_t)(src_pos * frame_bytes);
        player->wav_samples_played = (uint32_t)((double)time_us * AUDIO_SAMPLE_RATE / 1000000.0);
        result = time_us;
    } else if (player->midi) {
        midi_set_position_ms(player->midi, (uint32_t)(time_us / 1000));
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_LONG(result);
}

/* Player.getState - REAL implementation with auto-update */
static JavaValue native_player_getState(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_INT(PLAYER_CLOSED);
    
    int state;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    /* Check if playback has ended */
    if (player->state == PLAYER_STARTED && !player->playing) {
        player->state = PLAYER_PREFETCHED;
    }
    
    state = player->state;
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_INT(state);
}

static JavaValue native_player_getContentType(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player || !player->content_type) return NATIVE_RETURN_NULL();
    
    JavaString* str = jvm_new_string(jvm, player->content_type);
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_player_getLocator(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_NULL();
    
    /* Return locator if stored */
    return NATIVE_RETURN_NULL();
}

/* Player.getControl - returns control objects */
static JavaValue native_player_getControl(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    JavaString* type_str = (JavaString*)args[1].ref;
    
    if (!player || !type_str) return NATIVE_RETURN_NULL();
    
    const char* type = string_utf8(jvm, type_str);
    if (!type) return NATIVE_RETURN_NULL();
    
    /* Create appropriate control object */
    JavaClass* ctrl_class = NULL;
    
    if (strstr(type, "VolumeControl")) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/VolumeControlImpl");
    } else if (strstr(type, "MIDIControl") && player->type == PLAYER_TYPE_MIDI) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/MIDIControlImpl");
    } else if (strstr(type, "ToneControl") && 
               (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE)) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/ToneControlImpl");
    }
    
    if (!ctrl_class) return NATIVE_RETURN_NULL();
    
    JavaObject* ctrl_obj = jvm_new_object(jvm, ctrl_class);
    if (ctrl_obj && ctrl_class->fields_count >= 1) {
        ctrl_obj->fields[0].i = player->id;  /* Store player ID */
    }
    
    return NATIVE_RETURN_OBJECT(ctrl_obj);
}

static JavaValue native_player_getControls(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_NULL();
    
    /* Create array with available controls */
    int num_controls = 1;  /* VolumeControl */
    bool has_midi = (player->type == PLAYER_TYPE_MIDI);
    bool has_tone = (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE);
    if (has_midi || has_tone) num_controls = 2;
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, num_controls, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaObject** elems = (JavaObject**)array_data(array);
    
    /* Add VolumeControl */
    JavaClass* vol_class = jvm_load_class(jvm, "javax/microedition/media/control/VolumeControlImpl");
    if (vol_class) {
        JavaObject* vol_obj = jvm_new_object(jvm, vol_class);
        if (vol_obj && vol_class->fields_count >= 1) {
            vol_obj->fields[0].i = player->id;
        }
        elems[0] = vol_obj;
    }
    
    /* v23: fill the second slot (MIDIControl / ToneControl) instead of
     * leaving it NULL */
    if (num_controls == 2) {
        JavaClass* second_class = NULL;
        if (has_midi) {
            second_class = jvm_load_class(jvm, "javax/microedition/media/control/MIDIControlImpl");
        } else if (has_tone) {
            second_class = jvm_load_class(jvm, "javax/microedition/media/control/ToneControlImpl");
        }
        if (second_class) {
            JavaObject* ctrl_obj = jvm_new_object(jvm, second_class);
            if (ctrl_obj && second_class->fields_count >= 1) {
                ctrl_obj->fields[0].i = player->id;
            }
            elems[1] = ctrl_obj;
        }
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

static JavaValue native_player_addPlayerListener(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    JavaObject* listener = (JavaObject*)args[1].ref;
    MEDIA_DEBUG("addPlayerListener: player_by_obj=%s listener=%p",
                player ? "yes" : "NO", (void*)listener);
    if (!player) return NATIVE_RETURN_VOID();

    /* v23: real PlayerListener support - store the listener and keep it
     * alive as a GC root; events are delivered from the yield hook. */
    if (player->listener) {
        jvm_remove_root(jvm, player->listener);
        player->listener = NULL;
    }
    if (listener) {
        player->listener = listener;
        jvm_add_root(jvm, listener);
        MEDIA_DEBUG("addPlayerListener: registered on player id=%d", player->id);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_removePlayerListener(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    if (player->listener) {
        jvm_remove_root(jvm, player->listener);
        player->listener = NULL;
    }
    
    return NATIVE_RETURN_VOID();
}

/* v23: deliver queued PlayerListener events on the JVM thread.
 * Called from the interpreter yield hook (execute.c) so the callback runs
 * in normal Java context, never from the audio thread. */
void midp_media_poll_events(JVM* jvm, JavaThread* thread) {
    if (!jvm || !thread) return;
    if (g_media_evq_count == 0) return;
    if (jvm_exception_pending(jvm)) return;  /* never mix with a pending exception */
    
    for (;;) {
        pthread_mutex_lock(&g_media_ev_mutex);
        if (g_media_evq_count == 0) {
            pthread_mutex_unlock(&g_media_ev_mutex);
            break;
        }
        MediaPendingEvent ev = g_media_evq[0];
        memmove(&g_media_evq[0], &g_media_evq[1],
                sizeof(MediaPendingEvent) * (size_t)(g_media_evq_count - 1));
        g_media_evq_count--;
        pthread_mutex_unlock(&g_media_ev_mutex);
        
        MediaPlayer* player = find_player_by_id(ev.player_id);
        if (!player || !player->listener || !player->java_ref) {
            MEDIA_DEBUG("poll: drop ev=%u player_id=%d (player=%s listener=%s)",
                        (unsigned)ev.event, ev.player_id,
                        player ? "yes" : "NO",
                        (player && player->listener) ? "yes" : "NO");
            continue;
        }
        
        const char* ev_name =
            (ev.event == MEDIA_EV_END_OF_MEDIA) ? "endOfMedia" :
            (ev.event == MEDIA_EV_STARTED)      ? "started" : "stopped";
        
        /* Resolve PlayerListener.playerUpdate(Player, String, Object) */
        JavaObject* lst = player->listener;
        JavaClass* c = lst->header.clazz;
        JavaMethod* m = NULL;
        while (c && !m) {
            for (int i = 0; i < c->methods_count; i++) {
                JavaMethod* mm = &c->methods[i];
                if (mm->name && strcmp(mm->name, "playerUpdate") == 0 &&
                    mm->descriptor &&
                    strcmp(mm->descriptor,
                           "(Ljavax/microedition/media/Player;Ljava/lang/String;Ljava/lang/Object;)V") == 0) {
                    m = mm;
                    break;
                }
            }
            c = c->super_class;
        }
        if (!m) continue;
        
        JavaString* ev_str = jvm_new_string(jvm, ev_name);
        if (!ev_str) continue;
        
        /* [PLAYERUPDATE-ARGS] v36.47 (yetifix4) КРИТИЧЕСКИЙ ФИКС.
         * playerUpdate(Player, String, Object) — метод ЭКЗЕМПЛЯРА: execute_method
         * для не-static методов ждёт args[0] = this, затем по одному значению
         * на параметр дескриптора — всего 4 JavaValue. До v36.47 сюда
         * передавалось ТРИ (args[0]=Player!), и execute_method читал args[3]
         * ЗА ГРАНИЦЕЙ массива — мусор из стека становился eventData-ссылкой:
         *  - Ryujinx: Invalid memory access at 0x20697465592F7365 — это ASCII
         *    "es/Yeti " (байты пути игры ".../games/Yeti Sports...", оказавшиеся
         *    в соседнем слоте стека), 4 чтения подряд;
         *  - устройство: та же порча ломала слушатель игры (музыка главного
         *    меню Yeti Sports) — экран меню замирал при живом цикле рисования
         *    (полевой лог v36.46: fl=110..120, lvc>0, chg=0, dset=0).
         * Поймано AddressSanitizer-репро scripts/ys_asan_journey.sh:
         * stack-buffer-overflow READ of size 8 в execute_method (execute.c:626)
         * из этого места, ровно в момент старта менюшной музыки. */
        JavaValue args[4];
        args[0].ref = (JavaObject*)lst;        /* this: слушатель */
        args[1].ref = player->java_ref;        /* Player          */
        args[2].ref = (JavaObject*)ev_str;     /* event: String   */
        args[3].ref = NULL;                    /* eventData       */

        extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method,
                                  JavaValue* args, JavaValue* result);
        JavaValue result;
        memset(&result, 0, sizeof(result));
        /* JSR-135: exceptions thrown by a listener are ignored. Clear any
         * pending exception afterwards so it can never leak into the outer
         * interpreter frame that happens to be at this yield point.
         * [EXC-DROP-FREE] v36.47: освобождать и C-строки трейса — старый
         * код обнулял только указатель исключения и УТЕКАЛ strdup()
         * exception_stack_trace/exception_throw_info на каждом сброшенном
         * исключении (тот же паттерн, что в изоляторе paint(), display.c). */
        execute_method(jvm, thread, m, args, &result);
        /* v36.48 [PU-DELIVER-TRACE]: полевая видимость доставки — счётчик
         * отвечает на вопрос «доходят ли события до игры» без gdb. */
        { static unsigned pu_delivered_n = 0;
          pu_delivered_n++;
          MEDIA_DEBUG("playerUpdate #%u: player id=%d event=%s -> delivered",
                      pu_delivered_n, player->id, ev_name); }
        if (thread->pending_exception) {
            MEDIA_DEBUG("playerUpdate threw - exception dropped");
            thread->pending_exception = NULL;
            if (thread->exception_stack_trace) {
                free(thread->exception_stack_trace);
                thread->exception_stack_trace = NULL;
            }
            if (thread->exception_throw_info) {
                free(thread->exception_throw_info);
                thread->exception_throw_info = NULL;
            }
        }
    }
}

/* ============================================
 * VolumeControl Native Methods
 * ============================================ */

static JavaValue native_volumecontrol_setLevel(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int level = args[1].i;
    
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        pthread_mutex_lock(&g_audio_mutex);
        player->volume = level / 100.0f;
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

/* v34.53: VolumeControl.setLevel(I)I — NON-standard variant found in
 * commercial MIDlets compiled against vendor SDKs (Motorola et al.):
 * returns the level actually applied. 3D Ferrari Experience, Asphalt 3 and
 * Quake Plus all call this signature, and with only the JSR-135 (I)V
 * variant registered the invokeinterface missed — their volume adjustments
 * silently did nothing (engine loop always at 100%). */
static JavaValue native_volumecontrol_setLevel_ret(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int level = args[1].i;

    if (level < 0) level = 0;
    if (level > 100) level = 100;

    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        pthread_mutex_lock(&g_audio_mutex);
        player->volume = level / 100.0f;
        pthread_mutex_unlock(&g_audio_mutex);
    }

    return NATIVE_RETURN_INT(level);
}

static JavaValue native_volumecontrol_getLevel(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        return NATIVE_RETURN_INT((int)(player->volume * 100));
    }
    return NATIVE_RETURN_INT(100);
}

static JavaValue native_volumecontrol_setMute(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    bool mute = args[1].i != 0;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        pthread_mutex_lock(&g_audio_mutex);
        player->muted = mute;
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_volumecontrol_isMuted(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        return NATIVE_RETURN_INT(player->muted ? 1 : 0);
    }
    return NATIVE_RETURN_INT(0);
}

/* ============================================
 * MIDIControl Native Methods - REAL implementation
 * ============================================ */

static JavaValue native_midicontrol_shortMidiEvent(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int command = args[1].i;
    int data1 = args[2].i;
    int data2 = args[3].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && (player->type == PLAYER_TYPE_MIDI || player->type == PLAYER_TYPE_TONE_DEVICE)) {
        pthread_mutex_lock(&g_audio_mutex);
        
        /* Construct MIDI message and send to the synthesizer.
         * v23: route to THIS player's synth when it has one (device://midi),
         * otherwise fall back to the global active sequencer. */
        MidiFile* target = player->midi;  /* NULL for plain tone devices */
        uint8_t status = (uint8_t)command;
        
        if ((status & 0xF0) == 0x90) {  /* Note On */
            if (target) midi_note_on_file(target, status & 0x0F, data1, data2);
            else midi_note_on(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0x80) {  /* Note Off */
            if (target) midi_note_off_file(target, status & 0x0F, data1);
            else midi_note_off(status & 0x0F, data1);
        } else if ((status & 0xF0) == 0xB0) {  /* Control Change */
            if (target) midi_control_change_file(target, status & 0x0F, data1, data2);
            else midi_control_change(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0xC0) {  /* Program Change */
            if (target) midi_program_change_file(target, status & 0x0F, data1);
            else midi_program_change(status & 0x0F, data1);
        }
        
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_longMidiEvent(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    JavaArray* data = (JavaArray*)args[1].ref;
    int offset = args[2].i;
    int length = args[3].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && data) {
        uint8_t* bytes = (uint8_t*)array_data(data) + offset;
        /* Process long MIDI event */
        (void)bytes;
        (void)length;
    }
    
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_midicontrol_setChannelVolume(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int channel = args[1].i;
    int volume = args[2].i;
    
    pthread_mutex_lock(&g_audio_mutex);
    /* v23: route to the player's own synth when it has one */
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && player->midi) {
        midi_control_change_file(player->midi, (uint8_t)channel, 7, (uint8_t)volume);
    } else {
        midi_control_change((uint8_t)channel, 7, (uint8_t)volume);  /* CC 7 = Channel Volume */
    }
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_getChannelVolume(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int channel = args[1].i;
    
    /* v23: per-player synth state */
    MediaPlayer* player = find_player_by_id(player_id);
    int volume = (player && player->midi)
        ? midi_get_channel_volume_file(player->midi, channel)
        : midi_get_channel_volume(channel);
    return NATIVE_RETURN_INT(volume);
}

static JavaValue native_midicontrol_setProgram(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int channel = args[1].i;
    int bank = args[2].i;
    int program = args[3].i;
    
    pthread_mutex_lock(&g_audio_mutex);
    /* v23: honor bank select (CC0) before the program change */
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && player->midi) {
        midi_control_change_file(player->midi, (uint8_t)channel, 0, (uint8_t)bank);
        midi_program_change_file(player->midi, (uint8_t)channel, (uint8_t)program);
    } else {
        midi_control_change((uint8_t)channel, 0, (uint8_t)bank);
        midi_program_change((uint8_t)channel, (uint8_t)program);
    }
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_getProgram(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int channel = args[1].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    int program = (player && player->midi)
        ? midi_get_program_file(player->midi, channel)
        : midi_get_program(channel);
    return NATIVE_RETURN_INT(program);
}

static JavaValue native_midicontrol_isBankQuerySupported(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    return NATIVE_RETURN_INT(0);  /* Not supported in this implementation */
}

/* ============================================
 * ToneControl Native Methods
 * ============================================ */

static JavaValue native_tonecontrol_setSequence(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    JavaArray* seq = (JavaArray*)args[1].ref;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (!player || !seq) return NATIVE_RETURN_VOID();
    
    uint8_t* data = (uint8_t*)array_data(seq);
    int length = seq->length;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->tone_seq) {
        tone_sequence_free(player->tone_seq);
    }
    
    player->tone_seq = tone_sequence_parse(data, length);
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

/* ============================================
 * VideoControl Native Methods (stub)
 * ============================================ */

static JavaValue native_videocontrol_initDisplayMode(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

static JavaValue native_videocontrol_setDisplayLocation(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getDisplayX(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_videocontrol_getDisplayY(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_videocontrol_setVisible(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getDisplayWidth(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(176);
}

static JavaValue native_videocontrol_getDisplayHeight(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(144);
}

static JavaValue native_videocontrol_getSnapshot(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

static JavaValue native_videocontrol_setDisplaySize(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_setDisplayFullScreen(JVM* jvm, JavaThread* thread,
                                                            JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getSourceWidth(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(176);
}

static JavaValue native_videocontrol_getSourceHeight(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)args;
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(144);
}

/* ============================================
 * TimeBase
 * ============================================ */

static JavaValue native_timebase_getTime(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    uint64_t now = get_time_us();
    return NATIVE_RETURN_LONG((jlong)(now - g_start_time_us));
}

/* ============================================
 * Initialize media native methods
 * ============================================ */

void init_javax_microedition_media(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Manager */
        {"javax/microedition/media/Manager", "createPlayer", 
         "(Ljava/lang/String;)Ljavax/microedition/media/Player;", 
         native_manager_createPlayer_locator},
        {"javax/microedition/media/Manager", "createPlayer",
         "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;",
         native_manager_createPlayer_stream},
        {"javax/microedition/media/Manager", "playTone", "(III)V", native_manager_playTone},
        {"javax/microedition/media/Manager", "getSupportedContentTypes",
         "(Ljava/lang/String;)[Ljava/lang/String;", native_manager_getSupportedContentTypes},
        {"javax/microedition/media/Manager", "getSupportedProtocols",
         "(Ljava/lang/String;)[Ljava/lang/String;", native_manager_getSupportedProtocols},
        
        /* Player */
        {"javax/microedition/media/Player", "realize", "()V", native_player_realize},
        {"javax/microedition/media/Player", "prefetch", "()V", native_player_prefetch},
        {"javax/microedition/media/Player", "start", "()V", native_player_start},
        {"javax/microedition/media/Player", "stop", "()V", native_player_stop},
        {"javax/microedition/media/Player", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/Player", "close", "()V", native_player_close},
        {"javax/microedition/media/Player", "setLoopCount", "(I)V", native_player_setLoopCount},
        {"javax/microedition/media/Player", "getDuration", "()J", native_player_getDuration},
        {"javax/microedition/media/Player", "getMediaTime", "()J", native_player_getMediaTime},
        {"javax/microedition/media/Player", "setMediaTime", "(J)J", native_player_setMediaTime},
        {"javax/microedition/media/Player", "getState", "()I", native_player_getState},
        {"javax/microedition/media/Player", "getContentType", "()Ljava/lang/String;", native_player_getContentType},
        {"javax/microedition/media/Player", "getLocator", "()Ljava/lang/String;", native_player_getLocator},
        {"javax/microedition/media/Player", "getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;", native_player_getControl},
        {"javax/microedition/media/Player", "getControls", "()[Ljavax/microedition/media/Control;", native_player_getControls},
        {"javax/microedition/media/Player", "addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_addPlayerListener},
        {"javax/microedition/media/Player", "removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_removePlayerListener},
        
        /* PlayerImpl */
        {"javax/microedition/media/PlayerImpl", "realize", "()V", native_player_realize},
        {"javax/microedition/media/PlayerImpl", "prefetch", "()V", native_player_prefetch},
        {"javax/microedition/media/PlayerImpl", "start", "()V", native_player_start},
        {"javax/microedition/media/PlayerImpl", "stop", "()V", native_player_stop},
        {"javax/microedition/media/PlayerImpl", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/PlayerImpl", "close", "()V", native_player_close},
        {"javax/microedition/media/PlayerImpl", "setLoopCount", "(I)V", native_player_setLoopCount},
        {"javax/microedition/media/PlayerImpl", "getDuration", "()J", native_player_getDuration},
        {"javax/microedition/media/PlayerImpl", "getMediaTime", "()J", native_player_getMediaTime},
        {"javax/microedition/media/PlayerImpl", "setMediaTime", "(J)J", native_player_setMediaTime},
        {"javax/microedition/media/PlayerImpl", "getState", "()I", native_player_getState},
        {"javax/microedition/media/PlayerImpl", "getContentType", "()Ljava/lang/String;", native_player_getContentType},
        {"javax/microedition/media/PlayerImpl", "getLocator", "()Ljava/lang/String;", native_player_getLocator},
        {"javax/microedition/media/PlayerImpl", "getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;", native_player_getControl},
        {"javax/microedition/media/PlayerImpl", "getControls", "()[Ljavax/microedition/media/Control;", native_player_getControls},
        {"javax/microedition/media/PlayerImpl", "addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_addPlayerListener},
        {"javax/microedition/media/PlayerImpl", "removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_removePlayerListener},
        
        /* VolumeControl */
        {"javax/microedition/media/control/VolumeControl", "setLevel", "(I)V", native_volumecontrol_setLevel},
        /* v34.53: vendor (I)I variant — see native_volumecontrol_setLevel_ret */
        {"javax/microedition/media/control/VolumeControl", "setLevel", "(I)I", native_volumecontrol_setLevel_ret},
        {"javax/microedition/media/control/VolumeControl", "getLevel", "()I", native_volumecontrol_getLevel},
        {"javax/microedition/media/control/VolumeControl", "setMute", "(Z)V", native_volumecontrol_setMute},
        {"javax/microedition/media/control/VolumeControl", "isMuted", "()Z", native_volumecontrol_isMuted},
        {"javax/microedition/media/control/VolumeControlImpl", "setLevel", "(I)V", native_volumecontrol_setLevel},
        {"javax/microedition/media/control/VolumeControlImpl", "setLevel", "(I)I", native_volumecontrol_setLevel_ret},
        {"javax/microedition/media/control/VolumeControlImpl", "getLevel", "()I", native_volumecontrol_getLevel},
        {"javax/microedition/media/control/VolumeControlImpl", "setMute", "(Z)V", native_volumecontrol_setMute},
        {"javax/microedition/media/control/VolumeControlImpl", "isMuted", "()Z", native_volumecontrol_isMuted},
        
        /* MIDIControl */
        {"javax/microedition/media/control/MIDIControl", "shortMidiEvent", "(III)V", native_midicontrol_shortMidiEvent},
        {"javax/microedition/media/control/MIDIControl", "longMidiEvent", "([BII)I", native_midicontrol_longMidiEvent},
        {"javax/microedition/media/control/MIDIControl", "setChannelVolume", "(II)V", native_midicontrol_setChannelVolume},
        {"javax/microedition/media/control/MIDIControl", "getChannelVolume", "(I)I", native_midicontrol_getChannelVolume},
        {"javax/microedition/media/control/MIDIControl", "setProgram", "(III)V", native_midicontrol_setProgram},
        {"javax/microedition/media/control/MIDIControl", "getProgram", "(I)I", native_midicontrol_getProgram},
        {"javax/microedition/media/control/MIDIControl", "isBankQuerySupported", "()Z", native_midicontrol_isBankQuerySupported},
        {"javax/microedition/media/control/MIDIControlImpl", "shortMidiEvent", "(III)V", native_midicontrol_shortMidiEvent},
        {"javax/microedition/media/control/MIDIControlImpl", "longMidiEvent", "([BII)I", native_midicontrol_longMidiEvent},
        {"javax/microedition/media/control/MIDIControlImpl", "setChannelVolume", "(II)V", native_midicontrol_setChannelVolume},
        {"javax/microedition/media/control/MIDIControlImpl", "getChannelVolume", "(I)I", native_midicontrol_getChannelVolume},
        {"javax/microedition/media/control/MIDIControlImpl", "setProgram", "(III)V", native_midicontrol_setProgram},
        {"javax/microedition/media/control/MIDIControlImpl", "getProgram", "(I)I", native_midicontrol_getProgram},
        {"javax/microedition/media/control/MIDIControlImpl", "isBankQuerySupported", "()Z", native_midicontrol_isBankQuerySupported},
        
        /* ToneControl */
        {"javax/microedition/media/control/ToneControl", "setSequence", "([B)V", native_tonecontrol_setSequence},
        {"javax/microedition/media/control/ToneControlImpl", "setSequence", "([B)V", native_tonecontrol_setSequence},
        
        /* VideoControl */
        {"javax/microedition/media/control/VideoControl", "initDisplayMode", "(ILjava/lang/Object;)Ljava/lang/Object;", native_videocontrol_initDisplayMode},
        {"javax/microedition/media/control/VideoControl", "setDisplayLocation", "(II)V", native_videocontrol_setDisplayLocation},
        {"javax/microedition/media/control/VideoControl", "getDisplayX", "()I", native_videocontrol_getDisplayX},
        {"javax/microedition/media/control/VideoControl", "getDisplayY", "()I", native_videocontrol_getDisplayY},
        {"javax/microedition/media/control/VideoControl", "setVisible", "(Z)V", native_videocontrol_setVisible},
        {"javax/microedition/media/control/VideoControl", "getDisplayWidth", "()I", native_videocontrol_getDisplayWidth},
        {"javax/microedition/media/control/VideoControl", "getDisplayHeight", "()I", native_videocontrol_getDisplayHeight},
        {"javax/microedition/media/control/VideoControl", "getSnapshot", "(Ljava/lang/String;)[B", native_videocontrol_getSnapshot},
        {"javax/microedition/media/control/VideoControl", "setDisplaySize", "(II)V", native_videocontrol_setDisplaySize},
        {"javax/microedition/media/control/VideoControl", "setDisplayFullScreen", "(Z)V", native_videocontrol_setDisplayFullScreen},
        {"javax/microedition/media/control/VideoControl", "getSourceWidth", "()I", native_videocontrol_getSourceWidth},
        {"javax/microedition/media/control/VideoControl", "getSourceHeight", "()I", native_videocontrol_getSourceHeight},
        {"javax/microedition/media/control/VideoControlImpl", "initDisplayMode", "(ILjava/lang/Object;)Ljava/lang/Object;", native_videocontrol_initDisplayMode},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplayLocation", "(II)V", native_videocontrol_setDisplayLocation},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayX", "()I", native_videocontrol_getDisplayX},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayY", "()I", native_videocontrol_getDisplayY},
        {"javax/microedition/media/control/VideoControlImpl", "setVisible", "(Z)V", native_videocontrol_setVisible},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayWidth", "()I", native_videocontrol_getDisplayWidth},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayHeight", "()I", native_videocontrol_getDisplayHeight},
        {"javax/microedition/media/control/VideoControlImpl", "getSnapshot", "(Ljava/lang/String;)[B", native_videocontrol_getSnapshot},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplaySize", "(II)V", native_videocontrol_setDisplaySize},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplayFullScreen", "(Z)V", native_videocontrol_setDisplayFullScreen},
        {"javax/microedition/media/control/VideoControlImpl", "getSourceWidth", "()I", native_videocontrol_getSourceWidth},
        {"javax/microedition/media/control/VideoControlImpl", "getSourceHeight", "()I", native_videocontrol_getSourceHeight},
        
        /* TimeBase */
        {"javax/microedition/media/TimeBase", "getTime", "()J", native_timebase_getTime},
    };
    
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    MEDIA_DEBUG("Registered %zu media native methods", sizeof(methods) / sizeof(methods[0]));
}

/* External function called from SDL audio callback */
void media_generate_wav_samples(int16_t* buffer, int samples) {
    /* This is now handled by our audio thread */
    (void)buffer;
    (void)samples;
}

/* =========================================================================
 * Nokia Sound backend (com.nokia.mid.sound) - v17
 *
 * Real audio playback for the Nokia vendor Sound API:
 *  - FORMAT_WAV (5): full WAV parse/reuse of the JSR-135 wav parser
 *  - FORMAT_TONE (1): RTTTL text melodies ("d=4,o=5,b=112:e6,8d6,...")
 *    decoded to a note list and synthesized as a soft square wave
 *  - gain 0..100 scaling, loop counts, pause/resume, duration query
 * Binary OTA tones are rare and undocumented; they are rejected cleanly
 * (state stays STOPPED) instead of silently pretending to play.
 * Integrated into BOTH the standalone audio thread and the libretro
 * synchronous sample generator.
 * ========================================================================= */

#define NOKIA_SOUND_SLOTS      8
#define NOKIA_SOUND_FORMAT_TONE 1
#define NOKIA_SOUND_FORMAT_WAV   5
#define NOKIA_TONE_MAX_EVENTS 256

typedef struct {
    int midi_note;   /* 0..127, -1 = silence */
    int dur_ms;      /* duration in milliseconds */
} NokiaToneEvent;

typedef struct {
    int in_use;
    int sound_id;
    int state;            /* 0=SOUND_PLAYING, 1=SOUND_STOPPED, 3=SOUND_UNINITIALIZED */
    int format;           /* FORMAT_TONE / FORMAT_WAV */
    int gain;             /* 0..100 (Nokia scale) */
    bool paused;
    bool finished;        /* completed naturally (for listener events) */
    int loop_count;       /* -1 = infinite, 0 = once, n = n extra repeats */
    int loops_remaining;

    /* WAV data */
    WavFile* wav;
    uint32_t wav_samples_played;
    double wav_read_pos;   /* v23: fractional source-frame read position */

    /* Tone data */
    NokiaToneEvent tone_events[NOKIA_TONE_MAX_EVENTS];
    int tone_event_count;
    int tone_pos;
    uint64_t tone_note_start_us;
    uint64_t tone_note_len_us;
    float tone_phase;
} NokiaSoundChannel;

static NokiaSoundChannel g_nokia_sounds[NOKIA_SOUND_SLOTS];
static int g_nokia_sound_next_id = 1;
static bool g_nokia_sound_inited = false;

/* --- RTTTL parsing ------------------------------------------------------ */

static int rtttl_note_semitone(char c) {
    switch (c) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        default: return -1;
    }
}

/* Parse an RTTTL melody into tone events. Returns event count (0 = failed). */
static int rtttl_parse(const char* text, NokiaToneEvent* events, int max_events,
                       int* out_duration_ms) {
    if (!text || !events) return 0;

    /* Defaults per spec */
    int def_duration = 4;
    int def_octave = 5;
    int bpm = 112;

    /* Section 1: defaults "dd=...,oo=...,bb=..." */
    const char* p = text;
    /* skip name up to ':' */
    const char* colon = strchr(p, ':');
    if (!colon) return 0;
    p = colon + 1;

    /* parse defaults until second ':' */
    const char* colon2 = strchr(p, ':');
    if (!colon2) return 0;
    {
        char section[256];
        size_t n = (size_t)(colon2 - p);
        if (n >= sizeof(section)) n = sizeof(section) - 1;
        memcpy(section, p, n);
        section[n] = '\0';
        char* tok = section;
        while (*tok) {
            while (*tok == ' ' || *tok == ',') tok++;
            if (!*tok) break;
            char key = *tok++;
            /* skip '=' */
            while (*tok == '=') tok++;
            int val = atoi(tok);
            while (*tok && *tok != ',') tok++;
            switch (key) {
                case 'd': if (val > 0) def_duration = val; break;
                case 'o': if (val > 0) def_octave = val; break;
                case 'b': if (val > 0) bpm = val; break;
                default: break;
            }
        }
    }
    p = colon2 + 1;

    double beat_ms = 60000.0 / (bpm > 0 ? bpm : 112) * 4.0; /* whole note */

    int count = 0;
    while (*p && count < max_events) {
        /* skip separators */
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;

        /* optional duration */
        int dur = def_duration;
        {
            const char* q = p;
            while (*q >= '0' && *q <= '9') q++;
            if (q > p) {
                dur = atoi(p);
                p = q;
                if (dur <= 0) dur = def_duration;
            }
        }

        /* note letter or pause */
        int midi = -1;
        char letter = *p;
        if (letter == 'p' || letter == 'P' || letter == '-' || letter == 'r' || letter == 'R') {
            p++;
            midi = -1;
        } else {
            int semi = rtttl_note_semitone((char)tolower((unsigned char)letter));
            if (semi < 0) break; /* malformed - stop here */
            p++;
            /* sharp */
            if (*p == '#') { semi += 1; p++; }
            /* octave */
            int octave = def_octave;
            if (*p >= '1' && *p <= '7') { octave = *p - '0'; p++; }
            /* dotted */
            int dotted = 0;
            if (*p == '.') { dotted = 1; p++; }
            midi = (octave + 1) * 12 + semi; /* MIDI note (C4=60 with octave=4) */
            if (midi < 0) midi = 0;
            if (midi > 127) midi = 127;

            if (dotted) dur = dur * 3 / 2;
        }

        double ms = beat_ms / (double)dur;
        events[count].midi_note = midi;
        events[count].dur_ms = (int)(ms + 0.5);
        if (events[count].dur_ms < 10) events[count].dur_ms = 10;
        count++;

        /* advance to separator */
        while (*p && *p != ',') p++;
    }

    if (out_duration_ms) {
        int total = 0;
        for (int i = 0; i < count; i++) total += events[i].dur_ms;
        *out_duration_ms = total;
    }
    return count;
}

/* --- channel management -------------------------------------------------- */

static void nokia_sound_ensure_init(void) {
    if (!g_nokia_sound_inited) {
        memset(g_nokia_sounds, 0, sizeof(g_nokia_sounds));
        ensure_audio_init();
        g_nokia_sound_inited = true;
    }
}

static NokiaSoundChannel* nokia_sound_find(int id) {
    for (int i = 0; i < NOKIA_SOUND_SLOTS; i++) {
        if (g_nokia_sounds[i].in_use && g_nokia_sounds[i].sound_id == id) {
            return &g_nokia_sounds[i];
        }
    }
    return NULL;
}

/* Parse and store sound data for later play(). Returns channel id or -1. */
int nokia_sound_prepare(int id, const uint8_t* data, int len, int format) {
    nokia_sound_ensure_init();

    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) {
        /* Allocate new slot */
        for (int i = 0; i < NOKIA_SOUND_SLOTS; i++) {
            if (!g_nokia_sounds[i].in_use) {
                ch = &g_nokia_sounds[i];
                memset(ch, 0, sizeof(*ch));
                ch->in_use = 1;
                ch->sound_id = id ? id : g_nokia_sound_next_id++;
                break;
            }
        }
        if (!ch) return -1; /* no free slots */
    }

    /* Reset previous content */
    if (ch->wav) { wav_free(ch->wav); ch->wav = NULL; }
    ch->tone_event_count = 0;
    ch->state = 1; /* STOPPED */
    ch->paused = false;
    ch->finished = false;
    ch->format = format;

    if (!data || len <= 0) return ch->sound_id;

    if (format == NOKIA_SOUND_FORMAT_WAV) {
        ch->wav = wav_parse(data, (uint32_t)len);
        if (!ch->wav) {
            MEDIA_DEBUG("NokiaSound: WAV parse failed (%d bytes)", len);
        }
    } else if (format == NOKIA_SOUND_FORMAT_TONE) {
        /* RTTTL text starts with a section like "name:d=..." */
        const char* text = (const char*)data;
        /* Ensure NUL-safety: scan at most len chars */
        char buf[1024];
        int copy = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
        memcpy(buf, text, copy);
        buf[copy] = '\0';

        if (strchr(buf, ':') && (buf[0] == 'd' || buf[0] == 'D' || isalpha((unsigned char)buf[0]))) {
            ch->tone_event_count = rtttl_parse(buf, ch->tone_events,
                                               NOKIA_TONE_MAX_EVENTS, NULL);
            if (ch->tone_event_count > 0) {
                MEDIA_DEBUG("NokiaSound: RTTTL parsed, %d notes", ch->tone_event_count);
            }
        }
        if (ch->tone_event_count == 0) {
            MEDIA_DEBUG("NokiaSound: binary/unsupported tone format (%d bytes), will stay silent", len);
        }
    }

    return ch->sound_id;
}

int nokia_sound_play(int id, int loop_count) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return -1;

    if (!ch->wav && ch->tone_event_count == 0) {
        ch->state = 1; /* nothing playable */
        return -1;
    }

    ch->loop_count = loop_count;
    ch->loops_remaining = loop_count;
    ch->paused = false;
    ch->finished = false;
    ch->state = 0; /* PLAYING */

    if (ch->wav) {
        ch->wav_samples_played = 0;
        ch->wav_read_pos = 0.0;
    } else {
        ch->tone_pos = 0;
        ch->tone_note_start_us = get_time_us();
        ch->tone_phase = 0.0f;
        /* Preload first event length */
        ch->tone_note_len_us = (uint64_t)ch->tone_events[0].dur_ms * 1000ULL;
    }

    MEDIA_DEBUG("NokiaSound play id=%d fmt=%d loops=%d", id, ch->format, loop_count);
    return 0;
}

void nokia_sound_stop(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return;
    ch->state = 1; /* STOPPED */
    ch->paused = false;
}

void nokia_sound_resume(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return;
    if (ch->paused) {
        ch->paused = false;
        ch->state = 0; /* PLAYING */
        /* Restart current note timing from now */
        ch->tone_note_start_us = get_time_us();
    }
}

void nokia_sound_pause(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return;
    if (ch->state == 0) {
        ch->paused = true;
        ch->state = 1; /* STOPPED while paused */
    }
}

void nokia_sound_release(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return;
    if (ch->wav) { wav_free(ch->wav); ch->wav = NULL; }
    ch->tone_event_count = 0;
    ch->in_use = 0;
    ch->state = 3; /* UNINITIALIZED */
}

int nokia_sound_get_state(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return 3; /* UNINITIALIZED */
    return ch->state;
}

void nokia_sound_set_gain(int id, int gain) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return;
    if (gain < 0) gain = 0;
    if (gain > 100) gain = 100;
    ch->gain = gain;
}

int nokia_sound_get_gain(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return -1;
    return ch->gain;
}

int nokia_sound_get_duration_ms(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return -1;
    if (ch->wav) {
        double secs = 0.0;
        if (ch->wav->sample_rate > 0) {
            uint32_t frames = ch->wav->data_size /
                (ch->wav->num_channels * (ch->wav->bits_per_sample / 8));
            secs = (double)frames / (double)ch->wav->sample_rate;
        }
        return (int)(secs * 1000.0);
    }
    int total = 0;
    for (int i = 0; i < ch->tone_event_count; i++) total += ch->tone_events[i].dur_ms;
    return ch->tone_event_count > 0 ? total : -1;
}

/* Did playback finish naturally since the last check? (for listener events) */
bool nokia_sound_poll_finished(int id) {
    NokiaSoundChannel* ch = nokia_sound_find(id);
    if (!ch) return false;
    bool f = ch->finished;
    ch->finished = false;
    return f;
}

/* --- mixing -------------------------------------------------------------- */

static void nokia_sound_mix_one(NokiaSoundChannel* ch, int16_t* buffer, int samples) {
    if (!ch || ch->state != 0 || ch->paused) return;
    if (WILDGUARD_SKIP(ch->wav, "mix:nokia")) return; /* [WILDGUARD] v36.50 */

    float vol = ch->gain / 100.0f;

    if (ch->wav) {
        WavFile* wav = ch->wav;
        int src_bytes_per_sample = wav->num_channels * (wav->bits_per_sample / 8);
        uint32_t total_frames = src_bytes_per_sample > 0
            ? wav->data_size / (uint32_t)src_bytes_per_sample : 0;
        double src_ratio = (double)wav->sample_rate / AUDIO_SAMPLE_RATE;

        for (int i = 0; i < samples; i++) {
            /* v23: fractional position + linear interpolation (shared with
             * the JSR-135 mixer) instead of nearest-neighbor */
            if (ch->wav_read_pos >= (double)total_frames) {
                if (ch->loop_count != 0) {
                    ch->wav_read_pos = 0.0;
                    ch->wav_samples_played = 0;
                    if (ch->loops_remaining > 0 && --ch->loops_remaining == 0) {
                        ch->state = 1;
                        ch->finished = true;
                        break;
                    }
                } else {
                    ch->state = 1;
                    ch->finished = true;
                    break;
                }
            }

            int16_t left = 0, right = 0;
            wav_read_frame_lerp(wav, ch->wav_read_pos, &left, &right);

            buffer[i * 2]     = sat16((int32_t)buffer[i * 2]     + (int32_t)(left * vol));
            buffer[i * 2 + 1] = sat16((int32_t)buffer[i * 2 + 1] + (int32_t)(right * vol));
            ch->wav_read_pos += src_ratio;
            ch->wav_samples_played++;
        }
    } else if (ch->tone_event_count > 0) {
        uint64_t now = get_time_us();
        /* v34.27: sub-microsecond advance. The old 1000000/44100 = 22us
         * integer step ran events ~2.2% fast; 22675ns steps are exact to
         * 0.003%. */
        uint64_t now_ns_rem = 0;
        const uint64_t step_ns = 1000000000ULL / AUDIO_SAMPLE_RATE;

        for (int i = 0; i < samples; i++) {
            if (ch->state != 0) break;

            /* Advance to next event when the current one expires */
            if (now >= ch->tone_note_start_us + ch->tone_note_len_us) {
                ch->tone_pos++;
                if (ch->tone_pos >= ch->tone_event_count) {
                    if (ch->loop_count != 0) {
                        ch->tone_pos = 0;
                        if (ch->loops_remaining > 0 && --ch->loops_remaining == 0) {
                            ch->state = 1;
                            ch->finished = true;
                            break;
                        }
                    } else {
                        ch->state = 1;
                        ch->finished = true;
                        break;
                    }
                }
                ch->tone_note_start_us += ch->tone_note_len_us;
                if (ch->tone_note_start_us > now) ch->tone_note_start_us = now;
                ch->tone_note_len_us = (uint64_t)ch->tone_events[ch->tone_pos].dur_ms * 1000ULL;
            }

            int note = ch->tone_events[ch->tone_pos].midi_note;
            if (note >= 0) {
                double freq = 440.0 * pow(2.0, (note - 69) / 12.0);
                /* Soft square wave (two harmonics) - less shrill than pure square */
                ch->tone_phase += (float)(2.0 * M_PI * freq / AUDIO_SAMPLE_RATE);
                if (ch->tone_phase > 2.0f * (float)M_PI) ch->tone_phase -= 2.0f * (float)M_PI;
                float ph = ch->tone_phase;
                float sq = (ph < (float)M_PI) ? 1.0f : -1.0f;
                float sn = sinf(ph);
                float sample = (0.6f * sq + 0.4f * sn) * 0.25f * vol;
                int16_t pcm = (int16_t)(sample * 32000.0f);
                buffer[i * 2]     = sat16((int32_t)buffer[i * 2]     + pcm);
                buffer[i * 2 + 1] = sat16((int32_t)buffer[i * 2 + 1] + pcm);
            }
            now_ns_rem += step_ns;
            if (now_ns_rem >= 1000) {
                now += now_ns_rem / 1000;
                now_ns_rem %= 1000;
            }
        }
    }
}

/* Mix all Nokia sound channels; called under g_audio_mutex from the mixers */
void nokia_sound_mix_all(int16_t* buffer, int samples) {
    for (int i = 0; i < NOKIA_SOUND_SLOTS; i++) {
        if (g_nokia_sounds[i].in_use && g_nokia_sounds[i].state == 0) {
            nokia_sound_mix_one(&g_nokia_sounds[i], buffer, samples);
        }
    }
}
