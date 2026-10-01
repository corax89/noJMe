/*
 * J2ME Emulator - MIDI Synthesizer Implementation
 * FM synthesis-based MIDI player
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "midi.h"
#include "debug_macros.h" /* v36.28: MEDIA_DEBUG midi_load fingerprint */

/* Math constants */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Maximum polyphony */
#define MAX_POLYPHONY 32

/* Default tempo (120 BPM) */
#define DEFAULT_TEMPO 500000  /* microseconds per beat */
#define DEFAULT_TICKS_PER_BEAT 480

/* Global state */
static uint32_t g_sample_rate = 44100;
static MidiSequencer* g_active_sequencer = NULL;

/* Note frequency table (calculated at init) */
static float g_note_frequencies[MIDI_NOTE_COUNT];

/* FM synthesis parameters for different instruments */
typedef struct {
    float attack;
    float decay;
    float sustain;
    float release;
    float mod_index;
    float mod_ratio;
} FMPatch;

/* Simple FM patches for General MIDI instruments */
static const FMPatch g_patches[] = {
    /* 0: Piano */
    {0.01f, 0.1f, 0.7f, 0.3f, 1.0f, 1.0f},
    /* 1: Bright Piano */
    {0.005f, 0.15f, 0.6f, 0.25f, 1.5f, 2.0f},
    /* 2: Electric Grand */
    {0.005f, 0.2f, 0.5f, 0.2f, 2.0f, 1.0f},
    /* 3: Honky Tonk */
    {0.01f, 0.1f, 0.6f, 0.3f, 0.5f, 0.5f},
    /* 4: Electric Piano 1 */
    {0.001f, 0.3f, 0.4f, 0.3f, 3.0f, 0.5f},
    /* 5: Electric Piano 2 */
    {0.001f, 0.25f, 0.5f, 0.25f, 2.5f, 0.25f},
    /* 6: Harpsichord */
    {0.001f, 0.1f, 0.8f, 0.1f, 0.3f, 2.0f},
    /* 7: Clavinet */
    {0.001f, 0.15f, 0.7f, 0.15f, 0.5f, 3.0f},
    /* 8: Celesta */
    {0.001f, 0.3f, 0.3f, 0.5f, 2.0f, 3.0f},
    /* 9: Glockenspiel */
    {0.001f, 0.4f, 0.2f, 0.6f, 1.0f, 4.0f},
    /* 10: Music Box */
    {0.001f, 0.2f, 0.4f, 0.4f, 1.5f, 3.0f},
    /* 11: Vibraphone */
    {0.001f, 0.3f, 0.5f, 0.4f, 2.0f, 2.0f},
    /* 12: Marimba */
    {0.001f, 0.2f, 0.6f, 0.3f, 0.5f, 1.0f},
    /* 13: Xylophone */
    {0.001f, 0.15f, 0.5f, 0.4f, 0.3f, 2.0f},
    /* 14: Tubular Bells */
    {0.001f, 0.5f, 0.3f, 0.7f, 3.0f, 1.5f},
    /* 15: Dulcimer */
    {0.001f, 0.2f, 0.6f, 0.3f, 1.0f, 1.0f},
    /* 16: Drawbar Organ */
    {0.01f, 0.05f, 0.9f, 0.1f, 0.5f, 1.0f},
    /* 17: Percussive Organ */
    {0.01f, 0.1f, 0.8f, 0.1f, 0.7f, 2.0f},
    /* 18: Rock Organ */
    {0.01f, 0.05f, 0.85f, 0.1f, 1.0f, 0.5f},
    /* 19: Church Organ */
    {0.02f, 0.1f, 0.8f, 0.2f, 0.3f, 0.5f},
    /* 20: Reed Organ */
    {0.02f, 0.15f, 0.7f, 0.15f, 0.4f, 1.0f},
    /* 21: Accordion */
    {0.01f, 0.1f, 0.8f, 0.1f, 0.6f, 0.5f},
    /* 22: Harmonica */
    {0.01f, 0.1f, 0.75f, 0.15f, 0.8f, 1.0f},
    /* 23: Tango Accordion */
    {0.01f, 0.1f, 0.8f, 0.1f, 1.0f, 0.5f},
    /* 24: Nylon Guitar */
    {0.002f, 0.15f, 0.5f, 0.3f, 0.8f, 1.0f},
    /* 25: Steel Guitar */
    {0.002f, 0.1f, 0.6f, 0.25f, 1.2f, 0.5f},
    /* 26: Jazz Guitar */
    {0.002f, 0.15f, 0.55f, 0.2f, 1.5f, 1.0f},
    /* 27: Clean Guitar */
    {0.002f, 0.1f, 0.65f, 0.2f, 0.5f, 0.5f},
    /* 28: Muted Guitar */
    {0.001f, 0.1f, 0.3f, 0.2f, 1.0f, 2.0f},
    /* 29: Overdrive Guitar */
    {0.001f, 0.1f, 0.7f, 0.15f, 2.0f, 1.0f},
    /* 30: Distortion Guitar */
    {0.001f, 0.1f, 0.7f, 0.15f, 3.0f, 0.5f},
    /* 31: Guitar Harmonics */
    {0.001f, 0.2f, 0.5f, 0.3f, 2.0f, 2.0f},
    /* 32: Acoustic Bass */
    {0.002f, 0.2f, 0.4f, 0.25f, 0.5f, 0.5f},
    /* 33: Electric Bass (finger) */
    {0.002f, 0.15f, 0.5f, 0.2f, 1.0f, 0.5f},
    /* 34: Electric Bass (pick) */
    {0.001f, 0.1f, 0.5f, 0.15f, 1.5f, 0.5f},
    /* 35: Fretless Bass */
    {0.005f, 0.3f, 0.4f, 0.2f, 0.3f, 1.0f},
    /* 36: Slap Bass 1 */
    {0.001f, 0.05f, 0.6f, 0.1f, 2.0f, 1.0f},
    /* 37: Slap Bass 2 */
    {0.001f, 0.08f, 0.55f, 0.12f, 2.5f, 1.0f},
    /* 38: Synth Bass 1 */
    {0.002f, 0.1f, 0.6f, 0.15f, 3.0f, 0.5f},
    /* 39: Synth Bass 2 */
    {0.002f, 0.15f, 0.5f, 0.2f, 2.5f, 1.0f},
    /* 40: Violin */
    {0.05f, 0.2f, 0.7f, 0.35f, 0.5f, 1.0f},
    /* 41: Viola */
    {0.05f, 0.2f, 0.7f, 0.35f, 0.4f, 1.0f},
    /* 42: Cello */
    {0.05f, 0.22f, 0.7f, 0.35f, 0.35f, 1.0f},
    /* 43: Contrabass */
    {0.05f, 0.25f, 0.65f, 0.3f, 0.3f, 0.5f},
    /* 44: Tremolo Strings */
    {0.05f, 0.15f, 0.7f, 0.25f, 1.5f, 2.0f},
    /* 45: Pizzicato Strings */
    {0.002f, 0.12f, 0.4f, 0.25f, 0.6f, 1.0f},
    /* 46: Orchestral Harp */
    {0.002f, 0.15f, 0.45f, 0.3f, 0.8f, 1.5f},
    /* 47: Timpani */
    {0.002f, 0.3f, 0.35f, 0.4f, 1.2f, 0.5f},
    /* 48: String Ensemble 1 */
    {0.06f, 0.2f, 0.75f, 0.35f, 0.4f, 1.0f},
    /* 49: String Ensemble 2 */
    {0.07f, 0.2f, 0.75f, 0.35f, 0.6f, 1.5f},
    /* 50: Synth Strings 1 */
    {0.08f, 0.25f, 0.7f, 0.4f, 0.8f, 1.0f},
    /* 51: Synth Strings 2 */
    {0.08f, 0.25f, 0.7f, 0.4f, 1.2f, 2.0f},
    /* 52: Choir Aahs */
    {0.08f, 0.2f, 0.7f, 0.35f, 0.5f, 1.5f},
    /* 53: Voice Oohs */
    {0.08f, 0.2f, 0.65f, 0.35f, 0.3f, 1.0f},
    /* 54: Synth Voice */
    {0.06f, 0.2f, 0.7f, 0.3f, 1.2f, 2.0f},
    /* 55: Orchestra Hit */
    {0.001f, 0.15f, 0.5f, 0.2f, 3.0f, 1.0f},
    /* 56: Trumpet */
    {0.015f, 0.12f, 0.6f, 0.2f, 1.2f, 1.0f},
    /* 57: Trombone */
    {0.02f, 0.15f, 0.6f, 0.25f, 0.9f, 1.0f},
    /* 58: Tuba */
    {0.02f, 0.18f, 0.6f, 0.25f, 0.7f, 0.5f},
    /* 59: Muted Trumpet */
    {0.01f, 0.1f, 0.5f, 0.2f, 2.0f, 2.0f},
    /* 60: French Horn */
    {0.04f, 0.18f, 0.65f, 0.3f, 0.6f, 1.0f},
    /* 61: Brass Section */
    {0.025f, 0.15f, 0.65f, 0.25f, 1.1f, 1.0f},
    /* 62: Synth Brass 1 */
    {0.01f, 0.12f, 0.7f, 0.2f, 1.8f, 1.0f},
    /* 63: Synth Brass 2 */
    {0.01f, 0.12f, 0.7f, 0.2f, 2.4f, 2.0f},
    /* 64: Soprano Sax */
    {0.02f, 0.15f, 0.65f, 0.2f, 1.0f, 2.0f},
    /* 65: Alto Sax */
    {0.02f, 0.15f, 0.65f, 0.2f, 0.9f, 2.0f},
    /* 66: Tenor Sax */
    {0.02f, 0.15f, 0.65f, 0.2f, 0.8f, 1.5f},
    /* 67: Baritone Sax */
    {0.02f, 0.15f, 0.65f, 0.2f, 0.7f, 1.0f},
    /* 68: Oboe */
    {0.03f, 0.15f, 0.7f, 0.2f, 1.1f, 2.0f},
    /* 69: English Horn */
    {0.03f, 0.18f, 0.7f, 0.25f, 0.9f, 1.5f},
    /* 70: Bassoon */
    {0.03f, 0.2f, 0.65f, 0.25f, 0.6f, 1.0f},
    /* 71: Clarinet */
    {0.02f, 0.15f, 0.7f, 0.2f, 0.5f, 2.0f},
    /* 72: Piccolo */
    {0.01f, 0.12f, 0.6f, 0.2f, 0.6f, 1.0f},
    /* 73: Flute */
    {0.03f, 0.15f, 0.65f, 0.25f, 0.5f, 1.0f},
    /* 74: Recorder */
    {0.02f, 0.15f, 0.6f, 0.2f, 0.4f, 1.0f},
    /* 75: Pan Flute */
    {0.04f, 0.18f, 0.6f, 0.3f, 0.9f, 1.5f},
    /* 76: Blown Bottle */
    {0.04f, 0.2f, 0.6f, 0.3f, 1.3f, 2.0f},
    /* 77: Shakuhachi */
    {0.03f, 0.2f, 0.6f, 0.3f, 1.5f, 2.0f},
    /* 78: Whistle */
    {0.02f, 0.12f, 0.6f, 0.2f, 0.7f, 1.0f},
    /* 79: Ocarina */
    {0.02f, 0.15f, 0.6f, 0.2f, 0.5f, 1.0f},
    /* 80: Square Lead */
    {0.005f, 0.1f, 0.7f, 0.15f, 1.5f, 2.0f},
    /* 81: Sawtooth Lead */
    {0.005f, 0.1f, 0.7f, 0.15f, 2.5f, 1.0f},
    /* 82: Calliope Lead */
    {0.01f, 0.12f, 0.7f, 0.2f, 1.2f, 1.5f},
    /* 83: Chiff Lead */
    {0.008f, 0.1f, 0.65f, 0.2f, 2.0f, 3.0f},
    /* 84: Charang Lead */
    {0.005f, 0.1f, 0.7f, 0.15f, 2.8f, 2.0f},
    /* 85: Voice Lead */
    {0.02f, 0.12f, 0.7f, 0.2f, 1.0f, 1.5f},
    /* 86: Fifths Lead */
    {0.005f, 0.1f, 0.7f, 0.15f, 2.2f, 1.0f},
    /* 87: Bass + Lead */
    {0.004f, 0.1f, 0.7f, 0.15f, 3.0f, 0.5f},
    /* 88: New Age Pad */
    {0.12f, 0.3f, 0.65f, 0.45f, 0.8f, 1.0f},
    /* 89: Warm Pad */
    {0.15f, 0.3f, 0.7f, 0.45f, 0.4f, 1.0f},
    /* 90: Polysynth Pad */
    {0.1f, 0.25f, 0.7f, 0.4f, 1.2f, 2.0f},
    /* 91: Choir Pad */
    {0.14f, 0.3f, 0.7f, 0.45f, 0.6f, 1.5f},
    /* 92: Bowed Pad */
    {0.15f, 0.3f, 0.7f, 0.45f, 0.5f, 1.0f},
    /* 93: Metallic Pad */
    {0.12f, 0.3f, 0.6f, 0.45f, 2.0f, 3.0f},
    /* 94: Halo Pad */
    {0.18f, 0.35f, 0.65f, 0.5f, 1.5f, 2.5f},
    /* 95: Sweep Pad */
    {0.2f, 0.35f, 0.65f, 0.5f, 2.2f, 1.5f},
    /* 96: Rain */
    {0.05f, 0.25f, 0.5f, 0.4f, 3.0f, 4.0f},
    /* 97: Soundtrack */
    {0.1f, 0.3f, 0.6f, 0.45f, 1.2f, 1.5f},
    /* 98: Crystal */
    {0.002f, 0.3f, 0.35f, 0.5f, 2.5f, 3.5f},
    /* 99: Atmosphere */
    {0.15f, 0.35f, 0.6f, 0.5f, 2.0f, 2.0f},
    /* 100: Brightness */
    {0.002f, 0.3f, 0.35f, 0.5f, 3.0f, 5.0f},
    /* 101: Goblins */
    {0.12f, 0.3f, 0.6f, 0.45f, 1.8f, 2.5f},
    /* 102: Echoes */
    {0.1f, 0.3f, 0.55f, 0.5f, 2.5f, 1.5f},
    /* 103: Sci-Fi */
    {0.08f, 0.3f, 0.55f, 0.45f, 3.5f, 3.0f},
    /* 104: Sitar */
    {0.002f, 0.15f, 0.45f, 0.25f, 2.2f, 2.0f},
    /* 105: Banjo */
    {0.001f, 0.1f, 0.45f, 0.2f, 1.0f, 1.0f},
    /* 106: Shamisen */
    {0.001f, 0.12f, 0.45f, 0.2f, 1.4f, 1.5f},
    /* 107: Koto */
    {0.001f, 0.15f, 0.4f, 0.25f, 1.2f, 1.0f},
    /* 108: Kalimba */
    {0.001f, 0.2f, 0.35f, 0.3f, 0.8f, 2.0f},
    /* 109: Bagpipe */
    {0.04f, 0.2f, 0.75f, 0.25f, 1.5f, 2.0f},
    /* 110: Fiddle */
    {0.03f, 0.15f, 0.65f, 0.25f, 0.7f, 1.0f},
    /* 111: Shanai */
    {0.02f, 0.15f, 0.6f, 0.2f, 1.3f, 2.0f},
    /* 112: Tinkle Bell */
    {0.001f, 0.4f, 0.25f, 0.6f, 3.0f, 5.0f},
    /* 113: Agogo */
    {0.001f, 0.15f, 0.3f, 0.3f, 2.0f, 3.0f},
    /* 114: Steel Drums */
    {0.002f, 0.18f, 0.45f, 0.3f, 1.8f, 2.5f},
    /* 115: Woodblock */
    {0.001f, 0.08f, 0.2f, 0.2f, 1.0f, 1.0f},
    /* 116: Taiko Drum */
    {0.002f, 0.25f, 0.5f, 0.3f, 1.5f, 0.5f},
    /* 117: Melodic Tom */
    {0.002f, 0.2f, 0.4f, 0.3f, 1.0f, 1.0f},
    /* 118: Synth Drum */
    {0.001f, 0.2f, 0.3f, 0.3f, 3.0f, 0.5f},
    /* 119: Reverse Cymbal */
    {0.15f, 0.05f, 0.5f, 0.4f, 3.5f, 4.0f},
    /* 120: Guitar Fret Noise */
    {0.001f, 0.1f, 0.3f, 0.25f, 2.5f, 3.0f},
    /* 121: Breath Noise */
    {0.05f, 0.2f, 0.5f, 0.4f, 1.0f, 1.0f},
    /* 122: Seashore */
    {0.1f, 0.3f, 0.5f, 0.5f, 2.0f, 1.5f},
    /* 123: Bird Tweet */
    {0.005f, 0.1f, 0.3f, 0.3f, 4.0f, 6.0f},
    /* 124: Telephone Ring */
    {0.002f, 0.15f, 0.5f, 0.2f, 2.0f, 4.0f},
    /* 125: Helicopter */
    {0.05f, 0.25f, 0.5f, 0.4f, 3.0f, 2.0f},
    /* 126: Applause */
    {0.05f, 0.3f, 0.5f, 0.45f, 1.5f, 2.0f},
    /* 127: Gunshot */
    {0.001f, 0.25f, 0.4f, 0.3f, 4.0f, 1.0f},
};

