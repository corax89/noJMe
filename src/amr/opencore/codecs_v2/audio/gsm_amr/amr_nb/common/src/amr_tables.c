/* ==================================================================
 * amr_tables.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/bitno_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/bitreorder_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/c2_9pf_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gray_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gains_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/grid_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/inv_sqrt_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/lsp_lsf_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/lsp_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/overflow_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/ph_disp_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/pow2_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/qua_gain_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/window_tab.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sqrt_l_tbl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/get_const_tbls.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/bitno_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: bitno_tab.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    None

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

      File             : bitno.tab
      Purpose          : Tables for bit2prm and prm2bit

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 None

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"
#include "cnst.h"   /* parameter sizes: MAX_PRM_SIZE */
#include "mode.h"   /* N_MODES */
#include "bitno_tab.h"


/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; Define module specific macros here
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; DEFINES
    ; Include all pre-processor statements here. Include conditional
    ; compile variables also.
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; LOCAL STORE/BUFFER/POINTER DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/
    /* number of parameters per modes (values must be <= MAX_PRM_SIZE!) */
    const Word16 prmno[N_MODES] =
    {
        PRMNO_MR475,
        PRMNO_MR515,
        PRMNO_MR59,
        PRMNO_MR67,
        PRMNO_MR74,
        PRMNO_MR795,
        PRMNO_MR102,
        PRMNO_MR122,
        PRMNO_MRDTX
    };

    /* number of parameters to first subframe per modes */
    const Word16 prmnofsf[N_MODES - 1] =
    {
        PRMNOFSF_MR475,
        PRMNOFSF_MR515,
        PRMNOFSF_MR59,
        PRMNOFSF_MR67,
        PRMNOFSF_MR74,
        PRMNOFSF_MR795,
        PRMNOFSF_MR102,
        PRMNOFSF_MR122
    };

    /* parameter sizes (# of bits), one table per mode */
    const Word16 bitno_MR475[PRMNO_MR475] =
    {
        8, 8, 7,                                 /* LSP VQ          */
        8, 7, 2, 8,                              /* first subframe  */
        4, 7, 2,                                 /* second subframe */
        4, 7, 2, 8,                              /* third subframe  */
        4, 7, 2,                                 /* fourth subframe */
    };

    const Word16 bitno_MR515[PRMNO_MR515] =
    {
        8, 8, 7,                                 /* LSP VQ          */
        8, 7, 2, 6,                              /* first subframe  */
        4, 7, 2, 6,                              /* second subframe */
        4, 7, 2, 6,                              /* third subframe  */
        4, 7, 2, 6,                              /* fourth subframe */
    };

    const Word16 bitno_MR59[PRMNO_MR59] =
    {
        8, 9, 9,                                 /* LSP VQ          */
        8, 9, 2, 6,                              /* first subframe  */
        4, 9, 2, 6,                              /* second subframe */
        8, 9, 2, 6,                              /* third subframe  */
        4, 9, 2, 6,                              /* fourth subframe */
    };

    const Word16 bitno_MR67[PRMNO_MR67] =
    {
        8, 9, 9,                                 /* LSP VQ          */
        8, 11, 3, 7,                             /* first subframe  */
        4, 11, 3, 7,                             /* second subframe */
        8, 11, 3, 7,                             /* third subframe  */
        4, 11, 3, 7,                             /* fourth subframe */
    };

    const Word16 bitno_MR74[PRMNO_MR74] =
    {
        8, 9, 9,                                 /* LSP VQ          */
        8, 13, 4, 7,                             /* first subframe  */
        5, 13, 4, 7,                             /* second subframe */
        8, 13, 4, 7,                             /* third subframe  */
        5, 13, 4, 7,                             /* fourth subframe */
    };

    const Word16 bitno_MR795[PRMNO_MR795] =
    {
        9, 9, 9,                                 /* LSP VQ          */
        8, 13, 4, 4, 5,                          /* first subframe  */
        6, 13, 4, 4, 5,                          /* second subframe */
        8, 13, 4, 4, 5,                          /* third subframe  */
        6, 13, 4, 4, 5,                          /* fourth subframe */
    };

    const Word16 bitno_MR102[PRMNO_MR102] =
    {
        8, 9, 9,                                 /* LSP VQ          */
        8, 1, 1, 1, 1, 10, 10, 7, 7,             /* first subframe  */
        5, 1, 1, 1, 1, 10, 10, 7, 7,             /* second subframe */
        8, 1, 1, 1, 1, 10, 10, 7, 7,             /* third subframe  */
        5, 1, 1, 1, 1, 10, 10, 7, 7,             /* fourth subframe */
    };

    const Word16 bitno_MR122[PRMNO_MR122] =
    {
        7, 8, 9, 8, 6,                           /* LSP VQ          */
        9, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 5,   /* first subframe  */
        6, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 5,   /* second subframe */
        9, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 5,   /* third subframe  */
        6, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 5    /* fourth subframe */
    };

    const Word16 bitno_MRDTX[PRMNO_MRDTX] =
    {
        3,
        8, 9, 9,
        6
    };

    /* overall table with all parameter sizes for all modes */
    const Word16 * const bitno[N_MODES] =
    {
        bitno_MR475,
        bitno_MR515,
        bitno_MR59,
        bitno_MR67,
        bitno_MR74,
        bitno_MR795,
        bitno_MR102,
        bitno_MR122,
        bitno_MRDTX
    };
    /*----------------------------------------------------------------------------
    ; EXTERNAL FUNCTION REFERENCES
    ; Declare functions defined elsewhere and referenced in this module
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
    ; Declare variables used in this module but defined elsewhere
    ----------------------------------------------------------------------------*/


    /*--------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; Define all local variables
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Function body here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Return nothing or data or data pointer
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/bitreorder_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: bitreorder_tab.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    None

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function contains tables needed to reformat the encoded speech bits
 into IF2, WMF, and ETS.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 AMR Speech Codec Frame Structure,
 3GPP TS 26.101 version 4.1.0 Release 4, June 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "bitreorder_tab.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; Define module specific macros here
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; DEFINES
    ; Include all pre-processor statements here. Include conditional
    ; compile variables also.
    ----------------------------------------------------------------------------*/
#define NUM_MODES           16
#define NUMBIT_MR475        95
#define NUMBIT_MR515       103
#define NUMBIT_MR59        118
#define NUMBIT_MR67        134
#define NUMBIT_MR74        148
#define NUMBIT_MR795       159
#define NUMBIT_MR102       204
#define NUMBIT_MR122       244
#define NUMBIT_AMR_SID      39
#define NUMBIT_GSMEFR_SID   43
#define NUMBIT_TDMAEFR_SID  38
#define NUMBIT_PDCEFR_SID   37
#define NUMBIT_UNUSED1       0
#define NUMBIT_UNUSED2       0
#define NUMBIT_UNUSED3       0
#define NUMBIT_NO_DATA       0

