/* ------------------------------------------------------------------
 * nojme AMR-NB decoder facade (extern "C"), header for pure-C callers.
 * Implementation: src/amr/amr_nb_dec.c over the vendored
 * opencore-amrnb decoder, converted to plain C (Apache License 2.0).
 * ------------------------------------------------------------------
 */
#ifndef NOJME_AMR_NB_DEC_H
#define NOJME_AMR_NB_DEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* One AMR-NB frame decodes to 160 mono 16-bit samples at 8000 Hz. */
#define NOJME_AMRNB_PCM_RATE 8000
#define NOJME_AMRNB_PCMDIM 160

void* nojme_amrnb_dec_init(void);
void nojme_amrnb_dec_exit(void* state);
void nojme_amrnb_decode(void* state, const unsigned char* in,
                        short* out, int bfi);
int nojme_amrnb_frame_samples(void);

#ifdef __cplusplus
}
#endif

#endif /* NOJME_AMR_NB_DEC_H */
