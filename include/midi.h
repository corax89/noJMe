/*
 * J2ME Emulator - MIDI Synthesizer
 * Simple FM synthesis-based MIDI player for J2ME games
 */

#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI note frequencies (A4 = 440Hz) */
#define MIDI_NOTE_COUNT 128

/* MIDI channel count */
#define MIDI_CHANNELS 16

/* MIDI controller numbers */
#define MIDI_CTRL_BANK_SELECT      0
#define MIDI_CTRL_MODULATION       1
#define MIDI_CTRL_VOLUME           7
#define MIDI_CTRL_PAN              10
#define MIDI_CTRL_EXPRESSION       11
#define MIDI_CTRL_SUSTAIN          64
#define MIDI_CTRL_REVERB           91
#define MIDI_CTRL_CHORUS           93

/* MIDI event types */
#define MIDI_EVENT_NOTE_OFF        0x80
#define MIDI_EVENT_NOTE_ON         0x90
#define MIDI_EVENT_KEY_PRESSURE    0xA0
#define MIDI_EVENT_CONTROL_CHANGE  0xB0
#define MIDI_EVENT_PROGRAM_CHANGE  0xC0
#define MIDI_EVENT_CHANNEL_PRESSURE 0xD0
#define MIDI_EVENT_PITCH_BEND      0xE0
#define MIDI_EVENT_SYSTEM          0xF0

/* MIDI file parser state */
typedef enum {
    MIDI_PARSE_HEADER,
    MIDI_PARSE_TRACK,
    MIDI_PARSE_EVENT,
    MIDI_PARSE_SYSEX,
    MIDI_PARSE_META
} MidiParseState;

/* MIDI track information */
typedef struct MidiTrack {
    uint8_t* data;
    uint32_t length;
    uint32_t position;
    uint32_t delta_time;
    uint8_t running_status;
    bool ended;
    uint64_t end_time_us; /* v36.27: true song time at end of track (tempo-aware scan) */
} MidiTrack;

/* MIDI file header */
typedef struct MidiHeader {
    uint16_t format;
    uint16_t track_count;
    uint16_t division;
    uint8_t division_type;  /* 0 = ticks per beat, 1 = ticks per second */
} MidiHeader;

/* Active note */
typedef struct MidiNote {
    bool active;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    float frequency;
    float phase;
    float envelope;
    float envelope_target;
    /* FM synthesis parameters */
    float mod_phase;
    float mod_index;
    float mod_ratio;      /* v23: per-note (from the patch / drum map) */
    /* v23: drum/percussion synthesis (channel 9) + sustain support */
    bool drum;            /* synthesized as GM percussion, not melodic FM */
    bool key_held;        /* false while held only by the sustain pedal */
    float decay_k;        /* per-sample exponential decay coefficient (drums) */
    float sweep_k;        /* per-sample pitch-sweep coefficient (0 = no sweep) */
    float freq_target;    /* sweep target frequency */
    float noise_mix;      /* 0..1 noise blend for percussion */
    uint8_t choke_group;  /* 1 = open hat (choked by closed/pedal hat) */
    uint32_t rng;         /* xorshift state for noise */
} MidiNote;

/* MIDI channel state */
typedef struct MidiChannel {
    uint8_t program;
    uint8_t volume;
    uint8_t pan;
    uint8_t expression;
    uint8_t sustain;
    float pitch_bend;
    uint8_t bank;
    uint8_t modulation;
    /* v23: send effect levels (CC91/CC93) */
    uint8_t reverb;
    uint8_t chorus;
} MidiChannel;