#define MAX_NUM_BITS       244
    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; LOCAL STORE/BUFFER/POINTER DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/
    /* number of parameters per modes (values must be <= MAX_PRM_SIZE!) */
    const Word16 numOfBits[NUM_MODES] =
    {
        NUMBIT_MR475,
        NUMBIT_MR515,
        NUMBIT_MR59,
        NUMBIT_MR67,
        NUMBIT_MR74,
        NUMBIT_MR795,
        NUMBIT_MR102,
        NUMBIT_MR122,
        NUMBIT_AMR_SID,
        NUMBIT_GSMEFR_SID,
        NUMBIT_TDMAEFR_SID,
        NUMBIT_PDCEFR_SID,
        NUMBIT_UNUSED1,
        NUMBIT_UNUSED2,
        NUMBIT_UNUSED3,
        NUMBIT_NO_DATA
    };

    const Word16 reorderBits_MR475[NUMBIT_MR475] =
    {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 15, 23, 24, 25, 26,
        27, 28, 48, 49, 61, 62, 82, 83, 47, 46,
        45, 44, 81, 80, 79, 78, 17, 18, 20, 22,
        77, 76, 75, 74, 29, 30, 43, 42, 41, 40,
        38, 39, 16, 19, 21, 50, 51, 59, 60, 63,
        64, 72, 73, 84, 85, 93, 94, 32, 33, 35,
        36, 53, 54, 56, 57, 66, 67, 69, 70, 87,
        88, 90, 91, 34, 55, 68, 89, 37, 58, 71,
        92, 31, 52, 65, 86
    };

    const Word16 reorderBits_MR515[NUMBIT_MR515] =
    {
        7,  6,  5,  4,  3,  2,  1,  0, 15, 14,
        13, 12, 11, 10,  9,  8, 23, 24, 25, 26,
        27, 46, 65, 84, 45, 44, 43, 64, 63, 62,
        83, 82, 81, 102, 101, 100, 42, 61, 80, 99,
        28, 47, 66, 85, 18, 41, 60, 79, 98, 29,
        48, 67, 17, 20, 22, 40, 59, 78, 97, 21,
        30, 49, 68, 86, 19, 16, 87, 39, 38, 58,
        57, 77, 35, 54, 73, 92, 76, 96, 95, 36,
        55, 74, 93, 32, 51, 33, 52, 70, 71, 89,
        90, 31, 50, 69, 88, 37, 56, 75, 94, 34,
        53, 72, 91
    };

    const Word16 reorderBits_MR59[NUMBIT_MR59] =
    {
        0,  1,  4,  5,  3,  6,  7,  2, 13, 15,
        8,  9, 11, 12, 14, 10, 16, 28, 74, 29,
        75, 27, 73, 26, 72, 30, 76, 51, 97, 50,
        71, 96, 117, 31, 77, 52, 98, 49, 70, 95,
        116, 53, 99, 32, 78, 33, 79, 48, 69, 94,
        115, 47, 68, 93, 114, 46, 67, 92, 113, 19,
        21, 23, 22, 18, 17, 20, 24, 111, 43, 89,
        110, 64, 65, 44, 90, 25, 45, 66, 91, 112,
        54, 100, 40, 61, 86, 107, 39, 60, 85, 106,
        36, 57, 82, 103, 35, 56, 81, 102, 34, 55,
        80, 101, 42, 63, 88, 109, 41, 62, 87, 108,
        38, 59, 84, 105, 37, 58, 83, 104
    };

    const Word16 reorderBits_MR67[NUMBIT_MR67] =
    {
        0,  1,  4,  3,  5,  6, 13,  7,  2,  8,
        9, 11, 15, 12, 14, 10, 28, 82, 29, 83,
        27, 81, 26, 80, 30, 84, 16, 55, 109, 56,
        110, 31, 85, 57, 111, 48, 73, 102, 127, 32,
        86, 51, 76, 105, 130, 52, 77, 106, 131, 58,
        112, 33, 87, 19, 23, 53, 78, 107, 132, 21,
        22, 18, 17, 20, 24, 25, 50, 75, 104, 129,
        47, 72, 101, 126, 54, 79, 108, 133, 46, 71,
        100, 125, 128, 103, 74, 49, 45, 70, 99, 124,
        42, 67, 96, 121, 39, 64, 93, 118, 38, 63,
        92, 117, 35, 60, 89, 114, 34, 59, 88, 113,
        44, 69, 98, 123, 43, 68, 97, 122, 41, 66,
        95, 120, 40, 65, 94, 119, 37, 62, 91, 116,
        36, 61, 90, 115
    };

    const Word16 reorderBits_MR74[NUMBIT_MR74] =
    {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 15, 16, 26, 87, 27,
        88, 28, 89, 29, 90, 30, 91, 51, 80, 112,
        141, 52, 81, 113, 142, 54, 83, 115, 144, 55,
        84, 116, 145, 58, 119, 59, 120, 21, 22, 23,
        17, 18, 19, 31, 60, 92, 121, 56, 85, 117,
        146, 20, 24, 25, 50, 79, 111, 140, 57, 86,
        118, 147, 49, 78, 110, 139, 48, 77, 53, 82,
        114, 143, 109, 138, 47, 76, 108, 137, 32, 33,
        61, 62, 93, 94, 122, 123, 41, 42, 43, 44,
        45, 46, 70, 71, 72, 73, 74, 75, 102, 103,
        104, 105, 106, 107, 131, 132, 133, 134, 135, 136,
        34, 63, 95, 124, 35, 64, 96, 125, 36, 65,
        97, 126, 37, 66, 98, 127, 38, 67, 99, 128,
        39, 68, 100, 129, 40, 69, 101, 130
    };

    const Word16 reorderBits_MR795[NUMBIT_MR795] =
    {
        8,  7,  6,  5,  4,  3,  2, 14, 16,  9,
        10, 12, 13, 15, 11, 17, 20, 22, 24, 23,
        19, 18, 21, 56, 88, 122, 154, 57, 89, 123,
        155, 58, 90, 124, 156, 52, 84, 118, 150, 53,
        85, 119, 151, 27, 93, 28, 94, 29, 95, 30,
        96, 31, 97, 61, 127, 62, 128, 63, 129, 59,
        91, 125, 157, 32, 98, 64, 130,  1,  0, 25,
        26, 33, 99, 34, 100, 65, 131, 66, 132, 54,
        86, 120, 152, 60, 92, 126, 158, 55, 87, 121,
        153, 117, 116, 115, 46, 78, 112, 144, 43, 75,
        109, 141, 40, 72, 106, 138, 36, 68, 102, 134,
        114, 149, 148, 147, 146, 83, 82, 81, 80, 51,
        50, 49, 48, 47, 45, 44, 42, 39, 35, 79,
        77, 76, 74, 71, 67, 113, 111, 110, 108, 105,
        101, 145, 143, 142, 140, 137, 133, 41, 73, 107,
        139, 37, 69, 103, 135, 38, 70, 104, 136
    };

    const Word16 reorderBits_MR102[NUMBIT_MR102] =
    {
        7,  6,  5,  4,  3,  2,  1,  0, 16, 15,
        14, 13, 12, 11, 10,  9,  8, 26, 27, 28,
        29, 30, 31, 115, 116, 117, 118, 119, 120, 72,
        73, 161, 162, 65, 68, 69, 108, 111, 112, 154,
        157, 158, 197, 200, 201, 32, 33, 121, 122, 74,
        75, 163, 164, 66, 109, 155, 198, 19, 23, 21,
        22, 18, 17, 20, 24, 25, 37, 36, 35, 34,
        80, 79, 78, 77, 126, 125, 124, 123, 169, 168,
        167, 166, 70, 67, 71, 113, 110, 114, 159, 156,
        160, 202, 199, 203, 76, 165, 81, 82, 92, 91,
        93, 83, 95, 85, 84, 94, 101, 102, 96, 104,
        86, 103, 87, 97, 127, 128, 138, 137, 139, 129,
        141, 131, 130, 140, 147, 148, 142, 150, 132, 149,
        133, 143, 170, 171, 181, 180, 182, 172, 184, 174,
        173, 183, 190, 191, 185, 193, 175, 192, 176, 186,
        38, 39, 49, 48, 50, 40, 52, 42, 41, 51,
        58, 59, 53, 61, 43, 60, 44, 54, 194, 179,
        189, 196, 177, 195, 178, 187, 188, 151, 136, 146,
        153, 134, 152, 135, 144, 145, 105, 90, 100, 107,
        88, 106, 89, 98, 99, 62, 47, 57, 64, 45,
        63, 46, 55, 56
    };

    const Word16 reorderBits_MR122[NUMBIT_MR122] =
    {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
        10, 11, 12, 13, 14, 23, 15, 16, 17, 18,
        19, 20, 21, 22, 24, 25, 26, 27, 28, 38,
        141, 39, 142, 40, 143, 41, 144, 42, 145, 43,
        146, 44, 147, 45, 148, 46, 149, 47, 97, 150,
        200, 48, 98, 151, 201, 49, 99, 152, 202, 86,
        136, 189, 239, 87, 137, 190, 240, 88, 138, 191,
        241, 91, 194, 92, 195, 93, 196, 94, 197, 95,
        198, 29, 30, 31, 32, 33, 34, 35, 50, 100,
        153, 203, 89, 139, 192, 242, 51, 101, 154, 204,
        55, 105, 158, 208, 90, 140, 193, 243, 59, 109,
        162, 212, 63, 113, 166, 216, 67, 117, 170, 220,
        36, 37, 54, 53, 52, 58, 57, 56, 62, 61,
        60, 66, 65, 64, 70, 69, 68, 104, 103, 102,
        108, 107, 106, 112, 111, 110, 116, 115, 114, 120,
        119, 118, 157, 156, 155, 161, 160, 159, 165, 164,
        163, 169, 168, 167, 173, 172, 171, 207, 206, 205,
        211, 210, 209, 215, 214, 213, 219, 218, 217, 223,
        222, 221, 73, 72, 71, 76, 75, 74, 79, 78,
        77, 82, 81, 80, 85, 84, 83, 123, 122, 121,
        126, 125, 124, 129, 128, 127, 132, 131, 130, 135,
        134, 133, 176, 175, 174, 179, 178, 177, 182, 181,
        180, 185, 184, 183, 188, 187, 186, 226, 225, 224,
        229, 228, 227, 232, 231, 230, 235, 234, 233, 238,
        237, 236, 96, 199
    };

    /* overall table with all parameter sizes for all modes */
    const Word16 * const reorderBits[NUM_MODES-1] =
    {
        reorderBits_MR475,
        reorderBits_MR515,
        reorderBits_MR59,
        reorderBits_MR67,
        reorderBits_MR74,
        reorderBits_MR795,
        reorderBits_MR102,
        reorderBits_MR122
    };

    /* Number of Frames (16-bit segments sent for each mode */
    const Word16 numCompressedBytes[16] =
    {
        13, /*4.75*/
        14, /*5.15*/
        16, /*5.90*/
        18, /*6.70*/
        19, /*7.40*/
        21, /*7.95*/
        26, /*10.2*/
        31, /*12.2*/
        6, /*GsmAmr comfort noise*/
        6, /*Gsm-Efr comfort noise*/
        6, /*IS-641 comfort noise*/
        6, /*Pdc-Efr comfort noise*/
        0, /*future use*/
        0, /*future use*/
        0, /*future use*/
        1  /*No transmission*/
    };
    /*----------------------------------------------------------------------------
    ; EXTERNAL FUNCTION REFERENCES
    ; Declare functions defined elsewhere and referenced in this module
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
    ; Declare variables used in this module but defined elsewhere
    ----------------------------------------------------------------------------*/

    /*--------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; Define all local variables
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Function body here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Return nothing or data or data pointer
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/c2_9pf_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: c2_9pf_tab.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for startPos[] used by the functions
 c2_9pf.c and d2_9pf.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "get_const_tbls.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    extern const Word16 startPos[];
    const Word16 startPos[2*4*2] =
    {
        0, 2, 0, 3,
        0, 2, 0, 3,
        1, 3, 2, 4,
        1, 4, 1, 4
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] c2_9pf.c UMTS GSM AMR speech codec, R99 -  Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gray_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: gray_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for the gray encoding and decoding tables,
 gray_tbl[] and dgray_tbl[] used by the c1035pf and d1035pf module
 respectively.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    extern const Word16 gray[];
    extern const Word16 dgray[];
    const Word16 gray[8]  = {0, 1, 3, 2, 6, 4, 5, 7};
    const Word16 dgray[8] = {0, 1, 3, 2, 5, 6, 4, 7};

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] gray.tab,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gains_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: gains_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/
#define NB_QUA_PITCH 16
#define NB_QUA_CODE 32

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/


    extern const Word16 qua_gain_pitch[];
    const Word16 qua_gain_pitch[NB_QUA_PITCH] =
    {
        0, 3277, 6556, 8192, 9830, 11469, 12288, 13107,
        13926, 14746, 15565, 16384, 17203, 18022, 18842, 19661
    };


    extern const Word16 qua_gain_code[];
    const Word16 qua_gain_code[(NB_QUA_CODE+1)*3] =
    {
        /* gain factor (g_fac) and quantized energy error (qua_ener_MR122, qua_ener)
         * are stored:
         *
         * qua_ener_MR122 = log2(g_fac)      (not the rounded floating point value, but
         *                                    the value the original EFR algorithm
         *                                    calculates from g_fac [using Log2])
         * qua_ener       = 20*log10(g_fac); (rounded floating point value)
         *
         *
         * g_fac (Q11), qua_ener_MR122 (Q10), qua_ener (Q10)
         */
        159,                -3776,          -22731,
        206,                -3394,          -20428,
        268,                -3005,          -18088,
        349,                -2615,          -15739,
        419,                -2345,          -14113,
        482,                -2138,          -12867,
        554,                -1932,          -11629,
        637,                -1726,          -10387,
        733,                -1518,           -9139,
        842,                -1314,           -7906,
        969,                -1106,           -6656,
        1114,                 -900,           -5416,
        1281,                 -694,           -4173,
        1473,                 -487,           -2931,
        1694,                 -281,           -1688,
        1948,                  -75,            -445,
        2241,                  133,             801,
        2577,                  339,            2044,
        2963,                  545,            3285,
        3408,                  752,            4530,
        3919,                  958,            5772,
        4507,                 1165,            7016,
        5183,                 1371,            8259,
        5960,                 1577,            9501,
        6855,                 1784,           10745,
        7883,                 1991,           11988,
        9065,                 2197,           13231,
        10425,                 2404,           14474,
        12510,                 2673,           16096,
        16263,                 3060,           18429,
        21142,                 3448,           20763,
        27485,                 3836,           23097,
        27485,                 3836,           23097
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] gains.tab,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/grid_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: grid_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for grid_tbl[] used by the az_lsp()
 function.

 //  Table for az_lsp()
 //
 // grid[0] = 1.0;
 // grid[grid_points+1] = -1.0;
 // for (i = 1; i < grid_points; i++)
 //   grid[i] = cos((6.283185307*i)/(2.0*grid_points));
 //
 //

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "az_lsp.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/
#define grid_points 60

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/
    const Word16 grid[grid_points + 1] =
    {
        32760, 32723, 32588, 32364, 32051, 31651,
        31164, 30591, 29935, 29196, 28377, 27481,
        26509, 25465, 24351, 23170, 21926, 20621,
        19260, 17846, 16384, 14876, 13327, 11743,
        10125, 8480, 6812, 5126, 3425, 1714,
        0, -1714, -3425, -5126, -6812, -8480,
        -10125, -11743, -13327, -14876, -16384, -17846,
        -19260, -20621, -21926, -23170, -24351, -25465,
        -26509, -27481, -28377, -29196, -29935, -30591,
        -31164, -31651, -32051, -32364, -32588, -32723,
        -32760
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] grid.tab,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/inv_sqrt_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: inv_sqrt_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for table[] used by the inv_sqrt function.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "inv_sqrt.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/
    const Word16 inv_sqrt_tbl[49] =
    {

        32767, 31790, 30894, 30070, 29309, 28602, 27945, 27330, 26755, 26214,
        25705, 25225, 24770, 24339, 23930, 23541, 23170, 22817, 22479, 22155,
        21845, 21548, 21263, 20988, 20724, 20470, 20225, 19988, 19760, 19539,
        19326, 19119, 18919, 18725, 18536, 18354, 18176, 18004, 17837, 17674,
        17515, 17361, 17211, 17064, 16921, 16782, 16646, 16514, 16384
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] inv_sqrt.tab file,  UMTS GSM AMR speech codec, R99 - Version 3.2.0,
 March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: log2_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for log2_tbl[] used by the log2() and
 log2_norm() function.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "log2_norm.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    const Word16 log2_tbl[33] =
    {
        0, 1455, 2866, 4236, 5568, 6863, 8124, 9352, 10549, 11716,
        12855, 13967, 15054, 16117, 17156, 18172, 19167, 20142, 21097, 22033,
        22951, 23852, 24735, 25603, 26455, 27291, 28113, 28922, 29716, 30497,
        31266, 32023, 32767
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] log2.tab,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/lsp_lsf_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*
 Filename: lsp_lsf_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    extern const Word16 table[];
    const Word16 table[65] =
    {
        32767, 32729, 32610, 32413, 32138, 31786, 31357, 30853,
        30274, 29622, 28899, 28106, 27246, 26320, 25330, 24279,
        23170, 22006, 20788, 19520, 18205, 16846, 15447, 14010,
        12540, 11039, 9512, 7962, 6393, 4808, 3212, 1608,
        0, -1608, -3212, -4808, -6393, -7962, -9512, -11039,
        -12540, -14010, -15447, -16846, -18205, -19520, -20788, -22006,
        -23170, -24279, -25330, -26320, -27246, -28106, -28899, -29622,
        -30274, -30853, -31357, -31786, -32138, -32413, -32610, -32729,
        (Word16) 0x8000
    };

    /* 0x8000 = -32768 (used to silence the compiler) */

    /* slope used to compute y = acos(x) */

    extern const Word16 slope[];
    const Word16 slope[64] =
    {
        -26887, -8812, -5323, -3813, -2979, -2444, -2081, -1811,
        -1608, -1450, -1322, -1219, -1132, -1059, -998, -946,
        -901, -861, -827, -797, -772, -750, -730, -713,
        -699, -687, -677, -668, -662, -657, -654, -652,
        -652, -654, -657, -662, -668, -677, -687, -699,
        -713, -730, -750, -772, -797, -827, -861, -901,
        -946, -998, -1059, -1132, -1219, -1322, -1450, -1608,
        -1811, -2081, -2444, -2979, -3813, -5323, -8812, -26887
    };

    /*--------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/lsp_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: lsp_tab.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    None

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

   File             : lsp.tab
   Purpose          : Table for lsp init

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 None

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "lsp_tab.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; Define module specific macros here
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; DEFINES
    ; Include all pre-processor statements here. Include conditional
    ; compile variables also.
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; LOCAL STORE/BUFFER/POINTER DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/
    const Word16 lsp_init_data[M] =
    {
        30000, 26000, 21000, 15000, 8000,
        0, -8000, -15000, -21000, -26000
    };

    /*----------------------------------------------------------------------------
    ; EXTERNAL FUNCTION REFERENCES
    ; Declare functions defined elsewhere and referenced in this module
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
    ; Declare variables used in this module but defined elsewhere
    ----------------------------------------------------------------------------*/


    /*--------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; Define all local variables
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Function body here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Return nothing or data or data pointer
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/overflow_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: overflow_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for overflow_tbl[] used by the l_shl()
 and l_shr() functions.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/
    const Word32 overflow_tbl [32]   = {0x7fffffffL, 0x3fffffffL,
        0x1fffffffL, 0x0fffffffL,
        0x07ffffffL, 0x03ffffffL,
        0x01ffffffL, 0x00ffffffL,
        0x007fffffL, 0x003fffffL,
        0x001fffffL, 0x000fffffL,
        0x0007ffffL, 0x0003ffffL,
        0x0001ffffL, 0x0000ffffL,
        0x00007fffL, 0x00003fffL,
        0x00001fffL, 0x00000fffL,
        0x000007ffL, 0x000003ffL,
        0x000001ffL, 0x000000ffL,
        0x0000007fL, 0x0000003fL,
        0x0000001fL, 0x0000000fL,
        0x00000007L, 0x00000003L,
        0x00000001L, 0x00000000L
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] l_shl() function in basic_op2.c,  UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/ph_disp_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: ph_disp_tab.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the table of impulse responses of the phase dispersion
 filters. All impulse responses are in Q15

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "typedef.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/
    extern const Word16 ph_imp_low_MR795[];
    const Word16 ph_imp_low_MR795[40] =
    {
        26777,    801,   2505,   -683,  -1382,    582,    604,  -1274,   3511,  -5894,
        4534,   -499,  -1940,   3011,  -5058,   5614,  -1990,  -1061,  -1459,   4442,
        -700,  -5335,   4609,    452,   -589,  -3352,   2953,   1267,  -1212,  -2590,
        1731,   3670,  -4475,   -975,   4391,  -2537,    949,  -1363,   -979,   5734
    };
    extern const Word16 ph_imp_mid_MR795[];
    const Word16 ph_imp_mid_MR795[40] =
    {
        30274,   3831,  -4036,   2972,  -1048,  -1002,   2477,  -3043,   2815,  -2231,
        1753,  -1611,   1714,  -1775,   1543,  -1008,    429,   -169,    472,  -1264,
        2176,  -2706,   2523,  -1621,    344,    826,  -1529,   1724,  -1657,   1701,
        -2063,   2644,  -3060,   2897,  -1978,    557,    780,  -1369,    842,    655
    };

    extern const Word16 ph_imp_low[];
    const Word16 ph_imp_low[40] =
    {
        14690,  11518,   1268,  -2761,  -5671,   7514,    -35,  -2807,  -3040,   4823,
        2952,  -8424,   3785,   1455,   2179,  -8637,   8051,  -2103,  -1454,    777,
        1108,  -2385,   2254,   -363,   -674,  -2103,   6046,  -5681,   1072,   3123,
        -5058,   5312,  -2329,  -3728,   6924,  -3889,    675,  -1775,     29,  10145
    };
    extern const Word16 ph_imp_mid[];
    const Word16 ph_imp_mid[40] =
    {
        30274,   3831,  -4036,   2972,  -1048,  -1002,   2477,  -3043,   2815,  -2231,
        1753,  -1611,   1714,  -1775,   1543,  -1008,    429,   -169,    472,  -1264,
        2176,  -2706,   2523,  -1621,    344,    826,  -1529,   1724,  -1657,   1701,
        -2063,   2644,  -3060,   2897,  -1978,    557,    780,  -1369,    842,    655
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] ph_disp.tab, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/pow2_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: pow2_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for log2_tbl[] used by the Pow2() function.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "pow2.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    const Word16 pow2_tbl[33] =
    {
        16384, 16743, 17109, 17484, 17867, 18258, 18658, 19066, 19484, 19911,
        20347, 20792, 21247, 21713, 22188, 22674, 23170, 23678, 24196, 24726,
        25268, 25821, 26386, 26964, 27554, 28158, 28774, 29405, 30048, 30706,
        31379, 32066, 32767
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] pow2.tab,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/qua_gain_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: qua_gain_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "qua_gain_tbl.h"
#include "qua_gain.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/

    /* The tables contains the following data:
     *
     *    g_pitch        (Q14),
     *    g_fac          (Q12), (g_code = g_code0*g_fac),
     *    qua_ener_MR122 (Q10), (log2(g_fac))
     *    qua_ener       (Q10)  (20*log10(g_fac))
     *
     *    The log2() and log10() values are calculated on the fixed point value
     *    (g_fac Q12) and not on the original floating point value of g_fac
     *    to make the quantizer/MA predictdor use corresponding values.
     */

    /* table used in 'high' rates: MR67 MR74 */

    extern const Word16 table_gain_highrates[];
    const Word16 table_gain_highrates[VQ_SIZE_HIGHRATES*4] =
    {

        /*
         * Note: column 4 (qua_ener) contains the original values from IS641
         *       to ensure bit-exactness; however, they are not exactly the
         *       rounded value of (20*log10(g_fac))
         *
         */

        /*g_pit,    g_fac,  qua_ener_MR122, qua_ener */
        577,      662,           -2692,   -16214,
        806,     1836,           -1185,    -7135,
        3109,     1052,           -2008,   -12086,
        4181,     1387,           -1600,    -9629,
        2373,     1425,           -1560,    -9394,
        3248,     1985,           -1070,    -6442,
        1827,     2320,            -840,    -5056,
        941,     3314,            -313,    -1885,
        2351,     2977,            -471,    -2838,
        3616,     2420,            -777,    -4681,
        3451,     3096,            -414,    -2490,
        2955,     4301,              72,      434,
        1848,     4500,             139,      836,
        3884,     5416,             413,     2484,
        1187,     7210,             835,     5030,
        3083,     9000,            1163,     7002,
        7384,      883,           -2267,   -13647,
        5962,     1506,           -1478,    -8900,
        5155,     2134,            -963,    -5800,
        7944,     2009,           -1052,    -6335,
        6507,     2250,            -885,    -5327,
        7670,     2752,            -588,    -3537,
        5952,     3016,            -452,    -2724,
        4898,     3764,            -125,     -751,
        6989,     3588,            -196,    -1177,
        8174,     3978,             -43,     -260,
        6064,     4404,             107,      645,
        7709,     5087,             320,     1928,
        5523,     6021,             569,     3426,
        7769,     7126,             818,     4926,
        6060,     7938,             977,     5885,
        5594,    11487,            1523,     9172,
        10581,     1356,           -1633,    -9831,
        9049,     1597,           -1391,    -8380,
        9794,     2035,           -1033,    -6220,
        8946,     2415,            -780,    -4700,
        10296,     2584,            -681,    -4099,
        9407,     2734,            -597,    -3595,
        8700,     3218,            -356,    -2144,
        9757,     3395,            -277,    -1669,
        10177,     3892,             -75,     -454,
        9170,     4528,             148,      891,
        10152,     5004,             296,     1781,
        9114,     5735,             497,     2993,
        10500,     6266,             628,     3782,
        10110,     7631,             919,     5534,
        8844,     8727,            1117,     6728,
        8956,    12496,            1648,     9921,
        12924,      976,           -2119,   -12753,
        11435,     1755,           -1252,    -7539,
        12138,     2328,            -835,    -5024,
        11388,     2368,            -810,    -4872,
        10700,     3064,            -429,    -2580,
        12332,     2861,            -530,    -3192,
        11722,     3327,            -307,    -1848,
        11270,     3700,            -150,     -904,
        10861,     4413,             110,      663,
        12082,     4533,             150,      902,
        11283,     5205,             354,     2132,
        11960,     6305,             637,     3837,
        11167,     7534,             900,     5420,
        12128,     8329,            1049,     6312,
        10969,    10777,            1429,     8604,
        10300,    17376,            2135,    12853,
        13899,     1681,           -1316,    -7921,
        12580,     2045,           -1026,    -6179,
        13265,     2439,            -766,    -4610,
        14033,     2989,            -465,    -2802,
        13452,     3098,            -413,    -2482,
        12396,     3658,            -167,    -1006,
        13510,     3780,            -119,     -713,
        12880,     4272,              62,      374,
        13533,     4861,             253,     1523,
        12667,     5457,             424,     2552,
        13854,     6106,             590,     3551,
        13031,     6483,             678,     4084,
        13557,     7721,             937,     5639,
        12957,     9311,            1213,     7304,
        13714,    11551,            1532,     9221,
        12591,    15206,            1938,    11667,
        15113,     1540,           -1445,    -8700,
        15072,     2333,            -832,    -5007,
        14527,     2511,            -723,    -4352,
        14692,     3199,            -365,    -2197,
        15382,     3560,            -207,    -1247,
        14133,     3960,             -50,     -300,
        15102,     4236,              50,      298,
        14332,     4824,             242,     1454,
        14846,     5451,             422,     2542,
        15306,     6083,             584,     3518,
        14329,     6888,             768,     4623,
        15060,     7689,             930,     5602,
        14406,     9426,            1231,     7413,
        15387,     9741,            1280,     7706,
        14824,    14271,            1844,    11102,
        13600,    24939,            2669,    16067,
        16396,     1969,           -1082,    -6517,
        16817,     2832,            -545,    -3283,
        15713,     2843,            -539,    -3248,
        16104,     3336,            -303,    -1825,
        16384,     3963,             -49,     -294,
        16940,     4579,             165,      992,
        15711,     4599,             171,     1030,
        16222,     5448,             421,     2537,
        16832,     6382,             655,     3945,
        15745,     7141,             821,     4944,
        16326,     7469,             888,     5343,
        16611,     8624,            1100,     6622,
        17028,    10418,            1379,     8303,
        15905,    11817,            1565,     9423,
        16878,    14690,            1887,    11360,
        16515,    20870,            2406,    14483,
        18142,     2083,            -999,    -6013,
        19401,     3178,            -375,    -2257,
        17508,     3426,            -264,    -1589,
        20054,     4027,             -25,     -151,
        18069,     4249,              54,      326,
        18952,     5066,             314,     1890,
        17711,     5402,             409,     2461,
        19835,     6192,             610,     3676,
        17950,     7014,             795,     4784,
        21318,     7877,             966,     5816,
        17910,     9289,            1210,     7283,
        19144,     9290,            1210,     7284,
        20517,    11381,            1510,     9089,
        18075,    14485,            1866,    11234,
        19999,    17882,            2177,    13108,
        18842,    32764,            3072,    18494
    };


    /* table used in 'low' rates: MR475, MR515, MR59 */

    extern const Word16 table_gain_lowrates[];
    const Word16 table_gain_lowrates[VQ_SIZE_LOWRATES*4] =
    {
        /*g_pit,    g_fac,  qua_ener_MR122, qua_ener */
        10813,    28753,            2879,    17333,
        20480,     2785,            -570,    -3431,
        18841,     6594,             703,     4235,
        6225,     7413,             876,     5276,
        17203,    10444,            1383,     8325,
        21626,     1269,           -1731,   -10422,
        21135,     4423,             113,      683,
        11304,     1556,           -1430,    -8609,
        19005,    12820,            1686,    10148,
        17367,     2498,            -731,    -4398,
        17858,     4833,             244,     1472,
        9994,     2498,            -731,    -4398,
        17530,     7864,             964,     5802,
        14254,     1884,           -1147,    -6907,
        15892,     3153,            -387,    -2327,
        6717,     1802,           -1213,    -7303,
        18186,    20193,            2357,    14189,
        18022,     3031,            -445,    -2678,
        16711,     5857,             528,     3181,
        8847,     4014,             -30,     -180,
        15892,     8970,            1158,     6972,
        18022,     1392,           -1594,    -9599,
        16711,     4096,               0,        0,
        8192,      655,           -2708,   -16305,
        15237,    13926,            1808,    10884,
        14254,     3112,            -406,    -2444,
        14090,     4669,             193,     1165,
        5406,     2703,            -614,    -3697,
        13434,     6553,             694,     4180,
        12451,      901,           -2237,   -13468,
        12451,     2662,            -637,    -3833,
        3768,      655,           -2708,   -16305,
        14745,    23511,            2582,    15543,
        19169,     2457,            -755,    -4546,
        20152,     5079,             318,     1913,
        6881,     4096,               0,        0,
        20480,     8560,            1089,     6556,
        19660,      737,           -2534,   -15255,
        19005,     4259,              58,      347,
        7864,     2088,            -995,    -5993,
        11468,    12288,            1623,     9771,
        15892,     1474,           -1510,    -9090,
        15728,     4628,             180,     1086,
        9175,     1433,           -1552,    -9341,
        16056,     7004,             793,     4772,
        14827,      737,           -2534,   -15255,
        15073,     2252,            -884,    -5321,
        5079,     1228,           -1780,   -10714,
        13271,    17326,            2131,    12827,
        16547,     2334,            -831,    -5002,
        15073,     5816,             518,     3118,
        3932,     3686,            -156,     -938,
        14254,     8601,            1096,     6598,
        16875,      778,           -2454,   -14774,
        15073,     3809,            -107,     -646,
        6062,      614,           -2804,   -16879,
        9338,     9256,            1204,     7251,
        13271,     1761,           -1247,    -7508,
        13271,     3522,            -223,    -1343,
        2457,     1966,           -1084,    -6529,
        11468,     5529,             443,     2668,
        10485,      737,           -2534,   -15255,
        11632,     3194,            -367,    -2212,
        1474,      778,           -2454,   -14774
    };

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] qua_gain.tab,  UMTS GSM AMR speech codec,
                    R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/window_tab.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*
 Filename: window_tab.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    None

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

      File             : window.tab
      Purpose          : Hamming_cos window for LPC analysis.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 None

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "window_tab.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; Define module specific macros here
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; DEFINES
    ; Include all pre-processor statements here. Include conditional
    ; compile variables also.
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; LOCAL STORE/BUFFER/POINTER DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/
    /*************************************************************************
     *
     * Hamming_cos windows for LPC analysis.
     *
     *************************************************************************/

    /* window for non-EFR modesm; uses 40 samples lookahead */

    const Word16 window_200_40[L_WINDOW] =
    {
        2621,  2623,  2629,  2638,  2651,  2668,  2689,  2713,  2741,  2772,
        2808,  2847,  2890,  2936,  2986,  3040,  3097,  3158,  3223,  3291,
        3363,  3438,  3517,  3599,  3685,  3774,  3867,  3963,  4063,  4166,
        4272,  4382,  4495,  4611,  4731,  4853,  4979,  5108,  5240,  5376,
        5514,  5655,  5800,  5947,  6097,  6250,  6406,  6565,  6726,  6890,
        7057,  7227,  7399,  7573,  7750,  7930,  8112,  8296,  8483,  8672,
        8863,  9057,  9252,  9450,  9650,  9852, 10055, 10261, 10468, 10677,
        10888, 11101, 11315, 11531, 11748, 11967, 12187, 12409, 12632, 12856,
        13082, 13308, 13536, 13764, 13994, 14225, 14456, 14688, 14921, 15155,
        15389, 15624, 15859, 16095, 16331, 16568, 16805, 17042, 17279, 17516,
        17754, 17991, 18228, 18465, 18702, 18939, 19175, 19411, 19647, 19882,
        20117, 20350, 20584, 20816, 21048, 21279, 21509, 21738, 21967, 22194,
        22420, 22644, 22868, 23090, 23311, 23531, 23749, 23965, 24181, 24394,
        24606, 24816, 25024, 25231, 25435, 25638, 25839, 26037, 26234, 26428,
        26621, 26811, 26999, 27184, 27368, 27548, 27727, 27903, 28076, 28247,
        28415, 28581, 28743, 28903, 29061, 29215, 29367, 29515, 29661, 29804,
        29944, 30081, 30214, 30345, 30472, 30597, 30718, 30836, 30950, 31062,
        31170, 31274, 31376, 31474, 31568, 31659, 31747, 31831, 31911, 31988,
        32062, 32132, 32198, 32261, 32320, 32376, 32428, 32476, 32521, 32561,
        32599, 32632, 32662, 32688, 32711, 32729, 32744, 32755, 32763, 32767,
        32767, 32741, 32665, 32537, 32359, 32129, 31850, 31521, 31143, 30716,
        30242, 29720, 29151, 28538, 27879, 27177, 26433, 25647, 24821, 23957,
        23055, 22117, 21145, 20139, 19102, 18036, 16941, 15820, 14674, 13505,
        12315, 11106,  9879,  8637,  7381,  6114,  4838,  3554,  2264,   971
    };


    /* window for EFR, first two subframes, no lookahead */

    const Word16 window_160_80[L_WINDOW] =
    {
        2621, 2624, 2633, 2648, 2668, 2695, 2727, 2765, 2809, 2859,
        2915, 2976, 3043, 3116, 3194, 3279, 3368, 3464, 3565, 3671,
        3783, 3900, 4023, 4151, 4285, 4423, 4567, 4716, 4870, 5029,
        5193, 5362, 5535, 5714, 5897, 6084, 6277, 6473, 6674, 6880,
        7089, 7303, 7521, 7742, 7968, 8197, 8430, 8667, 8907, 9151,
        9398, 9648, 9902, 10158, 10417, 10680, 10945, 11212, 11482, 11755,
        12030, 12307, 12586, 12867, 13150, 13435, 13722, 14010, 14299, 14590,
        14882, 15175, 15469, 15764, 16060, 16356, 16653, 16950, 17248, 17546,
        17844, 18141, 18439, 18736, 19033, 19330, 19625, 19920, 20214, 20507,
        20799, 21090, 21380, 21668, 21954, 22239, 22522, 22803, 23083, 23360,
        23635, 23907, 24177, 24445, 24710, 24972, 25231, 25488, 25741, 25991,
        26238, 26482, 26722, 26959, 27192, 27422, 27647, 27869, 28087, 28300,
        28510, 28715, 28916, 29113, 29305, 29493, 29676, 29854, 30028, 30197,
        30361, 30519, 30673, 30822, 30966, 31105, 31238, 31366, 31489, 31606,
        31718, 31825, 31926, 32021, 32111, 32195, 32273, 32346, 32413, 32475,
        32530, 32580, 32624, 32662, 32695, 32721, 32742, 32756, 32765, 32767,
        32767, 32756, 32720, 32661, 32578, 32471, 32341, 32188, 32012, 31813,
        31592, 31349, 31084, 30798, 30492, 30165, 29818, 29453, 29068, 28666,
        28247, 27810, 27358, 26891, 26408, 25913, 25404, 24883, 24350, 23807,
        23255, 22693, 22124, 21548, 20965, 20378, 19786, 19191, 18593, 17994,
        17395, 16796, 16199, 15604, 15012, 14424, 13842, 13265, 12696, 12135,
        11582, 11039, 10507, 9986, 9477, 8981, 8499, 8031, 7579, 7143,
        6723, 6321, 5937, 5571, 5225, 4898, 4591, 4305, 4041, 3798,
        3577, 3378, 3202, 3048, 2918, 2812, 2729, 2669, 2633, 2621
    };

    /* window for EFR, last two subframes, no lookahead */

    const Word16 window_232_8[L_WINDOW] =
    {
        2621, 2623, 2627, 2634, 2644, 2656, 2671, 2689, 2710, 2734,
        2760, 2789, 2821, 2855, 2893, 2933, 2975, 3021, 3069, 3120,
        3173, 3229, 3288, 3350, 3414, 3481, 3550, 3622, 3697, 3774,
        3853, 3936, 4021, 4108, 4198, 4290, 4385, 4482, 4582, 4684,
        4788, 4895, 5004, 5116, 5230, 5346, 5464, 5585, 5708, 5833,
        5960, 6090, 6221, 6355, 6491, 6629, 6769, 6910, 7054, 7200,
        7348, 7498, 7649, 7803, 7958, 8115, 8274, 8434, 8597, 8761,
        8926, 9093, 9262, 9432, 9604, 9778, 9952, 10129, 10306, 10485,
        10665, 10847, 11030, 11214, 11399, 11586, 11773, 11962, 12152, 12342,
        12534, 12727, 12920, 13115, 13310, 13506, 13703, 13901, 14099, 14298,
        14497, 14698, 14898, 15100, 15301, 15504, 15706, 15909, 16112, 16316,
        16520, 16724, 16928, 17132, 17337, 17541, 17746, 17950, 18155, 18359,
        18564, 18768, 18972, 19175, 19379, 19582, 19785, 19987, 20189, 20390,
        20591, 20792, 20992, 21191, 21390, 21588, 21785, 21981, 22177, 22372,
        22566, 22759, 22951, 23143, 23333, 23522, 23710, 23897, 24083, 24268,
        24451, 24633, 24814, 24994, 25172, 25349, 25525, 25699, 25871, 26042,
        26212, 26380, 26546, 26711, 26874, 27035, 27195, 27353, 27509, 27664,
        27816, 27967, 28115, 28262, 28407, 28550, 28691, 28830, 28967, 29102,
        29234, 29365, 29493, 29619, 29743, 29865, 29985, 30102, 30217, 30330,
        30440, 30548, 30654, 30757, 30858, 30956, 31052, 31146, 31237, 31326,
        31412, 31495, 31576, 31655, 31730, 31804, 31874, 31942, 32008, 32071,
        32131, 32188, 32243, 32295, 32345, 32392, 32436, 32477, 32516, 32552,
        32585, 32615, 32643, 32668, 32690, 32709, 32726, 32740, 32751, 32759,
        32765, 32767, 32767, 32097, 30112, 26895, 22576, 17333, 11380, 4962
    };

    /*----------------------------------------------------------------------------
    ; EXTERNAL FUNCTION REFERENCES
    ; Declare functions defined elsewhere and referenced in this module
    ----------------------------------------------------------------------------*/


    /*----------------------------------------------------------------------------
    ; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
    ; Declare variables used in this module but defined elsewhere
    ----------------------------------------------------------------------------*/


    /*--------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; Define all local variables
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Function body here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; Return nothing or data or data pointer
----------------------------------------------------------------------------*/


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/get_const_tbls.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
#ifndef GET_CONST_TBLS_H
#include "get_const_tbls.h"
#endif


    extern const Word16 dgray[];
    extern const Word16 dico1_lsf_3[];
    extern const Word16 dico1_lsf_5[];
    extern const Word16 dico2_lsf_3[];
    extern const Word16 dico2_lsf_5[];
    extern const Word16 dico3_lsf_3[];
    extern const Word16 dico3_lsf_5[];
    extern const Word16 dico4_lsf_5[];
    extern const Word16 dico5_lsf_5[];
    extern const Word16 gray[];
    extern const Word16 lsp_init_data[];
    extern const Word16 mean_lsf_3[];
    extern const Word16 mean_lsf_5[];
    extern const Word16 mr515_3_lsf[];
    extern const Word16 mr795_1_lsf[];
    extern const Word16 past_rq_init[];
    extern const Word16 pred_fac_3[];
    extern const Word16 qua_gain_code[];
    extern const Word16 qua_gain_pitch[];
    extern const Word16 startPos[];
    extern const Word16 table_gain_lowrates[];
    extern const Word16 table_gain_highrates[];
    extern const Word16 prmno[];
    extern const Word16* const bitno[];
    extern const Word16 numOfBits[];
    extern const Word16* const reorderBits[];
    extern const Word16 numCompressedBytes[];
    extern const Word16 window_200_40[];
    extern const Word16 window_160_80[];
    extern const Word16 window_232_8[];
    extern const Word16 ph_imp_low_MR795[];
    extern const Word16 ph_imp_mid_MR795[];
    extern const Word16 ph_imp_low[];
    extern const Word16 ph_imp_mid[];


