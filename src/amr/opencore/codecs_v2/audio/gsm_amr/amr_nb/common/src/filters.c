/* ==================================================================
 * filters.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/residu.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/syn_filt.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/weight_a.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/residu.c
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

 Filename: residu.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "residu.h"
#include "typedef.h"
#include "cnst.h"

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


/*
------------------------------------------------------------------------------
 FUNCTION NAME: Residu
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    coef_ptr = pointer to buffer containing the prediction coefficients
    input_ptr = pointer to buffer containing the speech signal
    input_len = filter order
    residual_ptr = pointer to buffer of residual signal

 Outputs:
    residual_ptr buffer contains the newly calculated the residual signal

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes the LP residual by filtering the input speech through
 the LP inverse filter A(z).

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 residu.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

 Note: Input argument names were changed to be more descriptive. Shown below
       are the original names. Shown below are the name changes:
           a[]  <-->  coef_ptr[]
           x[]  <-->  input_ptr[]
           y[]  <-->  residual_ptr[]
           lg   <-->  input_len


void Residu (
    Word16 a[], // (i)     : prediction coefficients
    Word16 x[], // (i)     : speech signal
    Word16 y[], // (o)     : residual signal
    Word16 lg   // (i)     : size of filtering
)
{
    Word16 i, j;
    Word32 s;

    for (i = 0; i < lg; i++)
    {
        s = L_mult (x[i], a[0]);
        for (j = 1; j <= M; j++)
        {
            s = L_mac (s, a[j], x[i - j]);
        }
        s = L_shl (s, 3);
        y[i] = pv_round (s);
    }
    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF void Residu(
    Word16 coef_ptr[],      /* (i)     : prediction coefficients*/
    Word16 input_ptr[],     /* (i)     : speech signal          */
    Word16 residual_ptr[],  /* (o)     : residual signal        */
    Word16 input_len        /* (i)     : size of filtering      */
)
{


    register Word16 i, j;
    Word32 s1;
    Word32 s2;
    Word32 s3;
    Word32 s4;
    Word16 *p_input1;
    Word16 *p_input2;
    Word16 *p_input3;
    Word16 *p_input4;
    Word16 *p_coef;
    Word16 *p_residual_ptr = &residual_ptr[input_len-1];
    Word16 *p_input_ptr    = &input_ptr[input_len-1-M];

    for (i = input_len >> 2; i != 0; i--)
    {
        s1       = 0x0000800L;
        s2       = 0x0000800L;
        s3       = 0x0000800L;
        s4       = 0x0000800L;
        p_coef  = &coef_ptr[M];
        p_input1 = p_input_ptr--;
        p_input2 = p_input_ptr--;
        p_input3 = p_input_ptr--;
        p_input4 = p_input_ptr--;

        for (j = M >> 1; j != 0; j--)
        {
            s1 += ((Word32) * (p_coef) * *(p_input1++));
            s2 += ((Word32) * (p_coef) * *(p_input2++));
            s3 += ((Word32) * (p_coef) * *(p_input3++));
            s4 += ((Word32) * (p_coef--) * *(p_input4++));
            s1 += ((Word32) * (p_coef) * *(p_input1++));
            s2 += ((Word32) * (p_coef) * *(p_input2++));
            s3 += ((Word32) * (p_coef) * *(p_input3++));
            s4 += ((Word32) * (p_coef--) * *(p_input4++));
        }

        s1 += (((Word32) * (p_coef)) * *(p_input1));
        s2 += (((Word32) * (p_coef)) * *(p_input2));
        s3 += (((Word32) * (p_coef)) * *(p_input3));
        s4 += (((Word32) * (p_coef)) * *(p_input4));

        *(p_residual_ptr--) = (Word16)(s1 >> 12);
        *(p_residual_ptr--) = (Word16)(s2 >> 12);
        *(p_residual_ptr--) = (Word16)(s3 >> 12);
        *(p_residual_ptr--) = (Word16)(s4 >> 12);

    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/syn_filt.c
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
 Filename: syn_filt.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "syn_filt.h"
#include    "cnst.h"
#include    "basic_op.h"
#include    "oscl_mem.h"

#include    "basic_op.h"

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


/*
------------------------------------------------------------------------------
 FUNCTION NAME: Syn_filt
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    a = buffer containing the prediction coefficients (Word16)  max 2^12
    x = input signal buffer (Word16)                            max 2^15
    y = output signal buffer (Word16)
    lg = size of filtering (Word16)
    mem = memory buffer associated with this filtering (Word16)
    update = flag to indicate memory update; 0=no update, 1=update memory
             (Word16)

 Outputs:
    mem buffer is changed to be the last M data points of the output signal
      if update was set to 1
    y buffer contains the newly calculated filter output

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 Perform synthesis filtering through 1/A(z)

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 syn_filt.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void Syn_filt (
    Word16 a[],     // (i)     : a[M+1] prediction coefficients   (M=10)
    Word16 x[],     // (i)     : input signal
    Word16 y[],     // (o)     : output signal
    Word16 lg,      // (i)     : size of filtering
    Word16 mem[],   // (i/o)   : memory associated with this filtering.
    Word16 update   // (i)     : 0=no update, 1=update of memory.
)
{
    Word16 i, j;
    Word32 s;
    Word16 tmp[80];   // This is usually done by memory allocation (lg+M)
    Word16 *yy;

    // Copy mem[] to yy[]

    yy = tmp;

    for (i = 0; i < M; i++)
    {
        *yy++ = mem[i];
    }

    // Do the filtering.

    for (i = 0; i < lg; i++)
    {
        s = L_mult (x[i], a[0]);
        for (j = 1; j <= M; j++)
        {
            s = L_msu (s, a[j], yy[-j]);
        }
        s = L_shl (s, 3);
        *yy++ = pv_round (s);
    }

    for (i = 0; i < lg; i++)
    {
        y[i] = tmp[i + M];
    }

    // Update of memory if update==1

    if (update != 0)
    {
        for (i = 0; i < M; i++)
        {
            mem[i] = y[lg - M + i];
        }
    }
    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF void Syn_filt(
    Word16 a[],     /* (i)   : a[M+1] prediction coefficients   (M=10)  */
    Word16 x[],     /* (i)   : input signal                             */
    Word16 y[],     /* (o)   : output signal                            */
    Word16 lg,      /* (i)   : size of filtering   (40)                 */
    Word16 mem[],   /* (i/o) : memory associated with this filtering.   */
    Word16 update   /* (i)   : 0=no update, 1=update of memory.         */
)
{
    Word16 i, j;
    Word32 s1;
    Word32 s2;
    Word16 tmp[2*M]; /* This is usually done by memory allocation (lg+M) */
    Word16 *yy;

    Word16 *p_a;
    Word16 *p_yy1;
    Word16 *p_y;
    Word16 *p_x;
    Word16 temp;
    /* Copy mem[] to yy[] */

    yy = tmp;

    oscl_memcpy(yy, mem, M*sizeof(Word16));

    yy = yy + M;

    /* Do the filtering. */

    p_y  = y;
    p_x  = x;
    p_yy1 = &yy[-1];

    for (i = M >> 1; i != 0; i--)
    {
        p_a  = a;

        s1 = amrnb_fxp_mac_16_by_16bb((Word32) * (p_x++), (Word32) * (p_a), 0x00000800L);
        s2 = amrnb_fxp_mac_16_by_16bb((Word32) * (p_x++), (Word32) * (p_a++), 0x00000800L);
        s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);

        for (j = (M >> 1) - 2; j != 0; j--)
        {
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
        }

        /* check for overflow on s1 */
        if ((UWord32)(s1 + 134217728) < 0x0fffffffL)
        {
            temp = (Word16)(s1 >> 12);
        }
        else if (s1 > 0x07ffffffL)
        {
            temp = MAX_16;
        }
        else
        {
            temp = MIN_16;
        }

        s2 = amrnb_fxp_msu_16_by_16bb((Word32)a[1], (Word32)temp, s2);

        *(yy++)  = temp;
        *(p_y++) = temp;

        p_yy1 = yy;

        /* check for overflow on s2 */
        if ((UWord32)(s2 + 134217728) < 0x0fffffffL)
        {
            temp = (Word16)(s2 >> 12);
        }
        else if (s2 > 0x07ffffffL)
        {
            temp = MAX_16;
        }
        else
        {
            temp = MIN_16;
        }

        *(yy++)  = temp;
        *(p_y++) = temp;
    }

    p_yy1 = &y[M-1];

    for (i = (lg - M) >> 1; i != 0; i--)
    {
        p_a  = a;

        s1 = amrnb_fxp_mac_16_by_16bb((Word32) * (p_x++), (Word32) * (p_a), 0x00000800L);
        s2 = amrnb_fxp_mac_16_by_16bb((Word32) * (p_x++), (Word32) * (p_a++), 0x00000800L);
        s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);

        for (j = (M >> 1) - 2; j != 0; j--)
        {
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
            s2 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a), (Word32) * (p_yy1--), s2);
            s1 = amrnb_fxp_msu_16_by_16bb((Word32) * (p_a++), (Word32) * (p_yy1), s1);
        }

        if ((UWord32)(s1 + 134217728) < 0x0fffffffL)
        {
            temp = (Word16)(s1 >> 12);
        }
        else if (s1 > 0x07ffffffL)
        {
            temp = MAX_16;
        }
        else
        {
            temp = MIN_16;
        }

        s2 = amrnb_fxp_msu_16_by_16bb((Word32)a[1], (Word32)temp, s2);

        *(p_y++) = temp;
        p_yy1 = p_y;

        if ((UWord32)(s2 + 134217728) < 0x0fffffffL)
        {
            *(p_y++) = (Word16)(s2 >> 12);
        }
        else if (s2 > 0x07ffffffL)
        {
            *(p_y++) = MAX_16;
        }
        else
        {
            *(p_y++) = MIN_16;
        }
    }

    /* Update of memory if update==1 */
    if (update != 0)
    {
        oscl_memcpy(mem, &y[lg-M], M*sizeof(Word16));
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/weight_a.c
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

 Filename: weight_a.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "weight_a.h"
#include    "typedef.h"
#include    "cnst.h"

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


/*
------------------------------------------------------------------------------
 FUNCTION NAME: Weight_Ai
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    a = LPC coefficients (Word16)
    fac = Spectral expansion factors (Word16)
    a_exp = Spectral expanded LPC coefficients (Word16)

 Outputs:
    a_exp points to the updated spectral expanded LPC coefficients

 Returns:
    None.

 Global Variables Used:
    None.

 Local Variables Needed:
    None.

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function calculates the spectral expansion for the LP coefficients of
 order M.
    a_exp[i] = a[i] * fac[i-1]    ; i=1..M

------------------------------------------------------------------------------
 REQUIREMENTS

 None.

------------------------------------------------------------------------------
 REFERENCES

 weight_a.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void Weight_Ai (
    Word16 a[],         // (i)     : a[M+1]  LPC coefficients   (M=10)
    const Word16 fac[], // (i)     : Spectral expansion factors.
    Word16 a_exp[]      // (o)     : Spectral expanded LPC coefficients
)
{
    Word16 i;
    a_exp[0] = a[0];

    for (i = 1; i <= M; i++)
    {
        a_exp[i] = pv_round (L_mult (a[i], fac[i - 1]));
    }
    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF void Weight_Ai(
    Word16 a[],         /* (i)   : a[M+1]  LPC coefficients   (M=10)    */
    const Word16 fac[], /* (i)   : Spectral expansion factors.          */
    Word16 a_exp[]      /* (o)   : Spectral expanded LPC coefficients   */
)
{
    register Word16 i;

    *(a_exp) = *(a);

    for (i = M; i >= 1; i--)
    {
        a_exp += 1;
        a += 1;
        fac += 1;
        *(a_exp) = (Word16)((((Word32) * (a)) * *(fac - 1)
                             + 0x00004000L) >> 15);
    }

    return;
}