/* Default patch for unknown programs */
static const FMPatch g_default_patch = {0.01f, 0.1f, 0.7f, 0.2f, 1.0f, 1.0f};

/* ------------------------------------------------------------------
 * v23: GM percussion (channel 9 / drum map, notes 27..87).
 * Each drum is one FM voice (carrier + modulator) with exponential
 * decay, optional pitch sweep (kick/toms) and a noise blend
 * (snares/hats/cymbals/shakers). choke_group 1 = open hi-hat, cut by
 * a closed/pedal hi-hat start.
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  note;        /* GM percussion note number */
    float    freq_mult;   /* frequency multiplier on the note pitch */
    float    sweep_start; /* initial pitch multiplier (sweeps down to 1.0) */
    float    sweep_tau;   /* pitch sweep time constant in seconds (0 = none) */
    float    decay;       /* time to -60 dB in seconds */
    float    mod_index;   /* FM brightness */
    float    mod_ratio;   /* FM harmonic ratio (high = metallic) */
    float    noise_mix;   /* 0..1 noise blend */
    uint8_t  choke_group; /* 1 = open hat (choked), 0 = none */
} DrumPatch;

static const DrumPatch g_drum_patches[] = {
    {27,  1.0f, 1.0f, 0.00f, 0.06f, 3.0f, 7.0f,  0.30f, 0}, /* High Q */
    {28,  1.0f, 1.0f, 0.00f, 0.08f, 2.0f, 3.0f,  0.80f, 0}, /* Slap */
    {29,  1.5f, 1.0f, 0.00f, 0.05f, 2.0f, 5.0f,  0.80f, 0}, /* Scratch Push */
    {30,  1.2f, 1.0f, 0.00f, 0.05f, 2.0f, 5.0f,  0.80f, 0}, /* Scratch Pull */
    {31,  1.0f, 1.0f, 0.00f, 0.03f, 1.5f, 4.0f,  0.60f, 0}, /* Sticks */
    {32,  1.0f, 1.0f, 0.00f, 0.02f, 1.0f, 2.0f,  0.20f, 0}, /* Square Click */
    {33,  2.0f, 1.0f, 0.00f, 0.03f, 2.0f, 6.0f,  0.50f, 0}, /* Metronome Click */
    {34,  2.0f, 1.0f, 0.00f, 0.15f, 3.0f, 5.0f,  0.20f, 0}, /* Metronome Bell */
    {35,  1.0f, 2.0f, 0.05f, 0.25f, 0.7f, 1.0f,  0.05f, 0}, /* Acoustic Bass Drum */
    {36,  1.0f, 2.4f, 0.04f, 0.30f, 0.8f, 1.0f,  0.03f, 0}, /* Bass Drum 1 */
    {37,  1.0f, 1.0f, 0.00f, 0.04f, 2.0f, 4.0f,  0.70f, 0}, /* Side Stick */
    {38,  1.0f, 1.0f, 0.00f, 0.16f, 1.2f, 1.0f,  0.55f, 0}, /* Acoustic Snare */
    {39,  1.0f, 1.0f, 0.00f, 0.12f, 1.5f, 2.0f,  0.85f, 0}, /* Hand Clap */
    {40,  1.2f, 1.0f, 0.00f, 0.14f, 2.5f, 1.5f,  0.45f, 0}, /* Electric Snare */
    {41,  0.85f, 1.7f, 0.09f, 0.30f, 0.8f, 1.0f, 0.05f, 0}, /* Low Floor Tom */
    {42,  1.0f, 1.0f, 0.00f, 0.05f, 3.5f, 12.0f, 0.60f, 0}, /* Closed Hi-Hat */
    {43,  1.0f, 1.6f, 0.08f, 0.28f, 0.8f, 1.0f,  0.05f, 0}, /* High Floor Tom */
    {44,  1.0f, 1.0f, 0.00f, 0.04f, 3.5f, 12.0f, 0.60f, 0}, /* Pedal Hi-Hat */
    {45,  0.9f, 1.7f, 0.08f, 0.25f, 0.9f, 1.0f,  0.05f, 0}, /* Low Tom */
    {46,  1.0f, 1.0f, 0.00f, 0.40f, 3.5f, 12.0f, 0.55f, 1}, /* Open Hi-Hat */
    {47,  1.0f, 1.6f, 0.08f, 0.24f, 0.9f, 1.0f,  0.05f, 0}, /* Low-Mid Tom */
    {48,  1.1f, 1.6f, 0.07f, 0.22f, 0.9f, 1.0f,  0.05f, 0}, /* Hi-Mid Tom */
    {49,  1.0f, 1.0f, 0.00f, 1.20f, 3.0f, 8.0f,  0.40f, 0}, /* Crash Cymbal 1 */
    {50,  1.2f, 1.6f, 0.07f, 0.20f, 0.9f, 1.0f,  0.05f, 0}, /* High Tom */
    {51,  1.0f, 1.0f, 0.00f, 1.00f, 2.5f, 9.0f,  0.35f, 0}, /* Ride Cymbal 1 */
    {52,  0.9f, 1.0f, 0.00f, 0.90f, 4.0f, 7.0f,  0.45f, 0}, /* Chinese Cymbal */
    {53,  1.3f, 1.0f, 0.00f, 0.60f, 3.0f, 6.0f,  0.25f, 0}, /* Ride Bell */
    {54,  1.5f, 1.0f, 0.00f, 0.18f, 3.0f, 10.0f, 0.60f, 0}, /* Tambourine */
    {55,  1.3f, 1.0f, 0.00f, 0.70f, 3.5f, 8.0f,  0.50f, 0}, /* Splash Cymbal */
    {56,  1.0f, 1.0f, 0.00f, 0.25f, 1.5f, 1.5f,  0.10f, 0}, /* Cowbell */
    {57,  0.95f, 1.0f, 0.00f, 1.40f, 3.0f, 8.0f, 0.40f, 0}, /* Crash Cymbal 2 */
    {58,  1.2f, 1.0f, 0.00f, 0.30f, 2.5f, 5.0f,  0.50f, 0}, /* Vibraslap */
    {59,  1.05f, 1.0f, 0.00f, 1.10f, 2.5f, 9.0f, 0.35f, 0}, /* Ride Cymbal 2 */
    {60,  1.3f, 1.5f, 0.06f, 0.15f, 1.0f, 1.0f,  0.08f, 0}, /* Hi Bongo */
    {61,  0.9f, 1.5f, 0.07f, 0.18f, 1.0f, 1.0f,  0.08f, 0}, /* Low Bongo */
    {62,  1.1f, 1.4f, 0.04f, 0.10f, 1.0f, 1.0f,  0.05f, 0}, /* Mute Hi Conga */
    {63,  1.1f, 1.5f, 0.07f, 0.25f, 1.0f, 1.0f,  0.05f, 0}, /* Open Hi Conga */
    {64,  0.8f, 1.5f, 0.08f, 0.30f, 1.0f, 1.0f,  0.05f, 0}, /* Low Conga */
    {65,  1.2f, 1.0f, 0.00f, 0.15f, 1.5f, 2.0f,  0.30f, 0}, /* High Timbale */
    {66,  0.9f, 1.0f, 0.00f, 0.18f, 1.5f, 2.0f,  0.30f, 0}, /* Low Timbale */
    {67,  1.5f, 1.0f, 0.00f, 0.12f, 2.0f, 3.0f,  0.15f, 0}, /* High Agogo */
    {68,  1.0f, 1.0f, 0.00f, 0.14f, 2.0f, 3.0f,  0.15f, 0}, /* Low Agogo */
    {69,  1.0f, 1.0f, 0.00f, 0.12f, 2.0f, 8.0f,  0.85f, 0}, /* Cabasa */
    {70,  1.0f, 1.0f, 0.00f, 0.06f, 2.0f, 9.0f,  0.90f, 0}, /* Maracas */
    {71,  2.0f, 1.0f, 0.00f, 0.10f, 0.5f, 1.0f,  0.10f, 0}, /* Short Whistle */
    {72,  2.0f, 1.0f, 0.00f, 0.40f, 0.5f, 1.0f,  0.10f, 0}, /* Long Whistle */
    {73,  1.0f, 1.0f, 0.00f, 0.10f, 2.0f, 5.0f,  0.70f, 0}, /* Short Guiro */
    {74,  1.0f, 1.0f, 0.00f, 0.30f, 2.0f, 5.0f,  0.70f, 0}, /* Long Guiro */
    {75,  1.1f, 1.0f, 0.00f, 0.08f, 0.8f, 2.0f,  0.05f, 0}, /* Claves */
    {76,  1.3f, 1.0f, 0.00f, 0.06f, 1.0f, 2.0f,  0.10f, 0}, /* Hi Wood Block */
    {77,  0.9f, 1.0f, 0.00f, 0.08f, 1.0f, 2.0f,  0.10f, 0}, /* Low Wood Block */
    {78,  1.2f, 1.0f, 0.00f, 0.10f, 1.5f, 2.0f,  0.30f, 0}, /* Mute Cuica */
    {79,  1.2f, 1.0f, 0.00f, 0.25f, 1.5f, 2.0f,  0.30f, 0}, /* Open Cuica */
    {80,  2.2f, 1.0f, 0.00f, 0.20f, 3.0f, 10.0f, 0.15f, 0}, /* Mute Triangle */
    {81,  2.2f, 1.0f, 0.00f, 0.50f, 3.0f, 10.0f, 0.15f, 0}, /* Open Triangle */
    {82,  1.0f, 1.0f, 0.00f, 0.08f, 2.0f, 9.0f,  0.90f, 0}, /* Shaker */
    {83,  1.8f, 1.0f, 0.00f, 0.40f, 3.0f, 10.0f, 0.40f, 0}, /* Jingle Bell */
    {84,  2.5f, 1.0f, 0.00f, 0.90f, 3.0f, 8.0f,  0.20f, 0}, /* Belltree */
    {85,  1.5f, 1.0f, 0.00f, 0.05f, 2.0f, 8.0f,  0.70f, 0}, /* Castanets */
    {86,  0.6f, 1.5f, 0.05f, 0.15f, 0.8f, 1.0f,  0.05f, 0}, /* Mute Surdo */
    {87,  0.6f, 1.6f, 0.08f, 0.35f, 0.8f, 1.0f,  0.05f, 0}, /* Open Surdo */
};

