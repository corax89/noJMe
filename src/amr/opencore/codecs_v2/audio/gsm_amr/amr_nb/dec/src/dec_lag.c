/* ==================================================================
 * dec_lag.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/dec_lag3.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/dec_lag6.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/dec_lag3.c
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
------------------------------------------------------------------------------



 Filename: dec_lag3.c
 Functions: Dec_lag3

 ------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    index   -- Word16 -- received pitch index
    t0_min  -- Word16 -- minimum of search range
    t0_max  -- Word16 -- maximum of search range
    i_subfr -- Word16 -- subframe flag
    T0_prev -- Word16 -- integer pitch delay of last subframe
                         used in 2nd and 4th subframes
    flag4   -- Word16 -- flag for encoding with 4 bits

 Outputs:

    T0 -- Pointer to type Word16 -- integer part of pitch lag
    T0_frac -- Pointer to type Word16 -- fractional part of pitch lag
    pOverflow -- Pointer to type Flag -- Flag set when overflow occurs

 Returns:
    None.

 Global Variables Used:
    None

 Local Variables Needed:
    None


              )
------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 PURPOSE:  Decoding of fractional pitch lag with 1/3 resolution.
           Extract the integer and fraction parts of the pitch lag from
           the received adaptive codebook index.

  See "Enc_lag3.c" for more details about the encoding procedure.

  The fractional lag in 1st and 3rd subframes is encoded with 8 bits
  while that in 2nd and 4th subframes is relatively encoded with 4, 5
  and 6 bits depending on the mode.

------------------------------------------------------------------------------
 REQUIREMENTS



------------------------------------------------------------------------------
 REFERENCES

 dec_lag3.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE



------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "dec_lag3.h"
#include "typedef.h"
#include "basic_op.h"

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

/*----------------------------------------------------------------------------
; EXTERNAL FUNCTION REFERENCES
; Declare functions defined elsewhere and referenced in this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

void Dec_lag3(Word16 index,     /* i : received pitch index                 */
              Word16 t0_min,    /* i : minimum of search range              */
              Word16 t0_max,    /* i : maximum of search range              */
              Word16 i_subfr,   /* i : subframe flag                        */
              Word16 T0_prev,   /* i : integer pitch delay of last subframe
                                       used in 2nd and 4th subframes        */
              Word16 * T0,      /* o : integer part of pitch lag            */
              Word16 * T0_frac, /* o : fractional part of pitch lag         */
              Word16 flag4,     /* i : flag for encoding with 4 bits        */
              Flag  *pOverflow  /* o : Flag set when overflow occurs        */
             )
{
    Word16 i;
    Word16 tmp_lag;

    if (i_subfr == 0)    /* if 1st or 3rd subframe */
    {

        if (index < 197)
        {

            tmp_lag = index + 2;

            tmp_lag =
                mult(
                    tmp_lag,
                    10923,
                    pOverflow);

            i = tmp_lag + 19;

            *T0 = i;

            /* i = 3 * (*T0) */

            i <<=  1;
            i += *T0;

            tmp_lag = index - i;

            *T0_frac = tmp_lag + 58;
        }
        else
        {
            *T0 = index - 112;

            *T0_frac = 0;
        }

    }
    else
    {  /* 2nd or 4th subframe */

        if (flag4 == 0)
        {

            /* 'normal' decoding: either with 5 or 6 bit resolution */

            i = index + 2;

            i = ((Word32) i * 10923) >> 15;


            i -= 1;

            *T0 = i + t0_min;

            /* i = 3* (*T0) */
            i = i + (i << 1);

            tmp_lag = index - 2;

            *T0_frac = tmp_lag - i;
        }
        else
        {

            /* decoding with 4 bit resolution */

            tmp_lag = T0_prev;

            i =
                sub(
                    tmp_lag,
                    t0_min,
                    pOverflow);

            if (i > 5)
            {
                tmp_lag = t0_min + 5;
            }

            i = t0_max - tmp_lag;

            if (i > 4)
            {
                tmp_lag = t0_max - 4;
            }

            if (index < 4)
            {
                i = tmp_lag - 5;

                *T0 = i + index;

                *T0_frac = 0;
            }
            else
            {
                /* 4 >= index < 12 */
                if (index < 12)
                {
                    i = index - 5;
                    i = ((Word32) i * 10923) >> 15;


                    i--;

                    *T0 =  i + tmp_lag;

                    i = i + (i << 1);

                    tmp_lag = index - 9;

                    *T0_frac = tmp_lag - i;
                }
                else
                {
                    i = index - 12;

                    i = i + tmp_lag;

                    *T0 = i + 1;

                    *T0_frac = 0;
                }
            }

        } /* end if (decoding with 4 bit resolution) */
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/dec_lag6.c
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
------------------------------------------------------------------------------



 Filename: dec_lag6.c
 Functions: Dec_lag6

 ------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    index   -- Word16 -- received pitch index
    pit_min  -- Word16 -- minimum pitch lag
    pit_max  -- Word16 -- maximum pitch lag
    i_subfr -- Word16 -- subframe flag
    T0 -- Pointer to type Word16 -- integer part of pitch lag

 Outputs:

    T0 -- Pointer to type Word16 -- integer part of pitch lag
    T0_frac -- Pointer to type Word16 -- fractional part of pitch lag
    pOverflow -- Pointer to type Flag -- Flag set when overflow occurs

 Returns:
    None.

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 PURPOSE:  Decoding of fractional pitch lag with 1/6 resolution.
           Extract the integer and fraction parts of the pitch lag from
           the received adaptive codebook index.

  See "Enc_lag6.c" for more details about the encoding procedure.

  The fractional lag in 1st and 3rd subframes is encoded with 9 bits
  while that in 2nd and 4th subframes is relatively encoded with 6 bits.
  Note that in relative encoding only 61 values are used. If the
  decoder receives 61, 62, or 63 as the relative pitch index, it means
  that a transmission error occurred. In this case, the pitch lag from
  previous subframe (actually from previous frame) is used.

------------------------------------------------------------------------------
 REQUIREMENTS



------------------------------------------------------------------------------
 REFERENCES

 dec_lag6.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE



------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "dec_lag6.h"
#include "typedef.h"
#include "basic_op.h"

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

/*----------------------------------------------------------------------------
; EXTERNAL FUNCTION REFERENCES
; Declare functions defined elsewhere and referenced in this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

void Dec_lag6(
    Word16 index,      /* input : received pitch index           */
    Word16 pit_min,    /* input : minimum pitch lag              */
    Word16 pit_max,    /* input : maximum pitch lag              */
    Word16 i_subfr,    /* input : subframe flag                  */
    Word16 *T0,        /* in/out: integer part of pitch lag      */
    Word16 *T0_frac,   /* output: fractional part of pitch lag   */
    Flag   *pOverflow  /* o : Flag set when overflow occurs      */
)
{
    Word16 i;
    Word16 T0_min;
    Word16 T0_max;
    Word16 k;
    (void)pOverflow; /* фиксированная точность: переполнение здесь невозможно */

    if (i_subfr == 0)          /* if 1st or 3rd subframe */
    {
        if (index < 463)
        {
            /* T0 = (index+5)/6 + 17 */
            i = index + 5;
            i = ((Word32) i * 5462) >> 15;


            i += 17;

            *T0 = i;

            /* i = 3* (*T0) */

            i <<= 1;
            i += *T0;

            /* *T0_frac = index - T0*6 + 105 */

            i <<= 1;

            i = index - i;

            *T0_frac = i + 105;
        }
        else
        {
            *T0 = index - 368;

            *T0_frac = 0;
        }
    }
    else       /* second or fourth subframe */
    {
        /* find T0_min and T0_max for 2nd (or 4th) subframe */

        T0_min = *T0 - 5;

        if (T0_min < pit_min)
        {
            T0_min = pit_min;
        }

        T0_max = T0_min + 9;

        if (T0_max > pit_max)
        {
            T0_max = pit_max;

            T0_min = T0_max - 9;
        }

        /* i = (index+5)/6 - 1 */
        i = index + 5;

        i = ((Word32) i * 5462) >> 15;


        i -= 1;

        *T0 = i + T0_min;

        /* i = 3* (*T0) */

        i = i + (i << 1);

        i <<= 1;

        k = index - 3;

        *T0_frac = k - i;
    }
}
