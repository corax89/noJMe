/* ==================================================================
 * amr_math.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gmed_n.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/inv_sqrt.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2_norm.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/pow2.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sqrt_l.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/gmed_n.c
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
 Filename: gmed_n.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "gmed_n.h"
#include    "typedef.h"
#include    "oscl_mem.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here. Include conditional
; compile variables also.
----------------------------------------------------------------------------*/
#define NMAX    9   /* largest N used in median calculation */

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
 FUNCTION NAME: gmed_n
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    ind = input values (Word16)
    n = number of inputs to find the median (Word16)

 Returns:
    median value.

 Outputs:
    None.

 Global Variables Used:
    None.

 Local Variables Needed:
    None.

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function calculates N-point median of a data set. This routine is only
 valid for a odd number of gains (n <= NMAX).

------------------------------------------------------------------------------
 REQUIREMENTS

 None.

------------------------------------------------------------------------------
 REFERENCES

 gmed_n.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 gmed_n (   // o : The median value (0...N-1)
    Word16 ind[], // i : Past gain values
    Word16 n      // i : The number of gains; this routine
                  //     is only valid for a odd number of gains
                  //     (n <= NMAX)
)
{
    Word16 i, j, ix = 0;
    Word16 max;
    Word16 medianIndex;
    Word16 tmp[NMAX];
    Word16 tmp2[NMAX];

    for (i = 0; i < n; i++)
    {
        tmp2[i] = ind[i];
    }

    for (i = 0; i < n; i++)
    {
        max = -32767;
        for (j = 0; j < n; j++)
        {
            if (sub (tmp2[j], max) >= 0)
            {
                max = tmp2[j];
                ix = j;
            }
        }
        tmp2[ix] = -32768;
        tmp[i] = ix;
    }

    medianIndex=tmp[ shr(n,1) ];  // account for complex addressing
    return (ind[medianIndex]);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF Word16 gmed_n(            /* o : the median value    */
    Word16 ind[],   /* i : input values        */
    Word16 n        /* i : number of inputs    */
)
{
    register Word16 i, j, ix = 0;
    register Word16 max;
    register Word16 medianIndex;
    Word16  tmp[NMAX];
    Word16  tmp2[NMAX];

    oscl_memmove(tmp2, ind, n*sizeof(*ind));

    for (i = 0; i < n; i++)
    {
        max = -32767;
        for (j = 0; j < n; j++)
        {
            if (*(tmp2 + j) >= max)
            {
                max = *(tmp2 + j);
                ix = j;
            }
        }
        *(tmp2 + ix) = -32768;
        *(tmp + i) = ix;
    }

    medianIndex = *(tmp + (n >> 1));  /* account for complex addressing */

    return (*(ind + medianIndex));
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/inv_sqrt.c
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
 Filename: inv_sqrt.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "inv_sqrt.h"
#include    "typedef.h"
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
 FUNCTION NAME: Inv_sqrt
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_x = input value (Word32)
    pOverflow = pointer to overflow flag

 Outputs:
    pOverflow -> if the Inv_sqrt operation resulted in an overflow.

 Returns:
    L_y = inverse squareroot of L_x (Word32)

 Global Variables Used:
    None.

 Local Variables Needed:
    None.

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes 1/sqrt(L_x), where L_x is positive.
 If L_x is negative or zero, the result is 1 (3fff ffff).

 The function 1/sqrt(L_x) is approximated by a table and linear
 interpolation. The inverse square root is computed using the
 following steps:
    1- Normalization of L_x.
    2- If (30-exponent) is even then shift right once.
    3- exponent = (30-exponent)/2  +1
    4- i = bit25-b31 of L_x;  16<=i<=63  because of normalization.
    5- a = bit10-b24
    6- i -=16
    7- L_y = table[i]<<16 - (table[i] - table[i+1]) * a * 2
    8- L_y >>= exponent

------------------------------------------------------------------------------
 REQUIREMENTS

 None.

------------------------------------------------------------------------------
 REFERENCES

 inv_sqrt.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 Inv_sqrt (       // (o) : output value
    Word32 L_x          // (i) : input value
)
{
    Word16 exp, i, a, tmp;
    Word32 L_y;

* The reference ETSI code uses a global Overflow flag. In the actual
* implementation a pointer to the overflow flag is passed into the function.
* This pointer is in turn passed into the basic math functions such as add(),
* L_shl(), L_shr(), sub() called by this module.

    if (L_x <= (Word32) 0)
        return ((Word32) 0x3fffffffL);

    exp = norm_l (L_x);
    L_x = L_shl (L_x, exp);     // L_x is normalize

    exp = sub (30, exp);

    if ((exp & 1) == 0)         // If exponent even -> shift right
    {
        L_x = L_shr (L_x, 1);
    }
    exp = shr (exp, 1);
    exp = add (exp, 1);

    L_x = L_shr (L_x, 9);
    i = extract_h (L_x);        // Extract b25-b31
    L_x = L_shr (L_x, 1);
    a = extract_l (L_x);        // Extract b10-b24
    a = a & (Word16) 0x7fff;

    i = sub (i, 16);

    L_y = L_deposit_h (table[i]);       // table[i] << 16
    tmp = sub (table[i], table[i + 1]); // table[i] - table[i+1])
    L_y = L_msu (L_y, tmp, a);  // L_y -=  tmp*a*2

    L_y = L_shr (L_y, exp);     // denormalization

    return (L_y);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF Word32 Inv_sqrt(        /* (o) : output value   */
    Word32 L_x,         /* (i) : input value    */
    Flag   * pOverflow  /* (i) : pointer to overflow flag */
)
{
    Word16 exp;
    Word16 i;
    Word16 a;
    Word16 tmp;
    Word32 L_y;
    OSCL_UNUSED_ARG(pOverflow);

    if (L_x <= (Word32) 0)
    {
        return ((Word32) 0x3fffffffL);
    }

    exp = norm_l(L_x);
    L_x <<= exp;         /* L_x is normalize */

    exp = 30 - exp;

    if ((exp & 1) == 0)             /* If exponent even -> shift right */
    {
        L_x >>= 1;
    }
    exp >>= 1;
    exp += 1;

    L_x >>= 9;
    i = (Word16)(L_x >> 16);        /* Extract b25-b31 */
    a = (Word16)(L_x >> 1);         /* Extract b10-b24 */
    a &= (Word16) 0x7fff;

    i -= 16;

    L_y = (Word32)inv_sqrt_tbl[i] << 16;    /* inv_sqrt_tbl[i] << 16    */

    /* inv_sqrt_tbl[i] - inv_sqrt_tbl[i+1])  */
    tmp =  inv_sqrt_tbl[i] - inv_sqrt_tbl[i + 1];
    /* always a positive number less than 200 */

    L_y -= ((Word32)tmp * a) << 1;        /* L_y -=  tmp*a*2         */
    /* always a positive minus a small negative number */

    L_y >>= exp;                /* denormalization, exp always 0< exp < 31 */

    return (L_y);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2.c
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
 Filename: log2.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "log2.h"
#include "basic_op.h"
#include "log2_norm.h"

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


/*
------------------------------------------------------------------------------
 FUNCTION NAME: log2()
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_x = input value of type Word32
    pExponent = pointer to the integer part of Log2 of type Word16 whose
           valid range is: 0 <= value <= 30
    pFraction = pointer to the fractional part of Log2 of type Word16
           whose valid range is: 0 <= value < 1
    pOverflow = pointer to overflow flag


 Outputs:
    pExponent -> integer part of the newly calculated Log2
    pFraction -> fractional part of the newly calculated Log2
    pOverflow -> 1 if the log2() operation resulted in saturation

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes logarithm (base2) of the input L_x, where L_x is
 positive. If L_x is negative or zero, the result is 0.

 This function first normalizes the input L_x and calls the function Log2_norm
 to calculate the logarithm.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] log2.c,  UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

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
OSCL_EXPORT_REF void Log2(
    Word32 L_x,         /* (i) : input value                                */
    Word16 *pExponent,  /* (o) : Integer part of Log2.   (range: 0<=val<=30)*/
    Word16 *pFraction,  /* (o) : Fractional part of Log2. (range: 0<=val<1) */
    Flag   *pOverflow   /* (i/o) : overflow flag                            */
)
{
    Word16 exp;
    Word32 result;
    OSCL_UNUSED_ARG(pOverflow);

    exp = norm_l(L_x);
    result = L_x << exp;
    Log2_norm(result, exp, pExponent, pFraction);

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/log2_norm.c
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

 Filename: log2_norm.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "log2_norm.h"

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
 FUNCTION NAME: Log2_norm
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_x = normalized input value of type Word32
    exp = number of shifts required to normalize L_x; it is of type Word16
    exponent = pointer to the integer part of Log2 (of type Word16)
           whose valid range is: 0 <= value <= 30
    fraction = pointer to the fractional part of Log2 (of type Word16)
           whose valid range is: 0 <= value < 1

 Outputs:
    exponent points to the newly calculated integer part of Log2
    fraction points to the newly calculated fractional part of Log2

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    table = Log2 table of constants of type Word16

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 The function Log2(L_x) calculates the logarithm of the normalized input
 buffer L_x. The logarithm is approximated by a table and linear
 interpolation. The following steps are used to compute Log2(L_x):

 1. exponent = 30 - norm_exponent
 2. i = bit25-b31 of L_x;  32<=i<=63  (because of normalization).
 3. a = bit10-b24
 4. i = i - 32
 5. fraction = table[i]<<16 - (table[i] - table[i+1]) * a * 2

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 log2.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void Log2_norm (
    Word32 L_x,         // (i) : input value (normalized)
    Word16 exp,         // (i) : norm_l (L_x)
    Word16 *exponent,   // (o) : Integer part of Log2.   (range: 0<=val<=30)
    Word16 *fraction    // (o) : Fractional part of Log2. (range: 0<=val<1)
)
{
    Word16 i, a, tmp;
    Word32 L_y;

    if (L_x <= (Word32) 0)
    {
        *exponent = 0;
        *fraction = 0;
        return;
    }

    *exponent = sub (30, exp);

    L_x = L_shr (L_x, 9);
    i = extract_h (L_x);                // Extract b25-b31
    L_x = L_shr (L_x, 1);
    a = extract_l (L_x);                // Extract b10-b24 of fraction
    a = a & (Word16) 0x7fff;

    i = sub (i, 32);

    L_y = L_deposit_h (table[i]);       // table[i] << 16
    tmp = sub (table[i], table[i + 1]); // table[i] - table[i+1]
    L_y = L_msu (L_y, tmp, a);          // L_y -= tmp*a*2

    *fraction = extract_h (L_y);

    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void Log2_norm(
    Word32 L_x,         /* (i) : input value (normalized)                   */
    Word16 exp,         /* (i) : norm_l (L_x)                               */
    Word16 *exponent,   /* (o) : Integer part of Log2.   (range: 0<=val<=30)*/
    Word16 *fraction    /* (o) : Fractional part of Log2. (range: 0<=val<1) */
)
{
    Word16 i, a, tmp;
    Word32 L_y;

    if (L_x <= (Word32) 0)
    {
        *exponent = 0;
        *fraction = 0;
    }
    else
    {
        /* Calculate exponent portion of Log2 */
        *exponent = 30 - exp;

        /* At this point, L_x > 0       */
        /* Shift L_x to the right by 10 to extract bits 10-31,  */
        /* which is needed to calculate fractional part of Log2 */
        L_x >>= 10;
        i = (Word16)(L_x >> 15);    /* Extract b25-b31 */
        a = L_x & 0x7fff;           /* Extract b10-b24 of fraction */

        /* Calculate table index -> subtract by 32 is done for           */
        /* proper table indexing, since 32<=i<=63 (due to normalization) */
        i -= 32;

        /* Fraction part of Log2 is approximated by using table[]    */
        /* and linear interpolation, i.e.,                           */
        /* fraction = table[i]<<16 - (table[i] - table[i+1]) * a * 2 */
        L_y = (Word32) log2_tbl[i] << 16;  /* table[i] << 16        */
        tmp = log2_tbl[i] - log2_tbl[i + 1];  /* table[i] - table[i+1] */
        L_y -= (((Word32) tmp) * a) << 1; /* L_y -= tmp*a*2        */

        *fraction = (Word16)(L_y >> 16);
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/pow2.c
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

 Filename: pow2.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "pow2.h"
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
 FUNCTION NAME: Pow2
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    exponent = Integer part whose valid range is: 0 <= value <= 30 (Word16)
    fraction = Fractional part whose valid range is 0 <= value < 1

    pOverflow = pointer to overflow flag

 Outputs:
    L_x = Result of the Pow2() computation (Word32)
    pOverflow -> 1 if the Pow2() function results in saturation

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes  L_x = pow(2.0, exponent.fraction)

 The function Pow2(L_x) is approximated by a table and linear interpolation.

 1- i = bit10-b15 of fraction,   0 <= i <= 31
 2- a = bit0-b9   of fraction
 3- L_x = table[i]<<16 - (table[i] - table[i+1]) * a * 2
 4- L_x = L_x >> (30-exponent)     (with rounding)

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 pow2.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 Pow2 (           // (o)  : result       (range: 0<=val<=0x7fffffff)
    Word16 exponent,    // (i)  : Integer part.      (range: 0<=val<=30)
    Word16 fraction     // (i)  : Fractional part.  (range: 0.0<=val<1.0)
)
{
    Word16 exp, i, a, tmp;
    Word32 L_x;

    L_x = L_mult (fraction, 32);        // L_x = fraction<<6
    i = extract_h (L_x);                // Extract b10-b16 of fraction
    L_x = L_shr (L_x, 1);
    a = extract_l (L_x);                // Extract b0-b9   of fraction
    a = a & (Word16) 0x7fff;

    L_x = L_deposit_h (table[i]);       // table[i] << 16
    tmp = sub (table[i], table[i + 1]); // table[i] - table[i+1]
    L_x = L_msu (L_x, tmp, a);          // L_x -= tmp*a*2

    exp = sub (30, exponent);
    L_x = L_shr_r (L_x, exp);

    return (L_x);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

OSCL_EXPORT_REF Word32 Pow2(            /* (o)  : result       (range: 0<=val<=0x7fffffff) */
    Word16 exponent,    /* (i)  : Integer part.      (range: 0<=val<=30)   */
    Word16 fraction,    /* (i)  : Fractional part.  (range: 0.0<=val<1.0)  */
    Flag *pOverflow
)
{
    Word16 exp, i, a, tmp;
    Word32 L_x;

    L_x = L_mult(fraction, 32, pOverflow);      /* L_x = fraction<<6    */

    /* Extract b0-b16 of fraction */

    i = ((Word16)(L_x >> 16)) & 31;             /* ensure index i is bounded */
    a = (Word16)((L_x >> 1) & 0x7fff);

    L_x = ((Word32) pow2_tbl[i] << 16);             /* pow2_tbl[i] << 16       */

    /* pow2_tbl[i] - pow2_tbl[i+1] */
    tmp = pow2_tbl[i] - pow2_tbl[i + 1];
    L_x = L_msu(L_x, tmp, a, pOverflow);        /* L_x -= tmp*a*2        */

    exp = 30 - exponent;
    L_x = L_shr_r(L_x, exp, pOverflow);

    return (L_x);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sqrt_l.c
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

 Filename: sqrt_l.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "sqrt_l.h"
#include    "typedef.h"
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
 FUNCTION NAME: sqrt_l_exp
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_x = input value (Word32)
    pExp = pointer to right shift to be applied to result
    pOverflow = pointer to overflow flag

 Outputs:
    pOverflow -> if the Inv_sqrt operation resulted in an overflow.

 Returns:
    L_y = squareroot of L_x (Word32)

 Global Variables Used:
    None.

 Local Variables Needed:
    None.

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function computes sqrt(L_x),  where  L_x is positive.
 If L_var is negative or zero, the result is 0

 The function sqrt(L_x) is approximated by a table and linear
 interpolation. The square root is computed using the
 following steps:
    1- Normalization of L_x.
    2- If exponent is even then shift right once.
    3- exponent = exponent/2
    4- i = bit25-b31 of L_x;  16<=i<=63  because of normalization.
    5- a = bit10-b24
    6- i -=16
    7- L_y = table[i]<<16 - (table[i] - table[i+1]) * a * 2
    8- return L_y and exponent so caller can do denormalization

------------------------------------------------------------------------------
 REQUIREMENTS

 None.

------------------------------------------------------------------------------
 REFERENCES

 sqrt_l.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 sqrt_l_exp (     // o : output value
    Word32 L_x,         // i : input value
    Word16 *exp         // o : right shift to be applied to result
)
{

//          y = sqrt(x)
//          x = f * 2^-e,   0.5 <= f < 1   (normalization)
//          y = sqrt(f) * 2^(-e/2)
//
//          a) e = 2k   --> y = sqrt(f)   * 2^-k  (k = e div 2,
//                                                 0.707 <= sqrt(f) < 1)
//          b) e = 2k+1 --> y = sqrt(f/2) * 2^-k  (k = e div 2,
                                                 0.5 <= sqrt(f/2) < 0.707)


    Word16 e, i, a, tmp;
    Word32 L_y;

    if (L_x <= (Word32) 0)
    {
        *exp = 0;
        return (Word32) 0;
    }

* The reference ETSI code uses a global Overflow flag. In the actual
* implementation a pointer to the overflow flag is passed into the function.
* This pointer is in turn passed into the basic math functions such as add(),
* L_shl(), L_shr(), sub() called by this module.

    e = norm_l (L_x) & 0xFFFE;              // get next lower EVEN norm. exp
    L_x = L_shl (L_x, e);                   // L_x is normalized to [0.25..1)
    *exp = e;                               // return 2*exponent (or Q1)

    L_x = L_shr (L_x, 9);
    i = extract_h (L_x);                    // Extract b25-b31, 16 <= i <= 63
                                                because of normalization
    L_x = L_shr (L_x, 1);
    a = extract_l (L_x);                    // Extract b10-b24
    a = a & (Word16) 0x7fff;

    i = sub (i, 16);                        // 0 <= i <= 47

    L_y = L_deposit_h (table[i]);           // table[i] << 16
    tmp = sub (table[i], table[i + 1]);     // table[i] - table[i+1])
    L_y = L_msu (L_y, tmp, a);              // L_y -= tmp*a*2

    return (L_y);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

OSCL_EXPORT_REF Word32 sqrt_l_exp(      /* o : output value,          Q31 */
    Word32 L_x,         /* i : input value,                           Q31 */
    Word16 *pExp,       /* o : right shift to be applied to result,   Q1  */
    Flag   *pOverflow   /* i : pointer to overflow flag */
)

{
    Word16 e;
    Word16 i;
    Word16 a;
    Word16 tmp;
    Word32 L_y;

    /*
          y = sqrt(x)
          x = f * 2^-e,   0.5 <= f < 1   (normalization)
          y = sqrt(f) * 2^(-e/2)
          a) e = 2k   --> y = sqrt(f)   * 2^-k  (k = e div 2,
                                                 0.707 <= sqrt(f) < 1)
          b) e = 2k+1 --> y = sqrt(f/2) * 2^-k  (k = e div 2,
                                                 0.5 <= sqrt(f/2) < 0.707)
     */

    if (L_x <= (Word32) 0)
    {
        *pExp = 0;
        return (Word32) 0;
    }

    e = norm_l(L_x) & 0xFFFE;               /* get next lower EVEN norm. exp  */
    L_x = L_shl(L_x, e, pOverflow);         /* L_x is normalized to [0.25..1) */
    *pExp = e;                              /* return 2*exponent (or Q1)      */

    L_x >>= 10;
    i = (Word16)(L_x >> 15) & 63;            /* Extract b25-b31, 16<= i <=63  */
    /* because of normalization       */

    a = (Word16)(L_x);                      /* Extract b10-b24 */
    a &= (Word16) 0x7fff;

    if (i > 15)
    {
        i -= 16;                              /* 0 <= i <= 47                   */
    }

    L_y = ((Word32) sqrt_l_tbl[i] << 16);      /* sqrt_l_tbl[i] << 16            */

    /* sqrt_l_tbl[i] - sqrt_l_tbl[i+1]) */
    tmp = sqrt_l_tbl[i] - sqrt_l_tbl[i + 1];

    L_y = L_msu(L_y, tmp, a, pOverflow);    /* L_y -= tmp*a*2                 */

    /* L_y = L_shr (L_y, *exp); */          /* denormalization done by caller */

    return (L_y);
}