#define DRUM_PATCH_COUNT (sizeof(g_drum_patches) / sizeof(g_drum_patches[0]))

/* Calculate note frequency */
static void init_note_frequencies(void) {
    /* A4 = 440 Hz, MIDI note 69 */
    for (int i = 0; i < MIDI_NOTE_COUNT; i++) {
        g_note_frequencies[i] = 440.0f * powf(2.0f, (i - 69) / 12.0f);
    }
}

/* Get patch for program */
static const FMPatch* get_patch(uint8_t program) {
    if (program < sizeof(g_patches) / sizeof(g_patches[0])) {
        return &g_patches[program];
    }
    return &g_default_patch;
}

/* v23: drum patch lookup (GM percussion map, notes 27..87) */
static const DrumPatch* get_drum_patch(uint8_t note) {
    if (note < 27 || note > 87) return NULL;
    return &g_drum_patches[note - 27];
}

/* v23: per-sample exponential decay coefficient so the envelope falls to
 * -60 dB (0.1%) over `decay_s` seconds at `rate`. */
static float drum_decay_coeff(float decay_s, uint32_t rate) {
    if (decay_s < 0.01f) decay_s = 0.01f;
    return expf(-6.9078f / (decay_s * (float)rate));
}

/* v23: (re)compute the tick timing after tempo/header changes */
static void midi_recompute_timing(MidiSequencer* seq) {
    if (seq->smpte) {
        /* us_per_tick was fixed from the SMPTE division at load time */
    } else if (seq->ticks_per_beat > 0) {
        seq->us_per_tick = (double)seq->microseconds_per_beat /
                           (double)seq->ticks_per_beat;
    } else {
        seq->us_per_tick = 500.0;
    }
    if (seq->us_per_tick < 1.0) seq->us_per_tick = 1.0;
    seq->samples_per_tick = (double)seq->sample_rate * seq->us_per_tick / 1000000.0;
}

