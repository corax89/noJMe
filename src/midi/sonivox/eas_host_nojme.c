/*
 * NOJME - Sonivox host wrapper
 *
 * Memory-backed file I/O for EAS synthesizer. NOJME already has the
 * entire MIDI file (or tone sequence bytes) in memory — we route that
 * memory through the EAS_FILE_LOCATOR callback struct so Sonivox can
 * parse it without ever touching the filesystem. This keeps the Sonivox
 * synth usable on Switch (no SD card FS needed) and lets multiple
 * MidiFile instances share the same engine.
 *
 * The locator struct (EAS_FILE) lives in eas_types.h:
 *     { void *handle; int(*readAt)(void*, void*, int, int); int(*size)(void*); }
 *
 * Each EAS_HW_FILE slot has its own filePos but shares the locator.
 * EAS_HWDupHandle() just allocates another slot pointing at the same
 * locator (used by SMF type-1 parsers that open sub-streams).
 *
 * Apache 2.0 (inherits from Sonivox AOSP upstream).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eas_host.h"
#include "eas_report.h"
#include "eas_data.h"
#include "eas_midi.h"
#include "eas_miditypes.h"
#include "eas_parser.h"
#include "eas_vm_protos.h"
#include "eas_synth.h"

#ifndef EAS_MAX_FILE_HANDLES
#define EAS_MAX_FILE_HANDLES    32
#endif

/*============================================================================
 * NOJME v36.90 [MIDI-PERF]: runtime performance gates for the WT synth.
 *
 * The compile-time defines (_FILTER_ENABLED / _REVERB_ENABLED) still
 * BUILD the code paths in, but these flags decide at RUNTIME whether
 * they run. Defaults are the "fast" configuration the field asked for
 * (J2ME-era phones: no per-voice resonant filter, dry mix); midi.c
 * overrides them from settings.ini / env at midi_init() time.
 *
 * The read side is a plain int: the audio thread renders under
 * g_audio_mutex and the only writers (settings/menu/native API) also
 * take that mutex through the midi_set_* wrappers, so there is no
 * torn-read hazard worth a C11 atomic here (int reads/writes are
 * atomic on all target ABIs anyway).
 *============================================================================*/
int g_nojme_voice_filter = 0;   /* 1 = run WT_VoiceFilter when a patch sets k */
int g_nojme_reverb      = 0;    /* 1 = run the Schroeder reverb in the mixdown */

typedef struct eas_hw_file_tag {
    const EAS_FILE *locator;   /* points into EAS_FILE struct passed by caller */
    EAS_I32        filePos;    /* current read offset (per-handle) */
    EAS_BOOL       dup;        /* true if duplicated from another handle */
    EAS_BOOL       inUse;      /* slot is occupied */
} EAS_HW_FILE;

typedef struct eas_hw_inst_data_tag {
    EAS_HW_FILE files[EAS_MAX_FILE_HANDLES];
} EAS_HW_INST_DATA;

#ifdef _STATIC_MEMORY
EAS_HW_INST_DATA fileData;
#endif

/*----------------------------------------------------------------------------
 * EAS_HWInit
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWInit (EAS_HW_DATA_HANDLE *pHWInstData)
{
#ifndef _STATIC_MEMORY
    *pHWInstData = (EAS_HW_DATA_HANDLE) malloc(sizeof(EAS_HW_INST_DATA));
    if (!(*pHWInstData)) return EAS_ERROR_MALLOC_FAILED;
#else
    *pHWInstData = &fileData;
#endif
    EAS_HWMemSet(*pHWInstData, 0, sizeof(EAS_HW_INST_DATA));
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * EAS_HWShutdown
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWShutdown (EAS_HW_DATA_HANDLE hwInstData)
{
#ifndef _STATIC_MEMORY
    free(hwInstData);
#endif
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Memory wrappers
 *----------------------------------------------------------------------------*/
void *EAS_HWMalloc (EAS_HW_DATA_HANDLE hwInstData, EAS_I32 size)
{
    (void)hwInstData;
#ifdef _STATIC_MEMORY
    return NULL;
#else
    return malloc((size_t) size);
#endif
}