/* MIDI sequencer state */
typedef struct MidiSequencer {
    MidiHeader header;
    MidiTrack* tracks;
    MidiChannel channels[MIDI_CHANNELS];
    MidiNote* active_notes;
    int max_active_notes;
    int active_note_count;

    uint32_t ticks_per_beat;
    uint32_t microseconds_per_beat;
    float current_tempo;
    uint64_t total_time_us; /* v36.27: max track end time across tracks (duration) */

    /* v23: precise tick timing. SMPTE files carry ticks-per-second, not
     * ticks-per-beat, so all tick<->time conversions go through us_per_tick. */
    bool smpte;                  /* header division was SMPTE */
    double us_per_tick;          /* microseconds per sequencer tick */
    bool end_reached;            /* played through once (non-loop) - for END_OF_MEDIA */

    uint32_t position;
    bool playing;
    bool loop;
    float volume;

    /* v36.28 DIAG: lifetime counters for the sandbox MIDI silence hunt */
    uint32_t diag_events;        /* process_event calls */
    uint32_t diag_notes;         /* start_note calls that kept the voice */
    uint32_t diag_rejects;       /* start_note calls dropped before voice start */

    /* v23: send-effect state (reverb: 4 feedback combs + 2 allpass;
     * chorus: modulated delay). Mono input, applied to both channels. */
    float rv_comb[4][2048];
    int   rv_comb_len[4];
    int   rv_comb_pos[4];
    float rv_ap[2][512];
    int   rv_ap_len[2];
    int   rv_ap_pos[2];
    float cho_buf[4096];
    int   cho_len;
    int   cho_pos;
    float cho_lfo;

    /* Playback timing */
    uint32_t sample_rate;
    double samples_per_tick;
    double sample_accumulator;

    /* Time-based synchronization */
    uint64_t last_real_time_us;    /* Last real time in microseconds */
    double tick_accumulator;       /* Accumulated ticks from real time */
} MidiSequencer;

/* MIDI file structure */
typedef struct MidiFile {
    uint8_t* data;
    uint32_t size;
    MidiSequencer* sequencer;
} MidiFile;

/* Initialize MIDI subsystem */
int midi_init(uint32_t sample_rate);

/* Shutdown MIDI subsystem */
void midi_shutdown(void);

/* Load MIDI file from memory */
MidiFile* midi_load(const uint8_t* data, uint32_t size);

/* Free MIDI file */
void midi_free(MidiFile* midi);

/* Start playback */
void midi_play(MidiFile* midi);

/* Stop playback */
void midi_stop(MidiFile* midi);

/* Pause playback */
void midi_pause(MidiFile* midi);

/* Resume playback */
void midi_resume(MidiFile* midi);

/* Set loop mode */
void midi_set_loop(MidiFile* midi, bool loop);

/* Set volume (0.0 - 1.0) */
void midi_set_volume(MidiFile* midi, float volume);

/* Get current position in seconds */
float midi_get_position(MidiFile* midi);

/* Get total length in seconds */
float midi_get_length(MidiFile* midi);

/* Generate audio samples */
void midi_generate_samples(MidiFile* midi, int16_t* buffer, int samples);

/* Advance sequencer by real time (microseconds) - for time-based sync */
void midi_advance_time(MidiFile* midi, uint64_t elapsed_us);

/* Direct MIDI commands (for programmatic control) */
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_note_off(uint8_t channel, uint8_t note);
void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value);
void midi_program_change(uint8_t channel, uint8_t program);
void midi_pitch_bend(uint8_t channel, int16_t value);

/* v23: per-file variants - route MIDIControl events to a specific player's
 * synthesizer instead of the global active sequencer. */
void midi_note_on_file(MidiFile* midi, uint8_t channel, uint8_t note, uint8_t velocity);
void midi_note_off_file(MidiFile* midi, uint8_t channel, uint8_t note);
void midi_control_change_file(MidiFile* midi, uint8_t channel, uint8_t controller, uint8_t value);
void midi_program_change_file(MidiFile* midi, uint8_t channel, uint8_t program);
int  midi_get_channel_volume_file(MidiFile* midi, int channel);
int  midi_get_program_file(MidiFile* midi, int channel);

/* v23: minimal valid MIDI file (header + empty track) used as the synthesizer
 * target for device://tone and device://midi players. */
MidiFile* midi_load_empty(void);

/* Get active sequencer (for audio callback) */
MidiSequencer* midi_get_active_sequencer(void);

/* Process sequencer events */
void midi_process_tick(MidiSequencer* seq);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_H */

/* Additional functions for media.c */
uint32_t midi_get_duration_ms(MidiFile* midi);
uint32_t midi_get_position_ms(MidiFile* midi);
void midi_set_position_ms(MidiFile* midi, uint32_t position_ms);
int midi_get_channel_volume(int channel);
int midi_get_program(int channel);