/* v36.27: пройтись по треку БЕЗ синтеза и посчитать время конца в
 * МИКРОСЕКУНДАХ с учётом КАРТЫ ТЕМПА (сегменты между FF 51-событиями;
 * для SMPTE темп фиксирован заголовком). Прежние midi_get_length/
 * midi_get_duration_ms трактовали ДЛИНУ ТРЕКА В БАЙТАХ как тики —
 * Player.getDuration() врал на порядок для каждого файла. Семантика
 * статус/running-status зеркалит process_event (lenient). */
static uint32_t read_var_len(uint8_t* data, uint32_t* pos, uint32_t max); /* forward */

static uint64_t scan_track_time_us(const MidiSequencer* seq,
                                   const uint8_t* data, uint32_t len) {
    uint32_t pos = 0;
    uint64_t time_us = 0;
    uint32_t tick = 0;          /* позиция в тиках (для сегментов темпа) */
    uint32_t seg_start = 0;     /* тик начала текущего темп-сегмента */
    uint32_t seg_upb = 500000;  /* темп текущего сегмента (дефолт 120 BPM) */
    uint8_t run = 0;
    while (pos < len) {
        /* delta time */
        uint32_t v = 0;
        uint8_t b;
        do {
            if (pos >= len) break;
            b = data[pos++];
            v = (v << 7) | (b & 0x7F);
        } while (b & 0x80);
        tick += v;
        if (pos >= len) break;
        uint8_t st = data[pos];
        if (st < 0x80) {
            st = run; /* running status: статус-байт не потребляется */
        } else {
            pos++;
            run = (st < 0xF0) ? st : run; /* lenient, как в process_event */
        }
        uint8_t hi = st & 0xF0;
        if (st == 0xFF) {
            if (pos >= len) break;
            uint8_t mt = data[pos++];
            uint32_t ml = read_var_len((uint8_t*)data, &pos, len);
            if (mt == 0x51 && ml == 3 && pos + 3 <= len && !seq->smpte) {
                /* закрыть предыдущий сегмент, открыть новый темп */
                time_us += (uint64_t)(tick - seg_start) * seg_upb /
                           (seq->ticks_per_beat ? seq->ticks_per_beat : 480);
                seg_start = tick;
                seg_upb = ((uint32_t)data[pos] << 16) |
                          ((uint32_t)data[pos + 1] << 8) | data[pos + 2];
            }
            pos += ml;
        } else if (st == 0xF0 || st == 0xF7) {
            while (pos < len && data[pos] != 0xF7) pos++;
            if (pos < len) pos++;
        } else if (st == 0xF1 || st == 0xF3) {
            if (pos < len) pos++;
        } else if (st == 0xF2) {
            if (pos + 1 < len) pos += 2;
        } else if (hi == 0x80 || hi == 0x90 || hi == 0xA0 ||
                   hi == 0xB0 || hi == 0xE0) {
            pos += 2;
        } else if (hi == 0xC0 || hi == 0xD0) {
            pos += 1;
        }
        if (pos > len) pos = len;
    }
    if (seq->smpte) {
        /* SMPTE: фиксированные тики-в-секунду, us_per_tick задан заголовком */
        return (uint64_t)((double)tick * seq->us_per_tick);
    }
    time_us += (uint64_t)(tick - seg_start) * seg_upb /
               (seq->ticks_per_beat ? seq->ticks_per_beat : 480);
    return time_us;
}

/* Initialize MIDI subsystem */
int midi_init(uint32_t sample_rate) {
    g_sample_rate = sample_rate;
    init_note_frequencies();
    return 0;
}

/* Shutdown MIDI subsystem */
void midi_shutdown(void) {
    g_active_sequencer = NULL;
}