void EAS_HWFree (EAS_HW_DATA_HANDLE hwInstData, void *p)
{
    (void)hwInstData;
#ifndef _STATIC_MEMORY
    free(p);
#endif
}

void *EAS_HWMemCpy (void *dest, const void *src, EAS_I32 amount)
{
    return memcpy(dest, src, (size_t) amount);
}

void *EAS_HWMemSet (void *dest, int val, EAS_I32 amount)
{
    return memset(dest, val, (size_t) amount);
}

EAS_I32 EAS_HWMemCmp (const void *s1, const void *s2, EAS_I32 amount)
{
    return (EAS_I32) memcmp(s1, s2, (size_t) amount);
}

/*----------------------------------------------------------------------------
 * EAS_HWOpenFile — treat EAS_FILE_LOCATOR as the callback struct
 * {handle, readAt, size}. Store the pointer in our slot; no fopen.
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWOpenFile (EAS_HW_DATA_HANDLE hwInstData,
                           EAS_FILE_LOCATOR locator,
                           EAS_FILE_HANDLE *pFile,
                           EAS_FILE_MODE mode)
{
    EAS_HW_FILE *file;
    int i;

    *pFile = NULL;
    if (mode != EAS_FILE_READ)
        return EAS_ERROR_INVALID_FILE_MODE;

    if (!locator || !locator->readAt || !locator->size)
        return EAS_ERROR_INVALID_PARAMETER;

    file = hwInstData->files;
    for (i = 0; i < EAS_MAX_FILE_HANDLES; i++) {
        if (!file->inUse) {
            file->locator = (const EAS_FILE *) locator;
            file->filePos = 0;
            file->dup    = EAS_FALSE;
            file->inUse  = EAS_TRUE;
            *pFile = file;
            return EAS_SUCCESS;
        }
        file++;
    }
    return EAS_ERROR_MAX_FILES_OPEN;
}

/*----------------------------------------------------------------------------
 * EAS_HWReadFile
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWReadFile (EAS_HW_DATA_HANDLE hwInstData,
                            EAS_FILE_HANDLE file,
                            void *pBuffer, EAS_I32 n,
                            EAS_I32 *pBytesRead)
{
    EAS_I32 bytesRead;
    (void)hwInstData;

    if (!file || !file->inUse || !file->locator)
        return EAS_ERROR_INVALID_HANDLE;

    bytesRead = file->locator->readAt(file->locator->handle,
                                      pBuffer,
                                      file->filePos,
                                      (int) n);
    if (bytesRead < 0) bytesRead = 0;
    file->filePos += bytesRead;
    *pBytesRead = bytesRead;
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * EAS_HWGetByte / GetWord / GetDWord
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWGetByte (EAS_HW_DATA_HANDLE hwInstData,
                           EAS_FILE_HANDLE file, void *p)
{
    EAS_I32 n;
    EAS_RESULT result = EAS_HWReadFile(hwInstData, file, p, 1, &n);
    if (result != EAS_SUCCESS) return result;
    if (n != 1)                return EAS_EOF;
    return EAS_SUCCESS;
}

EAS_RESULT EAS_HWGetWord (EAS_HW_DATA_HANDLE hwInstData,
                          EAS_FILE_HANDLE file, void *p, EAS_BOOL msbFirst)
{
    EAS_U8 b0, b1;
    EAS_RESULT result;
    EAS_I32  n;

    if ((result = EAS_HWReadFile(hwInstData, file, &b0, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;
    if ((result = EAS_HWReadFile(hwInstData, file, &b1, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;

    *(EAS_U16*) p = msbFirst ? (((EAS_U16)b0 << 8) | b1)
                             : (((EAS_U16)b1 << 8) | b0);
    return EAS_SUCCESS;
}

EAS_RESULT EAS_HWGetDWord (EAS_HW_DATA_HANDLE hwInstData,
                           EAS_FILE_HANDLE file, void *p, EAS_BOOL msbFirst)
{
    EAS_U8 b0, b1, b2, b3;
    EAS_RESULT result;
    EAS_I32  n;

    if ((result = EAS_HWReadFile(hwInstData, file, &b0, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;
    if ((result = EAS_HWReadFile(hwInstData, file, &b1, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;
    if ((result = EAS_HWReadFile(hwInstData, file, &b2, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;
    if ((result = EAS_HWReadFile(hwInstData, file, &b3, 1, &n)) != EAS_SUCCESS) return result;
    if (n != 1) return EAS_EOF;

    if (msbFirst)
        *(EAS_U32*) p = ((EAS_U32)b0 << 24) | ((EAS_U32)b1 << 16) |
                        ((EAS_U32)b2 <<  8) |  (EAS_U32)b3;
    else
        *(EAS_U32*) p = ((EAS_U32)b3 << 24) | ((EAS_U32)b2 << 16) |
                        ((EAS_U32)b1 <<  8) |  (EAS_U32)b0;
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Position / seek
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWFilePos (EAS_HW_DATA_HANDLE hwInstData,
                          EAS_FILE_HANDLE file, EAS_I32 *pPosition)
{
    (void)hwInstData;
    if (!file || !file->inUse) return EAS_ERROR_INVALID_HANDLE;
    *pPosition = file->filePos;
    return EAS_SUCCESS;
}

EAS_RESULT EAS_HWFileSeek (EAS_HW_DATA_HANDLE hwInstData,
                           EAS_FILE_HANDLE file, EAS_I32 position)
{
    (void)hwInstData;
    if (!file || !file->inUse) return EAS_ERROR_INVALID_HANDLE;
    file->filePos = position;
    return EAS_SUCCESS;
}

EAS_RESULT EAS_HWFileSeekOfs (EAS_HW_DATA_HANDLE hwInstData,
                              EAS_FILE_HANDLE file, EAS_I32 position)
{
    (void)hwInstData;
    if (!file || !file->inUse) return EAS_ERROR_INVALID_HANDLE;
    file->filePos += position;
    return EAS_SUCCESS;
}

EAS_RESULT EAS_HWFileLength (EAS_HW_DATA_HANDLE hwInstData,
                             EAS_FILE_HANDLE file, EAS_I32 *pLength)
{
    (void)hwInstData;
    if (!file || !file->inUse || !file->locator) return EAS_ERROR_INVALID_HANDLE;
    *pLength = file->locator->size(file->locator->handle);
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * EAS_HWDupHandle — share the locator between two slots with independent
 * file positions. Used by SMF-1 type parsers when opening per-track streams.
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWDupHandle (EAS_HW_DATA_HANDLE hwInstData,
                            EAS_FILE_HANDLE file, EAS_FILE_HANDLE *pDupFile)
{
    EAS_HW_FILE *dup;
    int i;

    *pDupFile = NULL;
    if (!file || !file->inUse) return EAS_ERROR_INVALID_HANDLE;

    dup = hwInstData->files;
    for (i = 0; i < EAS_MAX_FILE_HANDLES; i++) {
        if (!dup->inUse) {
            dup->locator = file->locator;
            dup->filePos = 0;
            dup->dup    = EAS_TRUE;
            dup->inUse  = EAS_TRUE;
            *pDupFile = dup;
            return EAS_SUCCESS;
        }
        dup++;
    }
    return EAS_ERROR_MAX_FILES_OPEN;
}

EAS_RESULT EAS_HWCloseFile (EAS_HW_DATA_HANDLE hwInstData,
                            EAS_FILE_HANDLE file)
{
    (void)hwInstData;
    if (!file) return EAS_ERROR_INVALID_HANDLE;
    file->inUse  = EAS_FALSE;
    file->locator = NULL;
    file->filePos = 0;
    file->dup    = EAS_FALSE;
    return EAS_SUCCESS;
}

/*----------------------------------------------------------------------------
 * Stubs for phone hardware features NOJME doesn't have
 *----------------------------------------------------------------------------*/
