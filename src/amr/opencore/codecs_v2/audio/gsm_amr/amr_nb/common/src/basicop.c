/* ==================================================================
 * basicop.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/add.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sub.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/mult_r.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/extract_h.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/extract_l.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_abs.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_deposit_h.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_deposit_l.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_shr_r.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/norm_l.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/norm_s.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/round.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/shr.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/shr_r.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/set_zero.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/div_s.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/div_32.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/add.c
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

 Filename: add.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 Summation function with overflow control

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: add
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the add operation resulted in overflow

 Returns:
    sum = 16-bit limited sum of var1 and var2 (Word16)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function performs the addition (var1+var2) with overflow control and
 saturation; the 16 bit result is set at +32767 when overflow occurs or at
 -32768 when underflow occurs.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] add.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

 Word16 add (Word16 var1, Word16 var2)
{
    Word16 var_out;
    Word32 sum;

    sum = (Word32) var1 + var2;

* The reference ETSI code uses a global flag for Overflow inside the function
* saturate(). In the actual implementation a pointer to Overflow flag is passed in
* as a parameter to the function

    var_out = saturate (sum);
#if (WMOPS)
    multiCounter[currCounter].add++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
OSCL_EXPORT_REF Word16 add_16(Word16 var1, Word16 var2, Flag *pOverflow)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/
    Word32 sum;
    sum = (Word32) var1 + var2;

    /* Saturate result (if necessary). */
    /* Replaced function call with in-line code             */
    /* to conserve MIPS, i.e., var_out = saturate (sum)  */

    if (sum > 0X00007fffL)
    {
        *pOverflow = 1;
        sum = MAX_16;
    }
    else if (sum < (Word32) - 32768)
    {
        *pOverflow = 1;
        sum = MIN_16;
    }

    /* Return the sum as a 16 bit value by type casting Word32 to Word16 */

    return ((Word16) sum);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/sub.c
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

 Filename: sub.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 Subtraction function with overflow control

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: sub
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the subtract operation resulted in overflow

 Returns:
    diff = 16-bit limited difference between var1 and var2 (Word16)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function performs the subtraction (var1-var2) with overflow control and
 saturation; the 16 bit result is set at +32767 when overflow occurs or at
 -32768 when underflow occurs.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] sub() function in basicop2.c, UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------

 PSEUDO-CODE

 Word16 sub (Word16 var1, Word16 var2)
 {
    Word16 var_out;
    Word32 diff;

    diff = (Word32) var1 - var2;

* The reference ETSI code uses a global flag for Overflow inside the function
* saturate(). In the actual implementation a pointer to Overflow flag is passed
* in as a parameter to the function

    var_out = saturate (diff);

 #if (WMOPS)
    multiCounter[currCounter].sub++;
 #endif

    return (var_out);
 }

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