/* Read variable length value from MIDI data */
static uint32_t read_var_len(uint8_t* data, uint32_t* pos, uint32_t max) {
    uint32_t value = 0;
    uint8_t byte;
    
    do {
        if (*pos >= max) return value;
        byte = data[(*pos)++];
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    
    return value;
}

/* Parse MIDI file header */
static int parse_header(MidiFile* midi, uint32_t* pos) {
    if (midi->size < 14) return -1;
    
    /* Check "MThd" */
    if (memcmp(midi->data, "MThd", 4) != 0) return -1;
    
    uint32_t header_len = ((uint32_t)(midi->data[4]) <<  24) | (midi->data[5] << 16) | 
                          (midi->data[6] << 8) | midi->data[7];
    
    MidiHeader* hdr = &midi->sequencer->header;
    hdr->format = (midi->data[8] << 8) | midi->data[9];
    hdr->track_count = (midi->data[10] << 8) | midi->data[11];
    hdr->division = (midi->data[12] << 8) | midi->data[13];
    
    /* Determine division type */
    if (hdr->division & 0x8000) {
        /* v23: SMPTE - high byte is negative fps (-24/-25/-29/-30), low byte
         * is ticks-per-frame. Sequencing runs in ticks-per-second, so tempo
         * meta events are ignored for these files (standard behavior). */
        int neg_fps = (int)(int8_t)((hdr->division >> 8) & 0xFF);
        int tpf = hdr->division & 0xFF;
        double fps = (neg_fps == -29) ? 29.97 : (double)(-neg_fps);
        if (fps <= 0.0) fps = 30.0;
        if (tpf <= 0) tpf = 4;
        hdr->division_type = 1;  /* SMPTE */
        midi->sequencer->smpte = true;
        midi->sequencer->us_per_tick = 1000000.0 / (fps * (double)tpf);
        midi->sequencer->ticks_per_beat = 480;  /* nominal, unused for timing */
    } else {
        hdr->division_type = 0;  /* Ticks per beat */
        uint32_t tpb = hdr->division & 0x7FFF;
        midi->sequencer->ticks_per_beat = tpb ? tpb : 480;
    }
    
    *pos = 8 + header_len;
    return 0;
}

/* Parse track header */
static int parse_track(MidiFile* midi, int track_num, uint32_t* pos) {
    if (*pos + 8 > midi->size) return -1;
    
    uint8_t* data = midi->data + *pos;
    
    /* Check "MTrk" */
    if (memcmp(data, "MTrk", 4) != 0) return -1;
    
    MidiTrack* track = &midi->sequencer->tracks[track_num];
    track->length = ((uint32_t)(data[4]) <<  24) | (data[5] << 16) | 
                    (data[6] << 8) | data[7];
    track->data = data + 8;
    track->position = 0;
    track->delta_time = read_var_len(track->data, &track->position, track->length);
    track->running_status = 0;
    track->ended = false;
    
    *pos += 8 + track->length;
    return 0;
}

/* Load MIDI file from memory */
MidiFile* midi_load(const uint8_t* data, uint32_t size) {
    if (!data || size < 14) return NULL;

    /* v36.28 DIAG: fingerprint of the incoming MIDI data (Asphalt 3 3D
     * rasterizes its MIDIs through an Image drawRGB/getRGB roundtrip —
     * any pixel corruption shows up here before parsing). */
    {
        uint32_t crc = 0x21630E17u; /* ascii "MIDI-CRC" seed */
        for (uint32_t i = 0; i < size; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
        char head[64];
        for (int i = 0; i < 16; i++) snprintf(head + i * 2, 3, "%02X", data[i]);
        MEDIA_DEBUG("midi_load size=%u crc=%08X head=%s", size, crc, head);
    }

    MidiFile* midi = (MidiFile*)calloc(1, sizeof(MidiFile));
    if (!midi) return NULL;
    
    midi->data = (uint8_t*)malloc(size);
    if (!midi->data) {
        free(midi);
        return NULL;
    }
    memcpy(midi->data, data, size);
    midi->size = size;
    
    midi->sequencer = (MidiSequencer*)calloc(1, sizeof(MidiSequencer));
    if (!midi->sequencer) {
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Initialize channels */
    for (int i = 0; i < MIDI_CHANNELS; i++) {
        midi->sequencer->channels[i].program = 0;
        midi->sequencer->channels[i].volume = 100;
        midi->sequencer->channels[i].pan = 64;
        midi->sequencer->channels[i].expression = 127;
        midi->sequencer->channels[i].pitch_bend = 1.0f;
        midi->sequencer->channels[i].sustain = 0;
        midi->sequencer->channels[i].reverb = 40;  /* v23: GM-typical default send */
        midi->sequencer->channels[i].chorus = 0;
    }
    
    /* Parse header */
    uint32_t pos = 0;
    if (parse_header(midi, &pos) != 0) {
        free(midi->sequencer);
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Allocate tracks */
    midi->sequencer->tracks = (MidiTrack*)calloc(
        midi->sequencer->header.track_count, sizeof(MidiTrack));
    if (!midi->sequencer->tracks) {
        free(midi->sequencer);
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Parse tracks */
    for (int i = 0; i < midi->sequencer->header.track_count; i++) {
        if (parse_track(midi, i, &pos) != 0) {
            midi_free(midi);
            return NULL;
        }
    }

    /* v36.27: реальное время конца треков в мкс (для getDuration/getLength) */
    {
        MidiSequencer* sq = midi->sequencer;
        uint64_t total = 0;
        for (int i = 0; i < sq->header.track_count; i++) {
            sq->tracks[i].end_time_us =
                scan_track_time_us(sq, sq->tracks[i].data, sq->tracks[i].length);
            if (sq->tracks[i].end_time_us > total) total = sq->tracks[i].end_time_us;
        }
        sq->total_time_us = total;
    }
    
    /* Initialize sequencer */
    midi->sequencer->microseconds_per_beat = DEFAULT_TEMPO;
    midi->sequencer->current_tempo = 120.0f;
    midi->sequencer->sample_rate = g_sample_rate;
    midi->sequencer->volume = 1.0f;
    midi->sequencer->playing = false;
    midi->sequencer->last_real_time_us = 0;
    midi->sequencer->tick_accumulator = 0.0;

    /* v23: initialize the send-effect delay lengths for this sample rate */
    {
        double scale = (double)g_sample_rate / 44100.0;
        static const int comb_base[4] = {1557, 1617, 1491, 1422};
        static const int ap_base[2] = {225, 341};
        for (int i = 0; i < 4; i++) {
            int len = (int)(comb_base[i] * scale);
            if (len < 16) len = 16;
            if (len > 2048) len = 2048;
            midi->sequencer->rv_comb_len[i] = len;
            midi->sequencer->rv_comb_pos[i] = 0;
        }
        for (int i = 0; i < 2; i++) {
            int len = (int)(ap_base[i] * scale);
            if (len < 8) len = 8;
            if (len > 512) len = 512;
            midi->sequencer->rv_ap_len[i] = len;
            midi->sequencer->rv_ap_pos[i] = 0;
        }
        int clen = (int)(2048 * scale);  /* ~46 ms maximum delay */
        if (clen > 4096) clen = 4096;
        if (clen < 64) clen = 64;
        midi->sequencer->cho_len = clen;
        midi->sequencer->cho_pos = 0;
        midi->sequencer->cho_lfo = 0.0f;
    }

    /* Calculate samples per tick (v23: via us_per_tick; SMPTE set in header) */
    midi_recompute_timing(midi->sequencer);
    
    /* Allocate active notes */
    midi->sequencer->max_active_notes = MAX_POLYPHONY;
    midi->sequencer->active_notes = (MidiNote*)calloc(
        MAX_POLYPHONY, sizeof(MidiNote));
    
    return midi;
}

/* Free MIDI file */
void midi_free(MidiFile* midi) {
    if (!midi) return;
    
    if (midi->sequencer) {
        if (midi->sequencer->tracks) {
            free(midi->sequencer->tracks);
        }
        if (midi->sequencer->active_notes) {
            free(midi->sequencer->active_notes);
        }
        free(midi->sequencer);
    }
    if (midi->data) {
        free(midi->data);
    }
    free(midi);
}

/* Find free note slot */
static int find_free_note(MidiSequencer* seq) {
    for (int i = 0; i < seq->max_active_notes; i++) {
        if (!seq->active_notes[i].active) {
            return i;
        }
    }
    /* v36.27: все слоты заняты — крадём ТИХИЙ голос (минимальная огибающая),
     * а не «всегда слот 0»: старый код резал самый первый голос списка,
     * который мог быть ведущей мелодией. */
    int quiet = 0;
    for (int i = 1; i < seq->max_active_notes; i++) {
        if (seq->active_notes[i].envelope < seq->active_notes[quiet].envelope)
            quiet = i;
    }
    return quiet;
}

/* Start note */
static void start_note(MidiSequencer* seq, uint8_t channel,
                       uint8_t note, uint8_t velocity) {
    if (note >= MIDI_NOTE_COUNT) {
        seq->diag_rejects++; /* v36.28 DIAG */
        return;
    }
    if (!seq->active_notes || seq->max_active_notes <= 0) {
        seq->diag_rejects++; /* v36.28 DIAG: no voice pool */
        return;
    }

    int slot = find_free_note(seq);
    MidiNote* n = &seq->active_notes[slot];
    /* v36.27: слот мог содержать живой голос (кража) — счётчик активных
     * обязан быть честным, иначе active_note_count полз вверх навсегда. */
    if (n->active && seq->active_note_count > 0) seq->active_note_count--;

    n->active = true;
    n->channel = channel;
    n->note = note;
    n->velocity = velocity;
    n->frequency = g_note_frequencies[note];
    n->phase = 0.0f;
    n->mod_phase = 0.0f;
    n->envelope = 0.0f;
    n->envelope_target = velocity / 127.0f;

    /* v23: reset the drum/sustain state for every new voice */
    n->drum = false;
    n->key_held = true;
    n->sweep_k = 0.0f;
    n->noise_mix = 0.0f;
    n->choke_group = 0;
    n->rng = 0x9E3779B9u ^ ((uint32_t)note << 16) ^ (uint32_t)seq->position;
    n->freq_target = n->frequency;

    if (channel == 9) {
        /* v23: GM percussion channel - synthesize the note as a drum.
         * Melodic FM patches made drums sound like "fake piano". */
        const DrumPatch* dp = get_drum_patch(note);
        if (!dp) {
            n->active = false;  /* outside the GM kit: silent */
            return;
        }

        /* Choke: closed/pedal hi-hat cuts a ringing open hi-hat */
        if (note == 42 || note == 44) {
            for (int i = 0; i < seq->max_active_notes; i++) {
                MidiNote* o = &seq->active_notes[i];
                if (o->active && o != n && o->choke_group == 1) {
                    o->envelope *= 0.25f;
                    o->decay_k *= 0.02f;  /* fade out within a few ms */
                }
            }
        }

        n->drum = true;
        n->freq_target = g_note_frequencies[note] * dp->freq_mult;
        n->frequency = n->freq_target * dp->sweep_start;
        n->decay_k = drum_decay_coeff(dp->decay, seq->sample_rate);
        if (dp->sweep_tau > 0.0f) {
            n->sweep_k = expf(-1.0f / (dp->sweep_tau * (float)seq->sample_rate));
        }
        n->mod_index = dp->mod_index;
        n->mod_ratio = dp->mod_ratio;
        n->noise_mix = dp->noise_mix;
        n->choke_group = dp->choke_group;
        n->envelope = velocity / 127.0f;  /* percussive: instant attack */
        n->envelope_target = 0.0f;
        seq->active_note_count++;
        seq->diag_notes++; /* v36.28 DIAG */
        return;
    }

    /* Get FM patch */
    const FMPatch* patch = get_patch(seq->channels[channel].program);
    n->mod_index = patch->mod_index;
    n->mod_ratio = patch->mod_ratio;

    seq->active_note_count++;
    seq->diag_notes++; /* v36.28 DIAG */
}

/* Stop note */
static void stop_note(MidiSequencer* seq, uint8_t channel, uint8_t note) {
    /* v23: CC64 sustain - notes with the key released keep sounding while
     * the pedal is down and are released when the pedal comes back up. */
    bool sustained = seq->channels[channel].sustain >= 64;

    for (int i = 0; i < seq->max_active_notes; i++) {
        MidiNote* n = &seq->active_notes[i];
        if (n->active && n->channel == channel && n->note == note) {
            if (n->drum) continue;  /* one-shots ring out; choking handled at start */
            n->key_held = false;
            if (!sustained) {
                n->envelope_target = 0.0f;  /* Release */
            }
        }
    }
}

/* v23: release every note on this channel that is held only by the pedal.
 * channel > 15 releases on all channels (all-notes-off style). */
static void release_sustained(MidiSequencer* seq, uint8_t channel) {
    for (int i = 0; i < seq->max_active_notes; i++) {
        MidiNote* n = &seq->active_notes[i];
        if (n->active && !n->drum && !n->key_held &&
            (channel > 15 || n->channel == channel)) {
            n->envelope_target = 0.0f;
        }
    }
}

/* Process MIDI event */
static void process_event(MidiSequencer* seq, MidiTrack* track) {
    seq->diag_events++; /* v36.28 DIAG */
    uint8_t status, data1, data2;
    uint8_t* data = track->data;
    uint32_t pos = track->position;
    uint32_t len = track->length;
    
    if (pos >= len) {
        track->ended = true;
        return;
    }
    
    status = data[pos++];
    
    if (status < 0x80) {
        /* Running status */
        status = track->running_status;
        pos--;
    } else if (status < 0xF0) {
        /* v36.27 FIX: running status несут ТОЛЬКО channel-статусы
         * (0x80..0xEF). Прежний код сохранял ЛЮБОЙ статус, включая
         * 0xFF (meta) и 0xF0/0xF7 (sysex): после ЛЮБОГО meta-события
         * (темп, названия, тексты — они есть чуть ли не в каждом файле)
         * следующий event с пропущенным статус-байтом читался как
         * мусорная meta — поток рассинхронизировался до следующего
         * явного статуса. Симптомы: пропавшие/фантомные ноты и
         * мусорная перезапись темпа — «играют не все инструменты».
         * Компромиссное поведение (lenient, как у массовых плееров):
         * channel-статусы обновляют running status, meta/sysex —
         * оставляют прежний. */
        track->running_status = status;
    }
    
    uint8_t event_type = status & 0xF0;
    uint8_t channel = status & 0x0F;
    
    switch (event_type) {
        case MIDI_EVENT_NOTE_OFF:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                stop_note(seq, channel, data1);
            }
            break;
            
        case MIDI_EVENT_NOTE_ON:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                if (data2 == 0) {
                    stop_note(seq, channel, data1);
                } else {
                    start_note(seq, channel, data1, data2);
                }
            }
            break;
            
        case MIDI_EVENT_CONTROL_CHANGE:
            if (pos + 1 < len) {
                data1 = data[pos++];  /* Controller */
                data2 = data[pos++];  /* Value */
                
                switch (data1) {
                    case MIDI_CTRL_VOLUME:
                        seq->channels[channel].volume = data2;
                        break;
                    case MIDI_CTRL_PAN:
                        seq->channels[channel].pan = data2;
                        break;
                    case MIDI_CTRL_EXPRESSION:
                        seq->channels[channel].expression = data2;
                        break;
                    case MIDI_CTRL_SUSTAIN: {
                        /* v23: sustain pedal with release of held notes */
                        uint8_t prev = seq->channels[channel].sustain;
                        seq->channels[channel].sustain = data2;
                        if (prev >= 64 && data2 < 64) {
                            release_sustained(seq, channel);
                        }
                        break;
                    }
                    case MIDI_CTRL_BANK_SELECT:
                        seq->channels[channel].bank = data2;
                        break;
                    case MIDI_CTRL_MODULATION:
                        seq->channels[channel].modulation = data2;
                        break;
                    case MIDI_CTRL_REVERB:
                        /* v23: reverb send level drives the send effect */
                        seq->channels[channel].reverb = data2;
                        break;
                    case MIDI_CTRL_CHORUS:
                        seq->channels[channel].chorus = data2;
                        break;
                }
            }
            break;
            
        case MIDI_EVENT_PROGRAM_CHANGE:
            if (pos < len) {
                seq->channels[channel].program = data[pos++];
            }
            break;
            
        case MIDI_EVENT_PITCH_BEND:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                int16_t bend = ((data2 << 7) | data1) - 8192;
                seq->channels[channel].pitch_bend = powf(2.0f, bend / 8192.0f / 6.0f);
            }
            break;
            
        case MIDI_EVENT_SYSTEM:
            if (status == 0xFF) {
                /* Meta event */
                if (pos >= len) break;
                uint8_t meta_type = data[pos++];
                uint32_t meta_len = read_var_len(data, &pos, len);
                
                if (meta_type == 0x51 && meta_len == 3 && pos + 3 <= len && !seq->smpte) {
                    /* Tempo change (v23: ignored for SMPTE files) */
                    seq->microseconds_per_beat = (data[pos] << 16) | 
                                                  (data[pos+1] << 8) | data[pos+2];
                    midi_recompute_timing(seq);
                }
                pos += meta_len;
            } else if (status == 0xF1 || status == 0xF3) {
                /* v36.27: system common с 1 байтом данных (MTC quarter
                 * frame / song select). Раньше падали в «ничто» — pos
                 * не двигался, следующий delta читался со статус-байта
                 * (мусор). */
                if (pos < len) pos++;
            } else if (status == 0xF2) {
                /* v36.27: song position pointer — 2 байта данных. */
                if (pos + 1 < len) pos += 2;
            } else if (status == 0xF0 || status == 0xF7) {
                /* SysEx */
                while (pos < len && data[pos] != 0xF7) pos++;
                if (pos < len) pos++;
            }
            /* F4/F5/F6: без байтов данных — только статус (был потребован выше). */
            break;
            
        default:
            /* Skip unknown events */
            if (pos < len) pos++;
            if (pos < len) pos++;
            break;
    }
    
    track->position = pos;
    
    /* Read next delta time */
    if (track->position < track->length) {
        track->delta_time = read_var_len(track->data, &track->position, track->length);
    } else {
        track->ended = true;
    }
}

/* Process sequencer tick */
void midi_process_tick(MidiSequencer* seq) {
    if (!seq || !seq->playing) return;
    
    /* Process all tracks */
    bool all_ended = true;
    
    for (int i = 0; i < seq->header.track_count; i++) {
        MidiTrack* track = &seq->tracks[i];
        
        if (track->ended) continue;
        all_ended = false;
        
        while (track->delta_time == 0 && !track->ended) {
            process_event(seq, track);
        }
        
        if (!track->ended) {
            track->delta_time--;
        }
    }
    
    seq->position++;  /* v23: sequencer position advances for getMediaTime() */
    
    /* Check if all tracks ended */
    if (all_ended) {
        if (seq->loop) {
            /* Restart */
            for (int i = 0; i < seq->header.track_count; i++) {
                MidiTrack* track = &seq->tracks[i];
                track->position = 0;
                track->delta_time = read_var_len(track->data, &track->position, track->length);
                track->running_status = 0;
                track->ended = false;
            }
            /* v36.27 LOOP HYGIENE: следующий проход обязан стартовать
             * «чистым телефоном». Раньше активные голоса (в т.ч.
             * «залипшие» note-on без парного note-off) продолжали
             * звучать поверх второго прохода, а состояния каналов
             * (программы, громкости CC7, питч-бенд, сустейн) оставались
             * от КОНЦА предыдущего — аранжировка второго и дальше
             * кругов портилась. Голоса глушим, каналы сбрасываем в те
             * же дефолты, что и при загрузке (темп/программы из
             * tick-0 событий переприменятся сами при рестарте треков). */
            for (int i = 0; i < seq->max_active_notes; i++) {
                seq->active_notes[i].active = false;
            }
            seq->active_note_count = 0;
            for (int c = 0; c < MIDI_CHANNELS; c++) {
                MidiChannel* ch = &seq->channels[c];
                ch->program = 0;
                ch->volume = 100;
                ch->pan = 64;
                ch->expression = 127;
                ch->sustain = 0;
                ch->pitch_bend = 1.0f;
                ch->bank = 0;
                ch->modulation = 0;
                ch->reverb = 40;
                ch->chorus = 0;
            }
        } else {
            seq->playing = false;
            seq->end_reached = true;  /* v23: for PlayerListener END_OF_MEDIA */
        }
    }
}

/* Generate audio samples using FM synthesis */

/* v23: xorshift white noise for percussion */
static inline float drum_noise(uint32_t* st) {
    uint32_t x = *st;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* v23: feedback comb (Schroeder reverb stage) */
static inline float rv_comb_proc(float* buf, int len, int* pos, float in, float fb) {
    float out = buf[*pos];
    buf[*pos] = in + out * fb;
    if (++*pos >= len) *pos = 0;
    return out;
}

/* v23: allpass (Schroeder reverb stage) */
static inline float rv_ap_proc(float* buf, int len, int* pos, float in) {
    float bufout = buf[*pos];
    float out = -in + bufout;
    buf[*pos] = in + bufout * 0.5f;
    if (++*pos >= len) *pos = 0;
    return out;
}

void midi_generate_samples(MidiFile* midi, int16_t* buffer, int samples) {
    if (!midi || !midi->sequencer || !buffer) return;

    MidiSequencer* seq = midi->sequencer;

    memset(buffer, 0, samples * 2 * sizeof(int16_t));  /* Stereo */

    /* NOTE: Tick processing is now done via midi_advance_time() for time-based sync.
     * This function only generates audio samples, does NOT advance the sequencer. */

    for (int s = 0; s < samples; s++) {
        /* Generate audio for all active notes */
        float left = 0.0f, right = 0.0f;
        float rv_in = 0.0f, cho_in = 0.0f;

        if (seq->playing) {
            for (int i = 0; i < seq->max_active_notes; i++) {
                MidiNote* note = &seq->active_notes[i];
                if (!note->active) continue;

                MidiChannel* ch = &seq->channels[note->channel];
                float sample;

                if (note->drum) {
                    /* v23: percussion - exponential decay, optional pitch
                     * sweep and noise blend (channel 9 drum map) */
                    note->envelope *= note->decay_k;
                    if (note->envelope < 0.0008f) {
                        note->active = false;
                        seq->active_note_count--;
                        continue;
                    }
                    if (note->sweep_k > 0.0f) {
                        note->frequency += (note->freq_target - note->frequency) *
                                           (1.0f - note->sweep_k);
                    }

                    float freq = note->frequency * ch->pitch_bend;
                    float phase_inc = (2.0f * (float)M_PI * freq) / (float)seq->sample_rate;
                    float mod_phase_inc = phase_inc * note->mod_ratio;
                    float mod_wave = sinf(note->mod_phase);
                    float carrier = sinf(note->phase + note->mod_index * mod_wave);
                    float noise = drum_noise(&note->rng) * note->noise_mix;

                    sample = (carrier * (1.0f - note->noise_mix) + noise) *
                             note->envelope * (note->velocity / 127.0f);

                    note->phase += phase_inc;
                    note->mod_phase += mod_phase_inc;
                    if (note->phase > 2.0f * (float)M_PI) note->phase -= 2.0f * (float)M_PI;
                    if (note->mod_phase > 2.0f * (float)M_PI) note->mod_phase -= 2.0f * (float)M_PI;
                } else {
                    const FMPatch* patch = get_patch(ch->program);

                    /* Apply envelope */
                    float envelope_diff = note->envelope_target - note->envelope;
                    if (note->envelope_target > note->envelope) {
                        /* Attack */
                        note->envelope += envelope_diff * patch->attack;
                    } else {
                        /* Release */
                        note->envelope += envelope_diff * patch->release;
                    }

                    if (note->envelope < 0.001f && note->envelope_target == 0.0f) {
                        note->active = false;
                        seq->active_note_count--;
                        continue;
                    }

                    /* Calculate frequency with pitch bend */
                    float freq = note->frequency * ch->pitch_bend;

                    /* FM synthesis */
                    float mod_freq = freq * note->mod_ratio;
                    float mod_wave = sinf(note->mod_phase);

                    /* Phase increment */
                    float phase_inc = (2.0f * (float)M_PI * freq) / seq->sample_rate;
                    float mod_phase_inc = (2.0f * (float)M_PI * mod_freq) / seq->sample_rate;

                    /* Carrier with FM */
                    float carrier = sinf(note->phase + note->mod_index * mod_wave);

                    /* Apply envelope and velocity */
                    sample = carrier * note->envelope * (note->velocity / 127.0f);

                    note->phase += phase_inc;
                    note->mod_phase += mod_phase_inc;
                    if (note->phase > 2.0f * (float)M_PI) note->phase -= 2.0f * (float)M_PI;
                    if (note->mod_phase > 2.0f * (float)M_PI) note->mod_phase -= 2.0f * (float)M_PI;
                }

                /* v23: per-channel send levels (CC91/CC93) feed the effects */
                rv_in += sample * (ch->reverb / 127.0f) * 0.30f;
                cho_in += sample * (ch->chorus / 127.0f) * 0.30f;

                /* Apply channel volume and expression */
                sample *= (ch->volume / 127.0f) * (ch->expression / 127.0f);

                /* Pan */
                float pan = ch->pan / 127.0f;
                left += sample * (1.0f - pan);
                right += sample * pan;
            }
        }

        /* v23: reverb send effect (4 feedback combs + 2 allpass, Schroeder).
         * Runs while a tail is still ringing even if the note list is empty. */
        if (rv_in != 0.0f || seq->rv_comb[0][seq->rv_comb_pos[0]] != 0.0f) {
            float rv = 0.0f;
            for (int c = 0; c < 4; c++) {
                rv += rv_comb_proc(seq->rv_comb[c], seq->rv_comb_len[c],
                                   &seq->rv_comb_pos[c], rv_in, 0.78f + 0.02f * c);
            }
            rv = rv_ap_proc(seq->rv_ap[0], seq->rv_ap_len[0], &seq->rv_ap_pos[0], rv);
            rv = rv_ap_proc(seq->rv_ap[1], seq->rv_ap_len[1], &seq->rv_ap_pos[1], rv);
            left += rv * 0.5f;
            right += rv * 0.5f;
        }

        /* v23: chorus send effect (sine-modulated delay, slight feedback) */
        if (cho_in != 0.0f || seq->cho_buf[seq->cho_pos] != 0.0f) {
            seq->cho_lfo += (float)(2.0 * M_PI * 0.7) / (float)seq->sample_rate;
            if (seq->cho_lfo > 2.0f * (float)M_PI) seq->cho_lfo -= 2.0f * (float)M_PI;

            float delay = (float)seq->cho_len * 0.5f +
                          (float)seq->cho_len * 0.25f * sinf(seq->cho_lfo);
            float rpos = (float)seq->cho_pos - delay;
            while (rpos < 0.0f) rpos += (float)seq->cho_len;
            int i0 = (int)rpos;
            float fr = rpos - (float)i0;
            if (i0 >= seq->cho_len) i0 -= seq->cho_len;
            int i1 = (i0 + 1 == seq->cho_len) ? 0 : i0 + 1;
            float dout = seq->cho_buf[i0] + (seq->cho_buf[i1] - seq->cho_buf[i0]) * fr;

            seq->cho_buf[seq->cho_pos] = cho_in + dout * 0.15f;
            seq->cho_pos = (seq->cho_pos + 1 == seq->cho_len) ? 0 : seq->cho_pos + 1;

            left += dout * 0.35f;
            right += dout * 0.35f;
        }

        /* Apply master volume and clip */
        left *= seq->volume;
        right *= seq->volume;

        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;

        /* Output */
        buffer[s * 2] = (int16_t)(left * 32767);
        buffer[s * 2 + 1] = (int16_t)(right * 32767);
    }
}

/* Advance sequencer by real time (microseconds) - for time-based synchronization */
void midi_advance_time(MidiFile* midi, uint64_t elapsed_us) {
    if (!midi || !midi->sequencer || !midi->sequencer->playing) return;

    MidiSequencer* seq = midi->sequencer;

    /* Convert elapsed time to ticks */
    /* v23: via us_per_tick so SMPTE (ticks-per-second) files advance correctly */
    double ticks = (double)elapsed_us / seq->us_per_tick;
    seq->tick_accumulator += ticks;

    /* Process ticks */
    while (seq->tick_accumulator >= 1.0) {
        seq->tick_accumulator -= 1.0;
        midi_process_tick(seq);
    }
}

/* Start playback */
void midi_play(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = true;
    g_active_sequencer = midi->sequencer;
}

/* Stop playback */
void midi_stop(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = false;
    
    /* Stop all notes */
    for (int i = 0; i < midi->sequencer->max_active_notes; i++) {
        midi->sequencer->active_notes[i].active = false;
    }
    midi->sequencer->active_note_count = 0;
}

/* Pause playback */
void midi_pause(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = false;
}

/* Resume playback */
void midi_resume(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = true;
}

/* Set loop mode */
void midi_set_loop(MidiFile* midi, bool loop) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->loop = loop;
}

/* Set volume */
void midi_set_volume(MidiFile* midi, float volume) {
    if (!midi || !midi->sequencer) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    midi->sequencer->volume = volume;
}

/* Get current position in seconds */
float midi_get_position(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    
    MidiSequencer* seq = midi->sequencer;
    
    /* Convert position (ticks) to seconds */
    if (seq->us_per_tick > 0.0) {
        return (float)((double)seq->position * seq->us_per_tick / 1000000.0);
    }
    
    return 0.0f;
}

/* Get total length in seconds */
float midi_get_length(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    
    /* v36.27: честное время конца песни (карта темпов, скан при загрузке).
     * Прежний код брал max(длин треков) В БАЙТАХ и выдавал байты за тики. */
    return (float)((double)midi->sequencer->total_time_us / 1000000.0);
}

/* Direct note on */
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (g_active_sequencer) {
        start_note(g_active_sequencer, channel, note, velocity);
    }
}

/* Direct note off */
void midi_note_off(uint8_t channel, uint8_t note) {
    if (g_active_sequencer) {
        stop_note(g_active_sequencer, channel, note);
    }
}

/* Direct control change (v23: full controller set, incl. sustain pedal,
 * bank select, modulation and the reverb/chorus send levels) */
void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value) {
    if (!g_active_sequencer) return;
    MidiChannel* ch = &g_active_sequencer->channels[channel];

    switch (controller) {
        case MIDI_CTRL_VOLUME:
            ch->volume = value;
            break;
        case MIDI_CTRL_PAN:
            ch->pan = value;
            break;
        case MIDI_CTRL_EXPRESSION:
            ch->expression = value;
            break;
        case MIDI_CTRL_SUSTAIN: {
            uint8_t prev = ch->sustain;
            ch->sustain = value;
            if (prev >= 64 && value < 64) {
                release_sustained(g_active_sequencer, channel);
            }
            break;
        }
        case MIDI_CTRL_BANK_SELECT:
            ch->bank = value;
            break;
        case MIDI_CTRL_MODULATION:
            ch->modulation = value;
            break;
        case MIDI_CTRL_REVERB:
            ch->reverb = value;
            break;
        case MIDI_CTRL_CHORUS:
            ch->chorus = value;
            break;
    }
}