EAS_RESULT EAS_HWVibrate   (EAS_HW_DATA_HANDLE hwInstData, EAS_BOOL state) { (void)hwInstData; (void)state; return EAS_SUCCESS; }
EAS_RESULT EAS_HWLED       (EAS_HW_DATA_HANDLE hwInstData, EAS_BOOL state) { (void)hwInstData; (void)state; return EAS_SUCCESS; }
EAS_RESULT EAS_HWBackLight (EAS_HW_DATA_HANDLE hwInstData, EAS_BOOL state) { (void)hwInstData; (void)state; return EAS_SUCCESS; }

/*----------------------------------------------------------------------------
 * Threading stubs (Sonivox calls these but we run single-threaded)
 *----------------------------------------------------------------------------*/
void *EAS_HWRegisterSignalHandler (void) { return NULL; }
EAS_RESULT EAS_HWUnRegisterSignalHandler (void *cookie) { (void)cookie; return EAS_SUCCESS; }

EAS_BOOL EAS_HWYield (EAS_HW_DATA_HANDLE hwInstData)
{
    (void)hwInstData;
    return EAS_FALSE;
}

/*============================================================================
 * NOJME v36.89: EAS_OpenMIDIStream / EAS_WriteMIDIStream /
 * EAS_CloseMIDIStream — the AOSP snapshot is missing these three
 * functions (they're declared in eas.h but not implemented in any of
 * the lib_src .c files we vendored). They are required for the
 * device://tone and device://midi J2ME players, where the host feeds
 * raw MIDI bytes (note-on/off, CC, PC) to the synth without going
 * through the SMF parser.
 *
 * Implementation notes (v36.89b FIX):
 *  - Each "raw MIDI stream" is a S_EAS_STREAM slot in pEASData->streams[]
 *    whose pParserModule points at EAS_NOJME_MIDIStream_Parser — a REAL
 *    (const) S_FILE_PARSER_INTERFACE. The first draft used the marker
 *    value (void*)1 here; that was a CRASH: EAS_Render / EAS_SetVolume
 *    (EAS_StreamReady) / EAS_Shutdown dereference pParserModule as a
 *    struct of function pointers, so a marker of 1 = wild call into the
 *    first page of memory (SEGV, confirmed under ASAN on host).
 *  - pfTime is left NULL ON PURPOSE: eas_public.c EAS_ParseEvents() has
 *    an explicit branch for "parser without a time function" — it treats
 *    the stream as a LIVE stream (advances pStream->time by frameLength,
 *    sets STREAM_FLAGS_PARSED). That is exactly the semantics a raw
 *    MIDI stream needs: no file parsing inside EAS_Render, bytes are
 *    parsed at write time via EAS_ParseMIDIStream, voices are rendered
 *    by the global VMRender() call.
 *  - EAS_Render still calls VMRender(pVoiceMgr) every frame, which renders
 *    the synth voices we started — so the audio path is identical to the
 *    SMF path, only the parser layer is bypassed.
 *============================================================================*/