OSCL_EXPORT_REF void get_const_tbls(CommonAmrTbls* tbl_struct_ptr)
{
    tbl_struct_ptr->dgray_ptr = dgray;
    tbl_struct_ptr->dico1_lsf_3_ptr = dico1_lsf_3;
    tbl_struct_ptr->dico1_lsf_5_ptr = dico1_lsf_5;
    tbl_struct_ptr->dico2_lsf_3_ptr = dico2_lsf_3;
    tbl_struct_ptr->dico2_lsf_5_ptr = dico2_lsf_5;
    tbl_struct_ptr->dico3_lsf_3_ptr = dico3_lsf_3;
    tbl_struct_ptr->dico3_lsf_5_ptr = dico3_lsf_5;
    tbl_struct_ptr->dico4_lsf_5_ptr = dico4_lsf_5;
    tbl_struct_ptr->dico5_lsf_5_ptr = dico5_lsf_5;
    tbl_struct_ptr->gray_ptr = gray;
    tbl_struct_ptr->lsp_init_data_ptr = lsp_init_data;
    tbl_struct_ptr->mean_lsf_3_ptr = mean_lsf_3;
    tbl_struct_ptr->mean_lsf_5_ptr = mean_lsf_5;
    tbl_struct_ptr->mr515_3_lsf_ptr = mr515_3_lsf;
    tbl_struct_ptr->mr795_1_lsf_ptr = mr795_1_lsf;
    tbl_struct_ptr->past_rq_init_ptr = past_rq_init;
    tbl_struct_ptr->pred_fac_3_ptr = pred_fac_3;
    tbl_struct_ptr->qua_gain_code_ptr = qua_gain_code;
    tbl_struct_ptr->qua_gain_pitch_ptr = qua_gain_pitch;
    tbl_struct_ptr->startPos_ptr = startPos;
    tbl_struct_ptr->table_gain_lowrates_ptr = table_gain_lowrates;
    tbl_struct_ptr->table_gain_highrates_ptr = table_gain_highrates;
    tbl_struct_ptr->prmno_ptr = prmno;
    tbl_struct_ptr->bitno_ptr = bitno;
    tbl_struct_ptr->numOfBits_ptr = numOfBits;
    tbl_struct_ptr->reorderBits_ptr = reorderBits;
    tbl_struct_ptr->numCompressedBytes_ptr = numCompressedBytes;
    tbl_struct_ptr->window_200_40_ptr = window_200_40;
    tbl_struct_ptr->window_160_80_ptr = window_160_80;
    tbl_struct_ptr->window_232_8_ptr = window_232_8;
    tbl_struct_ptr->ph_imp_low_MR795_ptr = ph_imp_low_MR795;
    tbl_struct_ptr->ph_imp_mid_MR795_ptr = ph_imp_mid_MR795;
    tbl_struct_ptr->ph_imp_low_ptr = ph_imp_low;
    tbl_struct_ptr->ph_imp_mid_ptr = ph_imp_mid;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sqrt_l_tbl.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/****************************************************************************************
Portions of this file are derived from the following 3GPP standard:

    3GPP TS 26.073
    ANSI-C code for the Adaptive Multi-Rate (AMR) speech codec
    Available from http://www.3gpp.org

(C) 2004, 3GPP Organizational Partners (ARIB, ATIS, CCSA, ETSI, TTA, TTC)
Permission to distribute, modify and use this file under the standard license
terms listed above has been obtained from the copyright holder.
****************************************************************************************/
/*

 Filename: sqrt_l_tbl.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 This file contains the declaration for sqrt_l_table[] used by the sqrt_l_exp
 function.

    sqrt_l_tbl[i] = sqrt((i+16)*2^-6) * 2^15, i.e. sqrt(x) scaled Q15

 ------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "sqrt_l.h"

/*--------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; MACROS
    ; [Define module specific macros here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; DEFINES
    ; [Include all pre-processor statements here. Include conditional
    ; compile variables also.]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; [List function prototypes here]
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; [Variable declaration - defined here and used outside this module]
    ----------------------------------------------------------------------------*/
    const Word16 sqrt_l_tbl[50] =
    {
        16384, 16888, 17378, 17854, 18318, 18770, 19212, 19644, 20066, 20480,
        20886, 21283, 21674, 22058, 22435, 22806, 23170, 23530, 23884, 24232,
        24576, 24915, 25249, 25580, 25905, 26227, 26545, 26859, 27170, 27477,
        27780, 28081, 28378, 28672, 28963, 29251, 29537, 29819, 30099, 30377,
        30652, 30924, 31194, 31462, 31727, 31991, 32252, 32511, 32767, 32767
    };


    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME:
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    None

 Outputs:
    None

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 None

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] inv_sqrt.tab file,  UMTS GSM AMR speech codec, R99 - Version 3.2.0,
 March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