/* Direct program change */
void midi_program_change(uint8_t channel, uint8_t program) {
    if (g_active_sequencer) {
        g_active_sequencer->channels[channel].program = program;
    }
}

/* Direct pitch bend */
void midi_pitch_bend(uint8_t channel, int16_t value) {
    if (g_active_sequencer) {
        g_active_sequencer->channels[channel].pitch_bend = 
            powf(2.0f, value / 8192.0f / 6.0f);
    }
}

/* Get active sequencer */
MidiSequencer* midi_get_active_sequencer(void) {
    return g_active_sequencer;
}

/* Get duration in milliseconds */
uint32_t midi_get_duration_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    
    /* v36.27: total_time_us — конец песни по карте темпов (скан при
     * загрузке), а не байтовая длина трека. */
    return (uint32_t)(midi->sequencer->total_time_us / 1000);
}

/* Get current position in milliseconds */
uint32_t midi_get_position_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    
    MidiSequencer* seq = midi->sequencer;
    return (uint32_t)((double)seq->position * seq->us_per_tick / 1000.0);
}

/* Set position in milliseconds */
void midi_set_position_ms(MidiFile* midi, uint32_t position_ms) {
    if (!midi || !midi->sequencer) return;

    MidiSequencer* seq = midi->sequencer;
    if (seq->us_per_tick > 0.0) {
        seq->position = (uint32_t)((double)position_ms * 1000.0 / seq->us_per_tick);
    }
    seq->end_reached = false;  /* v23: seeking re-arms END_OF_MEDIA */

    /* v36.28 FIX («на главном экране Asphalt 3 3D тихо»): seek ДОЛЖЕН
     * ре-инициализировать треки так же, как старт прохода лупа в
     * midi_process_tick. Прежний код сбрасывал только byte-offset
     * (track->position=0) и ended, оставляя:
     *   - track->delta_time от СТАРОЙ позиции потока (мусор до десятков
     *     тысяч тиков — ноты «не наступают» никогда);
     *   - track->running_status от старой позиции (первый event после
     *     seek с running-status читался чужим статусом);
     *   - tick_accumulator (незакрытая дробь тика между сеансами).
     * А так как track->position=0 — это НАЧАЛО ТРЕКА ВМЕСТЕ С ЗАГОЛОВКОМ
     * "MTrk"+длиной, первые «события» парсились из байтов заголовка
     * (только они и успевали: diag ev=7, notes=0 — затем гигантская
     * мусорная дельта). Меню-мелодия Asphalt 3 (b(0,1): setMediaTime(0)
     * перед КАЖДЫМ start()) молчала ВСЕГДА; луповая гоночная тема
     * самовыздоравливала на рестарте лупа (там дельта перечитывается). */
    seq->tick_accumulator = 0.0;
    for (int i = 0; i < seq->header.track_count; i++) {
        MidiTrack* track = &seq->tracks[i];
        track->position = 0;
        track->ended = false;
        track->running_status = 0;
        track->delta_time = read_var_len(track->data, &track->position,
                                         track->length);
    }

    /* Seek на 0 — это рестарт: глушим голоса и сбрасываем каналы в те же
     * дефолты, что и рестарт лупа (v36.27 LOOP HYGIENE), иначе программы/
     * громкости/сустейн с прошлой позиции протекают в новое начало. */
    for (int i = 0; i < seq->max_active_notes; i++) {
        seq->active_notes[i].active = false;
    }
    seq->active_note_count = 0;
    for (int c = 0; c < MIDI_CHANNELS; c++) {
        MidiChannel* ch = &seq->channels[c];
        ch->program = 0;
        ch->volume = 100;
        ch->pan = 64;
        ch->expression = 127;
        ch->sustain = 0;
        ch->pitch_bend = 1.0f;
        ch->bank = 0;
        ch->modulation = 0;
        ch->reverb = 40;
        ch->chorus = 0;
    }
}