/* Internal state for one raw MIDI stream. Stored in
 * S_EAS_STREAM.handle (so we don't need a separate global array). */
typedef struct nojme_raw_midi_tag {
    S_SYNTH       *pSynth;
    S_MIDI_STREAM  midiStream;
    EAS_BOOL8      inUse;
} NOJME_RAW_MIDI;

/* ---- raw MIDI stream parser interface (v36.89b) ---- */

static EAS_RESULT NM_CheckFileType (S_EAS_DATA *pEASData, EAS_FILE_HANDLE fileHandle, EAS_VOID_PTR *ppHandle, EAS_I32 offset)
{ (void)pEASData; (void)fileHandle; (void)ppHandle; (void)offset; return EAS_ERROR_UNRECOGNIZED_FORMAT; }

static EAS_RESULT NM_Prepare (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData)
{ (void)pEASData; (void)pInstData; return EAS_SUCCESS; }

static EAS_RESULT NM_Event (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData, EAS_INT parseMode)
{ (void)pEASData; (void)pInstData; (void)parseMode; return EAS_SUCCESS; }

static EAS_RESULT NM_State (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData, EAS_STATE *pState)
{ (void)pEASData; (void)pInstData; if (pState) *pState = EAS_STATE_READY; return EAS_SUCCESS; }