OSCL_EXPORT_REF Word16 sub(Word16 var1, Word16 var2, Flag *pOverflow)
{

    Word32 diff;

    diff = (Word32) var1 - var2;

    /* Saturate result (if necessary). */
    /* Replaced function call with in-line code             */
    /*  to conserve MIPS, i.e., var_out = saturate (diff)  */


    if ((UWord32)(diff + 32768) > 0x000FFFF)
    {
        if (diff > (Word32) 0x0007FFFL)
        {
            diff = MAX_16;
        }
        else
        {
            diff = MIN_16;
        }

        *pOverflow = 1;
    }


    return ((Word16) diff);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/mult_r.c
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

 Filename: mult_r.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 Multiplication function with rounding and overflow control

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: mult_r
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the add operation resulted in overflow

 Returns:
    L_product_arr = 16-bit limited product of var1 and var2 (Word16)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function performs the multiplication of var1 by var2 with rounding, and
 gives a 16 bit result which is scaled, i.e.:
    mult_r(var1,var2) = extract_l(L_shr(((var1 * var2) + 16384),15)) and  |
    mult_r(-32768,-32768) = 32767

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] mult_r() function in basicop2.c, UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 mult_r (Word16 var1, Word16 var2)
{
    Word16 var_out;
    Word32 L_product_arr;

    L_product_arr = (Word32) var1 *(Word32) var2;
    L_product_arr += (Word32) 0x00004000L;
    L_product_arr &= (Word32) 0xffff8000L;
    L_product_arr >>= 15;

    if (L_product_arr & (Word32) 0x00010000L)
    {
        L_product_arr |= (Word32) 0xffff0000L;
    }
* The reference ETSI code uses a global flag for Overflow inside the function
* saturate(). In the actual implementation a pointer to Overflow flag is passed in
* as a parameter to the function

    var_out = saturate (L_product_arr);

#if (WMOPS)
    multiCounter[currCounter].mult_r++;
#endif

    return (var_out);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/

OSCL_EXPORT_REF Word16 mult_r(Word16 var1, Word16 var2, Flag *pOverflow)
{

    register Word32 L_product_arr;

    L_product_arr = ((Word32) var1) * var2;              /* product */
    L_product_arr += (Word32) 0x00004000L;               /* round */
    L_product_arr >>= 15;                                /* shift */

    /* sign extend when necessary */
    L_product_arr |= (Word32) - (L_product_arr & (Word32) 0x00010000L);

    /* Saturate result (if necessary). */
    /* Replaced function call with in-line code to conserve MIPS, */
    /* i.e., var_out = saturate (L_product_arr)  */

    if (L_product_arr > 0X00007fffL)
    {
        *pOverflow = 1;
        L_product_arr = MAX_16;
    }
    else if (L_product_arr < (Word32) - 32768)
    {
        *pOverflow = 1;
        L_product_arr = MIN_16;
    }

    return ((Word16) L_product_arr);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/extract_h.c
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

 Filename: extract_h.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit long signed integer (Word32 ) whose value falls
             in the range : 0x8000 0000 <= L_var1 <= 0x7fff ffff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    L_var1 = Most significant word of input (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function returns the 16 MSB of the input, L_var1.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 extract_h (Word32 L_var1)
{
    Word16 var_out;

    var_out = (Word16) (L_var1 >> 16);
#if (WMOPS)
    multiCounter[currCounter].extract_h++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
Word16 extract_h(Word32 L_var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return ((Word16)(L_var1 >> 16));
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/extract_l.c
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
 Filename: extract_l.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit long signed integer (Word32 ) whose value falls
             in the range : 0x8000 0000 <= L_var1 <= 0x7fff ffff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    L_var1 = Most significant word of input (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function returns the 16 LSB of the input, L_var1.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 extract_l (Word32 L_var1)
{
    Word16 var_out;

    var_out = (Word16) L_var1;
#if (WMOPS)
    multiCounter[currCounter].extract_l++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
Word16 extract_l(Word32 L_var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return ((Word16) L_var1);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_abs.c
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
 Filename: l_abs.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit long signed integer (Word32 ) whose value falls
             in the range : 0x8000 0000 <= L_var1 <= 0x7fff ffff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    L_var1 = absolute value of input (Word32)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function calculates the absolute value of L_var1; saturate in case
 where the input is -214783648.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 L_abs (Word32 L_var1)
{
    Word32 L_var_out;

    if (L_var1 == MIN_32)
    {
        L_var_out = MAX_32;
    }
    else
    {
        if (L_var1 < 0)
        {
            L_var_out = -L_var1;
        }
        else
        {
            L_var_out = L_var1;
        }
    }

#if (WMOPS)
    multiCounter[currCounter].L_abs++;
#endif
    return (L_var_out);
}


------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
Word32 L_abs(register Word32 L_var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    Word32 y = L_var1 - (L_var1 < 0);
    y = y ^(y >> 31);
    return (y);

}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_deposit_h.c
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
 Filename: l_deposit_h.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var1 = deposit of var1 into MSWord of 32 bit value (Word32)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function deposits the 16 bit var1 into the 16 MS bits of the 32 bit
 output. The 16 LS bits of the output are zeroed.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 L_deposit_h (Word16 var1)
{
    Word32 L_var_out;

    L_var_out = (Word32) var1 << 16;
#if (WMOPS)
    multiCounter[currCounter].L_deposit_h++;
#endif
    return (L_var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
Word32 L_deposit_h(Word16 var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return ((Word32) var1 << 16);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_deposit_l.c
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
 Filename: l_deposit_l.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var1 = deposit of var1 into LSWord of 32 bit value (Word32)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function deposits the 16 bit var1 into the 16 LS bits of the 32 bit
 output. The 16 MS bits of the output are sign extended.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 L_deposit_l (Word16 var1)
{
    Word32 L_var_out;

    L_var_out = (Word32) var1;
#if (WMOPS)
    multiCounter[currCounter].L_deposit_l++;
#endif
    return (L_var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
Word32 L_deposit_l(Word16 var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return ((Word32) var1);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/l_shr_r.c
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
 Filename: l_shr_r.c

*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: L_shr_r
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit long signed integer (Word32 ) whose value falls
             in the range : 0x8000 0000 <= L_var1 <= 0x7fff ffff.
    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the 32 bit shift operation resulted in overflow

 Returns:
    result = Shifted result w/ rounding (Word32)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function arithmetically shifts the 32 bit input L_var1 right var2
 positions with rounding. If var2 is negative, the function
 arithmetically shifts L_var1 left by -var2 and zero fills the -var2 LSB of
 the result. The result is saturated in case of underflows or overflows, i.e.,

 - If var2 is greater than zero :
    if (L_sub(L_shl(L_shr(L_var1,var2),1),L_shr(L_var1,sub(var2,1))))
    is equal to zero
    then
        L_shr_r(L_var1,var2) = L_shr(L_var1,var2)
    else
        L_shr_r(L_var1,var2) = L_add(L_shr(L_var1,var2),1)
 - If var2 is less than or equal to zero :
    L_shr_r(L_var1,var2) = L_shr(L_var1,var2).

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] L_shr_r() function in basic_op2.c,  UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word32 L_shr_r (Word32 L_var1, Word16 var2)
{
    Word32 L_var_out;

* The reference ETSI code uses a global flag for Overflow. In the actual
* implementation a pointer to Overflow flag is passed in as a parameter to the
* function L_shr()

    if (var2 > 31)
    {
        L_var_out = 0;
    }
    else
    {
        L_var_out = L_shr (L_var1, var2);
#if (WMOPS)
        multiCounter[currCounter].L_shr--;
#endif
        if (var2 > 0)
        {
            if ((L_var1 & ((Word32) 1 << (var2 - 1))) != 0)
            {
                L_var_out++;
            }
        }
    }
#if (WMOPS)
    multiCounter[currCounter].L_shr_r++;
#endif
    return (L_var_out);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
OSCL_EXPORT_REF Word32 L_shr_r(register Word32 L_var1, register Word16 var2, Flag *pOverflow)
{
    Word32 result;

    if (var2 > 31)
    {
        result = 0;
    }
    else
    {
        result = L_shr(L_var1, var2, pOverflow);

        if (var2 > 0)
        {
            if ((L_var1 & ((Word32) 1 << (var2 - 1))) != 0)
            {
                result++;
            }
        }
    }
    return (result);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/norm_l.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2010 PacketVideo
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
 Filename: norm_l.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit long signed integer (Word32) whose value falls
             in the range : 0x8000 0000 <= var1 <= 0x7fff ffff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var_out = number of left shifts need to normalize input (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function produces the number of left shifts needed to normalize the 32
 bit variable L_var1 for positive values on the interval with minimum of
 0x40000000 and maximum of 0x7fffffff, and for negative values on the interval
 with minimum of 0x80000000 and maximum of 0xc0000000. Note that when L_var1
 is equal to zero, the output var_out is set to zero.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 norm_l (Word32 L_var1)
{
    Word16 var_out;

    if (L_var1 == 0)
    {
        var_out = 0;
    }
    else
    {
        if (L_var1 == (Word32) 0xffffffffL)
        {
            var_out = 31;
        }
        else
        {
            if (L_var1 < 0)
            {
                L_var1 = ~L_var1;
            }
            for (var_out = 0; L_var1 < (Word32) 0x40000000L; var_out++)
            {
                L_var1 <<= 1;
            }
        }
    }

#if (WMOPS)
    multiCounter[currCounter].norm_l++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
#if !((PV_CPU_ARCH_VERSION >=5) && ((PV_COMPILER == EPV_ARM_GNUC) || (PV_COMPILER == EPV_ARM_RVCT)))
OSCL_EXPORT_REF Word16 norm_l(register Word32 L_var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/
    register Word16 var_out = 0;

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    if (L_var1)
    {

        Word32 y = L_var1 - (L_var1 < 0);
        L_var1 = y ^(y >> 31);


        while (!(0x40000000L & L_var1))
        {
            var_out++;
            if ((0x20000000L & L_var1))
            {
                break;
            }
            var_out++;
            if ((0x10000000L & L_var1))
            {
                break;
            }
            var_out++;
            if ((0x08000000L & L_var1))
            {
                break;
            }
            var_out++;
            L_var1 <<= 4;
        }
    }

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/


    return (var_out);
}
#endif


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/norm_s.c
 * ==================================================================
 */

/* ------------------------------------------------------------------
 * Copyright (C) 1998-2010 PacketVideo
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

 Filename: norm_s.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit signed integer of type Word16, whose value falls
           in the range: 0x8000 <= var1 <= 0x7fff

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var_out = number of left shifts need to normalize var1 (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function produces the number of left shifts needed to normalize the 16
 bit variable var1 for positive values on the interval with minimum of 0x4000
 and maximum of 0x7fff, and for negative values on the interval with minimum
 of 0x8000 and maximum of 0xc000. Note that when var1 is zero, the resulting
 output var_out is set to zero.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 norm_s (Word16 var1)
{
    Word16 var_out;

    if (var1 == 0)
    {
        var_out = 0;
    }
    else
    {
        if (var1 == (Word16) 0xffff)
        {
            var_out = 15;
        }
        else
        {
            if (var1 < 0)
            {
                var1 = ~var1;
            }
            for (var_out = 0; var1 < 0x4000; var_out++)
            {
                var1 <<= 1;
            }
        }
    }

#if (WMOPS)
    multiCounter[currCounter].norm_s++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
#if !((PV_CPU_ARCH_VERSION >=5) && ((PV_COMPILER == EPV_ARM_GNUC) || (PV_COMPILER == EPV_ARM_RVCT)))

OSCL_EXPORT_REF Word16 norm_s(register Word16 var1)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/

    register Word16 var_out = 0;

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/

    if (var1)
    {
        Word16 y = var1 - (var1 < 0);
        var1 = y ^(y >> 15);

        while (!(0x4000 & var1))
        {
            var_out++;
            if ((0x2000 & var1))
            {
                break;
            }
            var_out++;
            if ((0x1000 & var1))
            {
                break;
            }
            var_out++;
            if ((0x0800 & var1))
            {
                break;
            }
            var_out++;
            var1 <<= 4;
        }
    }

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return (var_out);
}

#endif


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/round.c
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

 Filename: round.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

 Rounding function with saturation.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: pv_round
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_var1 = 32 bit signed integer (Word32) whose value falls
             in the range : 0x8000 0000 <= L_var1 <= 0x7fff ffff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    None

 Returns:
        result = MS 16 bits of rounded input L_var1.

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function rounds the lower 16 bits of the 32 bit input number into the
 MS 16 bits with saturation. Shift the resulting bits right by 16 and return
 the 16 bit number:
    pv_round(L_var1) = extract_h(L_add(L_var1,32768))

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] round() function in basic_op2.c,  UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 pv_round (Word32 L_var1)
{
    Word16 var_out;
    Word32 L_rounded;

* The reference ETSI code uses a global flag for Overflow in the L_add() function.
* In the actual implementation a pointer to Overflow flag is passed in as a
* parameter to the function.

    L_rounded = L_add (L_var1, (Word32) 0x00008000L);
#if (WMOPS)
    multiCounter[currCounter].L_add--;
#endif
    var_out = extract_h (L_rounded);
#if (WMOPS)
    multiCounter[currCounter].extract_h--;
    multiCounter[currCounter].round++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
OSCL_EXPORT_REF Word16 pv_round(register Word32 L_var1, Flag *pOverflow)
{
    Word16  result;

    L_var1 = L_add(L_var1, (Word32) 0x00008000L, pOverflow);
    result = (Word16)(L_var1 >> 16);

    return (result);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/shr.c
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

 Filename: shr.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION
 Shift right function with overflow control
------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: shr
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.

    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the shift operation resulted in overflow

 Returns:
    product = Shifted result limited to 16 bits (Word16)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function arithmetically shifts the 16 bit input var1 right var2 positions
 with sign extension. If var2 is negative, arithmetically shift var1 left by
 -var2 with sign extension. Saturate the result in case of underflows or
 overflows.

------------------------------------------------------------------------------
 REQUIREMENTS
 None
------------------------------------------------------------------------------
 REFERENCES

 [1] shr() function in basic_op2.c,  UMTS GSM AMR speech codec, R99 -
 Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 shr_std (Word16 var1, Word16 var2)
{
    Word16 var_out;

    if (var2 < 0)
    {
        if (var2 < -16)
            var2 = -16;
        var_out = shl_std (var1, -var2);
#if (WMOPS)
        mult_stdiCounter[currCounter].shl_std--;
#endif
    }
    else
    {
        if (var2 >= 15)
        {
            var_out = (var1 < 0) ? -1 : 0;
        }
        else
        {
            if (var1 < 0)
            {
                var_out = ~((~var1) >> var2);
            }
            else
            {
                var_out = var1 >> var2;
            }
        }
    }

#if (WMOPS)
    mult_stdiCounter[currCounter].shr_std++;
#endif
    return (var_out);
}
------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
OSCL_EXPORT_REF Word16 shr(register Word16 var1, register Word16 var2, Flag *pOverflow)
{
    register Word16 result;

    if (var2 != 0)
    {
        if (var2 > 0)
        {
            if (var2 > 15)
            {
                var2 = 15;
            }

            result = var1 >> var2;

        }
        else
        {
            var2 = -var2;   /* Shift right negative is equivalent */

            if (var2 > 15)
            {
                var2 = 15;
            }

            result = (var1 << var2);
            if ((result >> var2) != var1)
            {
                *pOverflow = 1;
                result = ((var1 > 0) ? MAX_16 : MIN_16);
            }
        }

    }
    else
    {
        result = var1;
    }

    return (result);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/shr_r.c
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

 Filename: shr_r.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var1 <= 0x0000 7fff.
    var2 = 16 bit short signed integer (Word16) whose value falls in
           the range : 0xffff 8000 <= var2 <= 0x0000 7fff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var_out = shifted input w/ rounding (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function arithmetically shifts the 16 bit input var1 right var2 positions
 with rounding. If var2 is negative, arithmetically shift var1 left by
 -var2 with rounding. Saturate the result in case of underflows or
 overflows.

    - If var2 is greater than zero :
        if (sub(shl(shr(var1,var2),1),shr(var1,sub(var2,1))))
        is equal to zero
        then
            shr_r(var1,var2) = shr(var1,var2)
        else
            shr_r(var1,var2) = add(shr(var1,var2),1)
    - If var2 is less than or equal to zero :
        shr_r(var1,var2) = shr(var1,var2).

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 shr_r (Word16 var1, Word16 var2)
{
    Word16 var_out;

    if (var2 > 15)
    {
        var_out = 0;
    }
    else
    {
        var_out = shr (var1, var2);
#if (WMOPS)
        multiCounter[currCounter].shr--;
#endif

        if (var2 > 0)
        {
            if ((var1 & ((Word16) 1 << (var2 - 1))) != 0)
            {
                var_out++;
            }
        }
    }
#if (WMOPS)
    multiCounter[currCounter].shr_r++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
OSCL_EXPORT_REF Word16 shr_r(register Word16 var1, register Word16 var2, Flag *pOverflow)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/
    Word16 var_out;

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/
    if (var2 > 15)
    {
        var_out = 0;
    }
    else
    {
        var_out = shr(var1, var2, pOverflow);
        if (var2 > 0)
        {
            if ((var1 & ((Word16) 1 << (var2 - 1))) != 0)
            {
                var_out++;
            }
        }
    }

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return (var_out);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/set_zero.c
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
********************************************************************************
*      GSM AMR-NB speech codec   R98   Version 7.5.0   March 2, 2001
*                                R99   Version 3.2.0
*                                REL-4 Version 4.0.0
*
********************************************************************************
*
*      File             : set_zero.h
*
********************************************************************************
*/
/*
********************************************************************************
*                         MODULE INCLUDE FILE AND VERSION ID
********************************************************************************
*/
#include "set_zero.h"

/*
********************************************************************************
*                         INCLUDE FILES
********************************************************************************
*/
#include "typedef.h"
#include "basic_op.h"

/*
********************************************************************************
*                         PUBLIC PROGRAM CODE
********************************************************************************
*/
void Set_zero(
    Word16 x[],         /* (o)    : vector to clear     */
    Word16 L            /* (i)    : length of vector    */
)
{
    Word16 i;

    for (i = 0; i < L; i++)
    {
        x[i] = 0;
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/div_s.c
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
 Filename: div_s.c

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    var1 = 16 bit signed integer (Word16) whose value falls in
           the range : 0x0000 <= var1 <= 0x7fff.
    var2 = 16 bit signed integer (Word16) whose value falls in
           the range : 0x0000 <= var1 <= 0x7fff.

 Local Stores/Buffers/Pointers Needed:
    None

 Global Stores/Buffers/Pointers Needed:
    None

 Outputs:
    var_out = quotient of var1 divided by var2 (Word16)

 Pointers and Buffers Modified:
    None

 Local Stores Modified:
    None

 Global Stores Modified:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function produces a result which is the fractional integer division of
 var1 by var2; var1 and var2 must be positive and var2 must be greater or equal
 to var1; the result is positive (leading bit equal to 0) and truncated to 16
 bits. If var1 = var2 then div(var1,var2) = 32767.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] basicop2.c, ETS Version 2.0.0, February 8, 1999

------------------------------------------------------------------------------
 PSEUDO-CODE

Word16 div_s (Word16 var1, Word16 var2)
{
    Word16 var_out = 0;
    Word16 iteration;
    Word32 L_num;
    Word32 L_denom;
    Word16 abort_flag = 0;

    if ((var1 > var2) || (var1 < 0))
    {
        printf ("Division Error var1=%d  var2=%d\n", var1, var2);
        abort_flag = 1;
        exit(0);
    }
    if ((var1 != 0) && (abort_flag == 0))
    {
        if (var1 == var2)
        {
            var_out = MAX_16;
        }
        else
        {
            L_num = (Word32) var1;
            L_denom = (Word32) var2;

            for (iteration = 15; iteration > 0; iteration--)
            {
                var_out <<= 1;
                L_num <<= 1;

                if (L_num >= L_denom)
                {
                    L_num -= L_denom;
                    var_out += 1;
                }
            }
        }
    }

#if (WMOPS)
    multiCounter[currCounter].div_s++;
#endif
    return (var_out);
}

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
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
OSCL_EXPORT_REF Word16 div_s(register Word16 var1, register Word16 var2)
{
    /*----------------------------------------------------------------------------
    ; Define all local variables
    ----------------------------------------------------------------------------*/
    Word16 var_out = 0;
    register Word16 iteration;
    Word32 L_num;
    Word32 L_denom;
    Word32 L_denom_by_2;
    Word32 L_denom_by_4;

    /*----------------------------------------------------------------------------
    ; Function body here
    ----------------------------------------------------------------------------*/
    if ((var1 > var2) || (var1 < 0))
    {
        return 0; // used to exit(0);
    }
    if (var1)
    {
        if (var1 != var2)
        {

            L_num = (Word32) var1;
            L_denom = (Word32) var2;
            L_denom_by_2 = (L_denom << 1);
            L_denom_by_4 = (L_denom << 2);
            for (iteration = 5; iteration > 0; iteration--)
            {
                var_out <<= 3;
                L_num   <<= 3;

                if (L_num >= L_denom_by_4)
                {
                    L_num -= L_denom_by_4;
                    var_out |= 4;
                }

                if (L_num >= L_denom_by_2)
                {
                    L_num -= L_denom_by_2;
                    var_out |=  2;
                }

                if (L_num >= (L_denom))
                {
                    L_num -= (L_denom);
                    var_out |=  1;
                }

            }
        }
        else
        {
            var_out = MAX_16;
        }
    }

#if (WMOPS)
    multiCounter[currCounter].div_s++;
#endif

    /*----------------------------------------------------------------------------
    ; Return nothing or data or data pointer
    ----------------------------------------------------------------------------*/
    return (var_out);
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/div_32.c
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

 Filename: div_32.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "basic_op.h"

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
 FUNCTION NAME: div_32
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    L_num = 32 bit signed integer (Word32) whose value falls in the
                range : 0x0000 0000 < L_num < L_denom
    L_denom_hi = 16 bit positive normalized integer whose value falls in
               the range : 0x4000 < hi < 0x7fff
    L_denom_lo = 16 bit positive integer whose value falls in the range :
               0 < lo < 0x7fff

    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the 32 bit divide operation resulted in overflow

 Returns:
    result = 32-bit quotient of of the division of two 32 bit integers
                L_num / L_denom (Word32)

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function is a fractional integer division of two 32 bit numbers, the
 numerator L_num and the denominator L_denom. The denominator is formed by
 combining denom_hi and denom_lo. Note that denom_hi is a normalized numbers.
 The numerator and denominator must be positive and the numerator must be
 less than the denominator.

 The division is done as follows:
 1. Find 1/L_denom by first approximating: approx = 1 / denom_hi.
 2. 1/L_denom = approx * (2.0 - L_denom * approx ).
 3. result = L_num * (1/L_denom).

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] div_32() function in oper_32b.c,  UMTS GSM AMR speech codec, R99 -
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
Word32 Div_32(Word32 L_num,
              Word16 L_denom_hi,
              Word16 L_denom_lo,
              Flag   *pOverflow)
{

    Word16 approx;
    Word16 hi;
    Word16 lo;
    Word16 n_hi;
    Word16 n_lo;
    Word32 result;

    /* First approximation: 1 / L_denom = 1/L_denom_hi */

    approx = div_s((Word16) 0x3fff, L_denom_hi);

    /* 1/L_denom = approx * (2.0 - L_denom * approx) */

    result = Mpy_32_16(L_denom_hi, L_denom_lo, approx, pOverflow);
    /* result is > 0 , and less than 1.0 */
    result =  0x7fffffffL - result;

    hi = (Word16)(result >> 16);
    lo = (result >> 1) - (hi << 15);

    result = Mpy_32_16(hi, lo, approx, pOverflow);

    /* L_num * (1/L_denom) */

    hi = (Word16)(result >> 16);
    lo = (result >> 1) - (hi << 15);

    n_hi = (Word16)(L_num >> 16);
    n_lo = (L_num >> 1) - (n_hi << 15);

    result = Mpy_32(n_hi, n_lo, hi, lo, pOverflow);
    result = L_shl(result, 2, pOverflow);

    return (result);
}