/* Get channel volume */
int midi_get_channel_volume(int channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 100;
    return g_active_sequencer->channels[channel].volume;
}

/* Get channel program */
int midi_get_program(int channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return g_active_sequencer->channels[channel].program;
}

/* ------------------------------------------------------------------
 * v23: per-file direct commands - MIDIControl on a specific player's
 * synthesizer instead of the global active sequencer.
 * ------------------------------------------------------------------ */

static MidiSequencer* file_seq(MidiFile* midi) {
    return midi ? midi->sequencer : NULL;
}

void midi_note_on_file(MidiFile* midi, uint8_t channel, uint8_t note, uint8_t velocity) {
    MidiSequencer* seq = file_seq(midi);
    if (seq) start_note(seq, channel, note, velocity);
}

void midi_note_off_file(MidiFile* midi, uint8_t channel, uint8_t note) {
    MidiSequencer* seq = file_seq(midi);
    if (seq) stop_note(seq, channel, note);
}

void midi_control_change_file(MidiFile* midi, uint8_t channel, uint8_t controller, uint8_t value) {
    MidiSequencer* seq = file_seq(midi);
    if (!seq) return;
    MidiChannel* ch = &seq->channels[channel];

    switch (controller) {
        case MIDI_CTRL_VOLUME:      ch->volume = value; break;
        case MIDI_CTRL_PAN:         ch->pan = value; break;
        case MIDI_CTRL_EXPRESSION:  ch->expression = value; break;
        case MIDI_CTRL_SUSTAIN: {
            uint8_t prev = ch->sustain;
            ch->sustain = value;
            if (prev >= 64 && value < 64) release_sustained(seq, channel);
            break;
        }
        case MIDI_CTRL_BANK_SELECT: ch->bank = value; break;
        case MIDI_CTRL_MODULATION:  ch->modulation = value; break;
        case MIDI_CTRL_REVERB:      ch->reverb = value; break;
        case MIDI_CTRL_CHORUS:      ch->chorus = value; break;
    }
}