/* Only called from EAS_Shutdown's stream sweep (pParserModule && handle).
 * Owns the raw wrapper: shutdown the synth + free it. The normal close
 * path is EAS_CloseMIDIStream below, which clears the slot first, so the
 * two paths can never double-free. */
static EAS_RESULT NM_Close (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData)
{
    NOJME_RAW_MIDI *raw = (NOJME_RAW_MIDI *) pInstData;
    if (raw) {
        if (raw->pSynth) {
            VMMIDIShutdown(pEASData, raw->pSynth);
            raw->pSynth = NULL;
        }
        free(raw);
    }
    return EAS_SUCCESS;
}

static EAS_RESULT NM_Reset (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData)
{ (void)pEASData; (void)pInstData; return EAS_SUCCESS; }

static EAS_RESULT NM_Pause (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData)
{ (void)pEASData; (void)pInstData; return EAS_SUCCESS; }

static EAS_RESULT NM_Resume (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData)
{ (void)pEASData; (void)pInstData; return EAS_SUCCESS; }

static EAS_RESULT NM_Locate (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData, EAS_I32 time, EAS_BOOL *pParserLocate)
{ (void)pEASData; (void)pInstData; (void)time; if (pParserLocate) *pParserLocate = EAS_FALSE; return EAS_SUCCESS; }

static EAS_RESULT NM_SetData (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData, EAS_I32 param, EAS_I32 value)
{
    NOJME_RAW_MIDI *raw = (NOJME_RAW_MIDI *) pInstData;
    if (!raw || !raw->pSynth) return EAS_ERROR_INVALID_HANDLE;
    switch (param) {
        case PARSER_DATA_POLYPHONY:
            return VMSetPolyphony(pEASData->pVoiceMgr, raw->pSynth, value);
        default:
            return EAS_ERROR_INVALID_PARAMETER;
    }
}

static EAS_RESULT NM_GetData (S_EAS_DATA *pEASData, EAS_VOID_PTR pInstData, EAS_I32 param, EAS_I32 *pValue)
{
    NOJME_RAW_MIDI *raw = (NOJME_RAW_MIDI *) pInstData;
    if (!raw) return EAS_ERROR_INVALID_HANDLE;
    switch (param) {
        case PARSER_DATA_SYNTH_HANDLE:
            if (pValue) *pValue = (EAS_I32) raw->pSynth;
            return EAS_SUCCESS;
        default:
            return EAS_ERROR_INVALID_PARAMETER;
    }
}

/* pfTime == NULL is the AOSP "live stream" marker consumed by
 * EAS_ParseEvents(); see the block comment above. */
const S_FILE_PARSER_INTERFACE EAS_NOJME_MIDIStream_Parser =
{
    NM_CheckFileType,   /* pfCheckFileType */
    NM_Prepare,         /* pfPrepare */
    NULL,               /* pfTime    (NULL = live stream) */
    NM_Event,           /* pfEvent   */
    NM_State,           /* pfState   */
    NM_Close,           /* pfClose   */
    NM_Reset,           /* pfReset   */
    NM_Pause,           /* pfPause   */
    NM_Resume,          /* pfResume  */
    NM_Locate,          /* pfLocate  */
    NM_SetData,         /* pfSetData */
    NM_GetData,         /* pfGetData */
    NULL                /* pfGetMetaData */
};

