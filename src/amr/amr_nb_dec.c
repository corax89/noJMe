/* ------------------------------------------------------------------
 * nojme AMR-NB decoder wrapper for the J2ME libretro core.
 *
 * Thin C facade over the vendored opencore-amrnb decoder (Apache
 * License 2.0, see opencore-amr-LICENSE). Mirrors the upstream
 * amrnb/wrapper.cpp Decoder_Interface, but exposes nojme_* symbols so
 * the pure-C media stack (src/midp/media.c) can link against it.
 *
 * The vendored opencore decoder (src/amr/opencore) has been converted
 * to plain C: no exceptions, no RTTI, no STL, allocation via
 * oscl_malloc (plain malloc). The whole AMR-NB decoder builds with the
 * C compiler only and carries no libstdc++ dependency.
 * ------------------------------------------------------------------
 */

#include "amr_nb_dec.h"

#include <sp_dec.h>
#include <amrdecode.h>

/* AMR-NB speech frames are 20 ms = 160 samples at 8000 Hz. */
#define NOJME_AMRNB_FRAME_SAMPLES 160

void* nojme_amrnb_dec_init(void) {
    void* state = NULL;
    if (GSMInitDecode(&state, (Word8*)"nojme") != 0 || !state) {
        return NULL;
    }
    return state;
}

void nojme_amrnb_dec_exit(void* state) {
    if (state) {
        GSMDecodeFrameExit(&state);
    }
}

/* in  = one AMR-NB storage-frame: TOC byte (FT/Q) + payload.
 * out = 160 mono 16-bit PCM samples.
 * bfi = 1 requests erasure/concealment of this frame. */
void nojme_amrnb_decode(void* state, const unsigned char* in,
                         short* out, int bfi) {
    if (!state || !in || !out) return;

    unsigned char type = (in[0] >> 3) & 0x0F;
    if (bfi) type = 15; /* AMR_NO_DATA -> comfort-noise concealment */

    AMRDecode((void*)state, (enum Frame_Type_3GPP)type,
              (UWord8*)(in + 1), out, MIME_IETF);
}

int nojme_amrnb_frame_samples(void) {
    return NOJME_AMRNB_FRAME_SAMPLES;
}