void midi_program_change_file(MidiFile* midi, uint8_t channel, uint8_t program) {
    MidiSequencer* seq = file_seq(midi);
    if (seq) seq->channels[channel].program = program;
}

int midi_get_channel_volume_file(MidiFile* midi, int channel) {
    MidiSequencer* seq = file_seq(midi);
    if (!seq || channel < 0 || channel >= MIDI_CHANNELS) return 100;
    return seq->channels[channel].volume;
}

int midi_get_program_file(MidiFile* midi, int channel) {
    MidiSequencer* seq = file_seq(midi);
    if (!seq || channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return seq->channels[channel].program;
}

/* v23: minimal valid MIDI file (header + track with only EOT) used as the
 * synthesizer target for device://tone and device://midi players so that
 * MIDIControl can drive a synth attached to that player. */
MidiFile* midi_load_empty(void) {
    static const uint8_t empty_midi[] = {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,  /* MThd, len 6 */
        0x00, 0x00,                                   /* format 0 */
        0x00, 0x01,                                   /* 1 track */
        0x01, 0xE0,                                   /* 480 ticks per beat */
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x04,  /* MTrk, len 4 */
        0x00, 0xFF, 0x2F, 0x00                        /* delta 0, End of Track */
    };
    MidiFile* m = midi_load(empty_midi, (uint32_t)sizeof(empty_midi));
    return m;
}