EAS_PUBLIC EAS_RESULT EAS_OpenMIDIStream (EAS_DATA_HANDLE pEASData,
                                          EAS_HANDLE *pStreamHandle,
                                          EAS_HANDLE streamHandle)
{
    S_EAS_DATA  *pEAS = (S_EAS_DATA *) pEASData;
    int           i;
    EAS_RESULT    result;
    NOJME_RAW_MIDI *raw;

    *pStreamHandle = NULL;
    if (!pEAS || !pEAS->pVoiceMgr)
        return EAS_ERROR_INVALID_HANDLE;

    /* If streamHandle is non-NULL, the caller wants to attach a MIDI
     * stream to an existing stream (per AOSP doc). We don't support
     * that — fail gracefully and treat as a fresh stream. */
    (void) streamHandle;

    /* Allocate our raw-MIDI wrapper */
    raw = (NOJME_RAW_MIDI *) malloc(sizeof(NOJME_RAW_MIDI));
    if (!raw) return EAS_ERROR_MALLOC_FAILED;
    raw->pSynth = NULL;
    raw->inUse = EAS_TRUE;

    /* Init the MIDI synth instance */
    result = VMInitMIDI(pEAS, &raw->pSynth);
    if (result != EAS_SUCCESS || !raw->pSynth) {
        free(raw);
        return result != EAS_SUCCESS ? result : EAS_FAILURE;
    }

    /* Init MIDI stream parser state */
    EAS_InitMIDIStream(&raw->midiStream);

    /* Find a free slot in pEASData->streams[] to expose to EAS_Render */
    for (i = 0; i < MAX_NUMBER_STREAMS; i++) {
        if (pEAS->streams[i].pParserModule == NULL) {
            pEAS->streams[i].pParserModule = (S_FILE_PARSER_INTERFACE *) &EAS_NOJME_MIDIStream_Parser;
            pEAS->streams[i].handle        = (EAS_VOID_PTR) raw;
            pEAS->streams[i].streamFlags   = 0;
            pEAS->streams[i].time           = 0;
            pEAS->streams[i].volume         = DEFAULT_STREAM_VOLUME;
            *pStreamHandle = &pEAS->streams[i];
            return EAS_SUCCESS;
        }
    }

    /* No free slot — cleanup and bail */
    VMMIDIShutdown(pEAS, raw->pSynth);
    free(raw);
    return EAS_ERROR_MAX_STREAMS_OPEN;
}

EAS_PUBLIC EAS_RESULT EAS_WriteMIDIStream (EAS_DATA_HANDLE pEASData,
                                            EAS_HANDLE streamHandle,
                                            EAS_U8 *pBuffer,
                                            EAS_I32 count)
{
    S_EAS_DATA    *pEAS = (S_EAS_DATA *) pEASData;
    S_EAS_STREAM  *stream = (S_EAS_STREAM *) streamHandle;
    NOJME_RAW_MIDI *raw;
    EAS_INT        i;
    EAS_RESULT     result;

    if (!pEAS || !stream || !stream->handle || !pBuffer)
        return EAS_ERROR_INVALID_HANDLE;
    raw = (NOJME_RAW_MIDI *) stream->handle;

    for (i = 0; i < count; i++) {
        result = EAS_ParseMIDIStream(pEAS, raw->pSynth, &raw->midiStream,
                                     pBuffer[i], eParserModePlay);
        if (result != EAS_SUCCESS) return result;
    }
    return EAS_SUCCESS;
}

EAS_PUBLIC EAS_RESULT EAS_CloseMIDIStream (EAS_DATA_HANDLE pEASData,
                                           EAS_HANDLE streamHandle)
{
    S_EAS_DATA    *pEAS = (S_EAS_DATA *) pEASData;
    S_EAS_STREAM  *stream = (S_EAS_STREAM *) streamHandle;
    NOJME_RAW_MIDI *raw;

    if (!pEAS || !stream || !stream->handle)
        return EAS_ERROR_INVALID_HANDLE;

    raw = (NOJME_RAW_MIDI *) stream->handle;
    if (raw->pSynth) {
        VMMIDIShutdown(pEAS, raw->pSynth);
        raw->pSynth = NULL;
    }
    raw->inUse = EAS_FALSE;
    free(raw);

    stream->handle = NULL;
    stream->pParserModule = NULL;
    stream->streamFlags = 0;
    return EAS_SUCCESS;
}
