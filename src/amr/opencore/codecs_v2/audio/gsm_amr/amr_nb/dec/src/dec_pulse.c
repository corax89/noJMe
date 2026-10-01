/* ==================================================================
 * dec_pulse.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d1035pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d2_11pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d2_9pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d3_14pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d4_17pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d8_31pf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/reorder.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d1035pf.c
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



 Filename: d1035pf.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "d1035pf.h"
#include "typedef.h"
#include "basic_op.h"
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
#define NB_PULSE  10            /* number of pulses  */

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
 FUNCTION NAME: dec_10i40_35bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    index = buffer containing index of 10 pulses; each element is
        represented by sign+position
    cod = buffer of algebraic (fixed) codebook excitation

 Outputs:
    cod buffer contains the new algebraic codebook excitation

 Returns:
    None

 Global Variables Used:
    dgray = gray decoding table

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function builds the innovative codevector from the received index of
 algebraic codebook. See c1035pf.c for more details about the algebraic
 codebook structure.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d1035pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void dec_10i40_35bits (
    Word16 index[],    // (i)     : index of 10 pulses (sign+position)
    Word16 cod[]       // (o)     : algebraic (fixed) codebook excitation
)
{
    Word16 i, j, pos1, pos2, sign, tmp;

    for (i = 0; i < L_CODE; i++)
    {
        cod[i] = 0;
    }

    // decode the positions and signs of pulses and build the codeword

    for (j = 0; j < NB_TRACK; j++)
    {
        // compute index i

        tmp = index[j];
        i = tmp & 7;
        i = dgray[i];

        i = extract_l (L_shr (L_mult (i, 5), 1));
        pos1 = add (i, j); // position of pulse "j"

        i = shr (tmp, 3) & 1;
        if (i == 0)
        {
            sign = 4096; // +1.0
        }
        else
        {
            sign = -4096; // -1.0
        }

        cod[pos1] = sign;

        // compute index i

        i = index[add (j, 5)] & 7;
        i = dgray[i];
        i = extract_l (L_shr (L_mult (i, 5), 1));

        pos2 = add (i, j);      // position of pulse "j+5"

        if (sub (pos2, pos1) < 0)
        {
            sign = negate (sign);
        }
        cod[pos2] = add (cod[pos2], sign);
    }

    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void dec_10i40_35bits(
    Word16 index[],    /* (i)     : index of 10 pulses (sign+position)       */
    Word16 cod[],       /* (o)     : algebraic (fixed) codebook excitation    */
    const Word16* dgray_ptr /* i : ptr to read-only tbl                       */
)
{
    register Word16 i, j, pos1, pos2;
    Word16 sign, tmp;

    for (i = 0; i < L_CODE; i++)
    {
        *(cod + i) = 0;
    }

    /* decode the positions and signs of pulses and build the codeword */

    for (j = 0; j < NB_TRACK; j++)
    {
        /* compute index i */

        tmp = *(index + j);
        i = tmp & 7;
        i = *(dgray_ptr + i);

        i = (Word16)(i * 5);
        pos1 = i + j; /* position of pulse "j" */

        i = (tmp >> 3) & 1;

        if (i == 0)
        {
            sign = 4096;                                 /* +1.0 */
        }
        else
        {
            sign = -4096;                                /* -1.0 */
        }

        *(cod + pos1) = sign;

        /* compute index i */

        i = *(index + j + 5) & 7;
        i = *(dgray_ptr + i);
        i = (Word16)(i * 5);

        pos2 = i + j;      /* position of pulse "j+5" */


        if (pos2 < pos1)
        {
            sign = negate(sign);
        }
        *(cod + pos2) += sign;
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d2_11pf.c
 * ==================================================================
 */

/* The sections above already define the macro(s) NB_PULSE; undefine them so this section sees
   exactly the values it defined in its original single-file build. */
#undef NB_PULSE

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



 Filename: d2_11pf.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "d2_11pf.h"
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
#define NB_PULSE  2


/*----------------------------------------------------------------------------
; LOCAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL VARIABLE DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: decode_2i40_11bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    sign  -- Word16 -- signs of 2 pulses.
    index -- Word16 -- Positions of the 2 pulses.

 Outputs:
    cod[] -- array of type Word16 -- algebraic (fixed) codebook excitation

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION


------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d2_11pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void decode_2i40_11bits(
    Word16 sign,   /* i : signs of 2 pulses.                       */
    Word16 index,  /* i : Positions of the 2 pulses.               */
    Word16 cod[]   /* o : algebraic (fixed) codebook excitation    */
)

{
    Word16 i;
    Word16 j;

    Word16 pos[NB_PULSE];

    /* Decode the positions */

    j = index & 0x1;

    index >>= 1;

    i = index & 0x7;

    pos[0] = i * 5 + j * 2 + 1;




    index >>= 3;

    j = index & 0x3;

    index >>= 2;

    i = index & 0x7;

    if (j == 3)
    {
        pos[1] = i * 5 + 4;
    }
    else
    {
        pos[1] = i * 5 + j;
    }




    /* decode the signs  and build the codeword */
    for (i = 0; i < L_SUBFR; i++)
    {
        cod[i] = 0;
    }

    for (j = 0; j < NB_PULSE; j++)
    {
        i = sign & 1;

        /* This line is equivalent to...
         *
         *
         *  if (i == 1)
         *  {
         *      cod[pos[j]] = 8191;
         *  }
         *  if (i == 0)
         *  {
         *      cod[pos[j]] = -8192;
         *  }
         */

        cod[pos[j]] = i * 16383 - 8192;

        sign >>= 1;
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d2_9pf.c
 * ==================================================================
 */

/* The sections above already define the macro(s) NB_PULSE; undefine them so this section sees
   exactly the values it defined in its original single-file build. */
#undef NB_PULSE

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



 Filename: d2_9pf.c
 Functions: decode_2i40_9bits

------------------------------------------------------------------------------
 MODULE DESCRIPTION


 FUNCTION:  decode_2i40_9bits (decod_ACELP())

 PURPOSE:   Algebraic codebook decoder. For details about the encoding see
            c2_9pf.c
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "d2_9pf.h"
#include "typedef.h"
#include "basic_op.h"
#include "cnst.h"


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
#define NB_PULSE  2


    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: decode_2i40_11bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    sign  -- Word16 -- signs of 2 pulses.
    index -- Word16 -- Positions of the 2 pulses.

 Outputs:
    cod[] -- array of type Word16 -- algebraic (fixed) codebook excitation
    pOverflow = pointer to overflow flag

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION


------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d2_9pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void decode_2i40_9bits(
    Word16 subNr,  /* i : subframe number                          */
    Word16 sign,   /* i : signs of 2 pulses.                       */
    Word16 index,  /* i : Positions of the 2 pulses.               */
    const Word16* startPos_ptr, /*  i: ptr to read only table          */
    Word16 cod[],  /* o : algebraic (fixed) codebook excitation    */
    Flag  *pOverflow  /* o : Flag set when overflow occurs         */
)
{
    Word16 i;
    Word16 j;
    Word16 k;

    Word16 pos[NB_PULSE];

    /* Decode the positions */
    /* table bit  is the MSB */

    j = (Word16)(index & 64);

    j >>= 3;

    i = index & 7;

    k =
        shl(
            subNr,
            1,
            pOverflow);

    k += j;

    /* pos0 =i*5+startPos_ptr[j*8+subNr*2] */
    pos[0] = i * 5 + startPos_ptr[k++];


    index >>= 3;

    i = index & 7;

    /* pos1 =i*5+startPos_ptr[j*8+subNr*2 + 1] */
    pos[1] = i * 5 + startPos_ptr[k];


    /* decode the signs  and build the codeword */

    for (i = L_SUBFR - 1; i >= 0; i--)
    {
        cod[i] = 0;
    }

    for (j = 0; j < NB_PULSE; j++)
    {
        i = sign & 0x1;

        /* This line is equivalent to...
         *
         *
         *  if (i == 1)
         *  {
         *      cod[pos[j]] = 8191;
         *  }
         *  if (i == 0)
         *  {
         *      cod[pos[j]] = -8192;
         *  }
         */

        cod[pos[j]] = i * 16383 - 8192;

        sign >>= 1;
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d3_14pf.c
 * ==================================================================
 */

/* The sections above already define the macro(s) NB_PULSE; undefine them so this section sees
   exactly the values it defined in its original single-file build. */
#undef NB_PULSE

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



 Filename: d3_14pf.c
 Functions: decode_3i40_14bits

------------------------------------------------------------------------------
 MODULE DESCRIPTION


 FUNCTION:  decode_3i40_14bits (decod_ACELP())

 PURPOSE:   Algebraic codebook decoder. For details about the encoding see
            c3_14pf.c
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"
#include "cnst.h"
#include "d3_14pf.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here. Include conditional
; compile variables also.
----------------------------------------------------------------------------*/
#define NB_PULSE 3           /* number of pulses  */


/*----------------------------------------------------------------------------
; LOCAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL VARIABLE DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: decode_3i40_14bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    sign  -- Word16 -- signs of 3 pulses.
    index -- Word16 -- Positions of the 3 pulses.

 Outputs:
    cod[] -- array of type Word16 -- algebraic (fixed) codebook excitation

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION


------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d2_9pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void decode_3i40_14bits(
    Word16 sign,   /* i : signs of 3 pulses.                       */
    Word16 index,  /* i : Positions of the 3 pulses.               */
    Word16 cod[]   /* o : algebraic (fixed) codebook excitation    */
)
{
    Word16 i;
    Word16 j;

    Word16 pos[NB_PULSE];

    /* Decode the positions */

    i = index & 0x7;

    pos[0] = i * 5;





    index >>= 3;

    j = index & 0x1;

    index >>= 1;

    i = index & 0x7;

    pos[1] = i * 5 + j * 2 + 1;





    index >>= 3;

    j = index & 0x1;

    index >>= 1;

    i = index & 0x7;

    pos[2] = i * 5 + j * 2 + 2;


    /* decode the signs  and build the codeword */

    for (i = 0; i < L_SUBFR; i++)
    {
        cod[i] = 0;
    }

    for (j = 0; j < NB_PULSE; j++)
    {
        i = sign & 1;

        /* This line is equivalent to...
         *
         *
         *  if (i == 1)
         *  {
         *      cod[pos[j]] = 8191;
         *  }
         *  if (i == 0)
         *  {
         *      cod[pos[j]] = -8192;
         *  }
         */

        cod[pos[j]] = i * 16383 - 8192;

        sign >>= 1;

    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d4_17pf.c
 * ==================================================================
 */

/* The sections above already define the macro(s) NB_PULSE; undefine them so this section sees
   exactly the values it defined in its original single-file build. */
#undef NB_PULSE

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



 Filename: d4_17pf.c
 Functions: decode_4i40_17bits

------------------------------------------------------------------------------
 MODULE DESCRIPTION


 FUNCTION:  decode_4i40_17bits (decod_ACELP())

 PURPOSE:   Algebraic codebook decoder. For details about the encoding see
            c4_17pf.c
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "typedef.h"
#include "basic_op.h"
#include "cnst.h"
#include "d4_17pf.h"

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
#define NB_PULSE 4           /* number of pulses  */


    /*----------------------------------------------------------------------------
    ; LOCAL FUNCTION DEFINITIONS
    ; Function Prototype declaration
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; LOCAL VARIABLE DEFINITIONS
    ; Variable declaration - defined here and used outside this module
    ----------------------------------------------------------------------------*/

    /*----------------------------------------------------------------------------
    ; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
    ; Declare variables used in this module but defined elsewhere
    ----------------------------------------------------------------------------*/

    /*--------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: decode_4i40_17bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    sign  -- Word16 -- signs of 3 pulses.
    index -- Word16 -- Positions of the 3 pulses.

 Outputs:
    cod[] -- array of type Word16 -- algebraic (fixed) codebook excitation

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION


------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d4_17pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void decode_4i40_17bits(
    Word16 sign,   /* i : signs of 4 pulses.                       */
    Word16 index,  /* i : Positions of the 4 pulses.               */
    const Word16* dgray_ptr, /* i : Pointer to read-only table         */
    Word16 cod[]   /* o : algebraic (fixed) codebook excitation    */
)
{
    Word16 i;
    Word16 j;

    Word16 pos[NB_PULSE];

    /* Index is a 13-bit value.  3 bits each correspond to the
     * positions 0-2, with 4 bits allocated for position 3.
     *
     *
     * [][][][] [][][] [][][] [][][]
     *    |       |      |     |
     *    |       |      |     |
     *   pos3    pos2   pos1  pos0
     */

    /* Decode the positions */

    i = index & 0x7;

    i = dgray_ptr[i];

    pos[0] = i * 5; /* pos0 =i*5 */


    index >>= 3;

    i = index & 0x7;

    i = dgray_ptr[i];

    pos[1] = i * 5 + 1;  /* pos1 =i*5+1 */



    index >>= 3;

    i = index & 0x7;

    i = dgray_ptr[i];

    pos[2] = i * 5 + 2; /* pos2 =i*5+2 */





    index >>= 3;

    j = index & 0x1;

    index >>= 1;

    i = index & 0x7;

    i = dgray_ptr[i];

    pos[3] = i * 5 + 3 + j; /* pos3 =i*5+3+j */


    /* decode the signs  and build the codeword */

    for (i = 0; i < L_SUBFR; i++)
    {
        cod[i] = 0;
    }

    for (j = 0; j < NB_PULSE; j++)
    {
        i = sign & 0x1;

        /* This line is equivalent to...
         *
         *
         *  if (i == 1)
         *  {
         *      cod[pos[j]] = 8191;
         *  }
         *  if (i == 0)
         *  {
         *      cod[pos[j]] = -8192;
         *  }
         */

        cod[pos[j]] = i * 16383 - 8192;

        sign >>= 1;
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/d8_31pf.c
 * ==================================================================
 */

/* The sections above already define the macro(s) NB_PULSE; undefine them so this section sees
   exactly the values it defined in its original single-file build. */
#undef NB_PULSE

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



 Filename: d8_31pf.c

------------------------------------------------------------------------------

 MODULE DESCRIPTION
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "d8_31pf.h"
#include "typedef.h"
#include "basic_op.h"
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
#define NB_PULSE  8           /* number of pulses  */

/* define values/representation for output codevector and sign */
#define POS_CODE  8191
#define NEG_CODE  8191


/*----------------------------------------------------------------------------
; LOCAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL VARIABLE DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: decompress10
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
   MSBs -- Word16 -- MSB part of the index
   LSBs -- Word16 -- LSB part of the index
   index1 -- Word16 -- index for first pos in pos_index[]
   index2 -- Word16 -- index for second pos in pos_index[]
   index3 -- Word16 -- index for third pos in pos_index[]

 Outputs:
   pos_indx[] -- array of type Word16 -- position of 3 pulses (decompressed)

   pOverflow  Flag set when overflow occurs, pointer of type Flag *

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION


------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d8_31pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

static void decompress10(
    Word16 MSBs,        /* i : MSB part of the index                 */
    Word16 LSBs,        /* i : LSB part of the index                 */
    Word16 index1,      /* i : index for first pos in pos_index[]    */
    Word16 index2,      /* i : index for second pos in pos_index[]   */
    Word16 index3,      /* i : index for third pos in pos_index[]    */
    Word16 pos_indx[],  /* o : position of 3 pulses (decompressed)   */
    Flag  *pOverflow)   /* o : Flag set when overflow occurs         */
{
    Word16 ia;
    Word16 ib;
    Word16 ic;
    Word32 tempWord32;

    /*
      pos_indx[index1] = ((MSBs-25*(MSBs/25))%5)*2 + (LSBs-4*(LSBs/4))%2;
      pos_indx[index2] = ((MSBs-25*(MSBs/25))/5)*2 + (LSBs-4*(LSBs/4))/2;
      pos_indx[index3] = (MSBs/25)*2 + LSBs/4;
    */

    if (MSBs > 124)
    {
        MSBs = 124;
    }

    ia =
        mult(
            MSBs,
            1311,
            pOverflow);

    tempWord32 =
        L_mult(
            ia,
            25,
            pOverflow);


    ia = (Word16)(MSBs - (tempWord32 >> 1));
    ib =
        mult(
            ia,
            6554,
            pOverflow);

    tempWord32 =
        L_mult(
            ib,
            5,
            pOverflow);

    ib = ia - (Word16)(tempWord32 >> 1);

    ib =
        shl(
            ib,
            1,
            pOverflow);


    ic = LSBs - ((LSBs >> 2) << 2);


    pos_indx[index1] = ib + (ic & 1);


    ib =
        mult(
            ia,
            6554,
            pOverflow);

    ib =
        shl(
            ib,
            1,
            pOverflow);


    pos_indx[index2] = ib + (ic >> 1);


    ib = LSBs >> 2;

    ic =
        mult(
            MSBs,
            1311,
            pOverflow);

    ic =
        shl(
            ic,
            1,
            pOverflow);

    pos_indx[index3] = add_16(ib, ic, pOverflow);

    return;
}


/*
------------------------------------------------------------------------------
 FUNCTION NAME: decompress_code
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    indx[] -- array of type Word16 -- position and sign of
                                      8 pulses (compressed)

 Outputs:
    sign_indx[] -- array of type Word16 -- signs of 4 pulses (signs only)
    pos_indx[]  -- array of type Word16 -- position index of 8 pulses
                                           (position only)
    pOverflow pointer to type Flag -- Flag set when overflow occurs

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

    PURPOSE: decompression of the linear codewords to 4+three indeces
             one bit from each pulse is made robust to errors by
             minimizing the phase shift of a bit error.
             4 signs (one for each track)
             i0,i4,i1 => one index (7+3) bits, 3   LSBs more robust
             i2,i6,i5 => one index (7+3) bits, 3   LSBs more robust
             i3,i7    => one index (5+2) bits, 2-3 LSbs more robust

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d8_31pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

static void decompress_code(
    Word16 indx[],      /* i : position and sign of 8 pulses (compressed) */
    Word16 sign_indx[], /* o : signs of 4 pulses (signs only)             */
    Word16 pos_indx[],  /* o : position index of 8 pulses (position only) */
    Flag  *pOverflow    /* o : Flag set when overflow occurs              */
)
{
    Word16 i;
    Word16 ia;
    Word16 ib;
    Word16 MSBs;
    Word16 LSBs;
    Word16 MSBs0_24;
    Word32 tempWord32;

    for (i = 0; i < NB_TRACK_MR102; i++)
    {
        sign_indx[i] = indx[i];
    }

    /*
      First index: 10x10x10 -> 2x5x2x5x2x5-> 125x2x2x2 -> 7+1x3 bits
      MSBs = indx[NB_TRACK]/8;
      LSBs = indx[NB_TRACK]%8;
      */
    MSBs = indx[NB_TRACK_MR102] >> 3;

    LSBs = indx[NB_TRACK_MR102] & 0x7;

    decompress10(
        MSBs,
        LSBs,
        0,
        4,
        1,
        pos_indx,
        pOverflow);

    /*
      Second index: 10x10x10 -> 2x5x2x5x2x5-> 125x2x2x2 -> 7+1x3 bits
      MSBs = indx[NB_TRACK+1]/8;
      LSBs = indx[NB_TRACK+1]%8;
      */
    MSBs = indx[NB_TRACK_MR102+1] >> 3;

    LSBs = indx[NB_TRACK_MR102+1] & 0x7;

    decompress10(
        MSBs,
        LSBs,
        2,
        6,
        5,
        pos_indx,
        pOverflow);

    /*
      Third index: 10x10 -> 2x5x2x5-> 25x2x2 -> 5+1x2 bits
      MSBs = indx[NB_TRACK+2]/4;
      LSBs = indx[NB_TRACK+2]%4;
      MSBs0_24 = (MSBs*25+12)/32;
      if ((MSBs0_24/5)%2==1)
         pos_indx[3] = (4-(MSBs0_24%5))*2 + LSBs%2;
      else
         pos_indx[3] = (MSBs0_24%5)*2 + LSBs%2;
      pos_indx[7] = (MSBs0_24/5)*2 + LSBs/2;
      */

    MSBs = indx[NB_TRACK_MR102+2] >> 2;

    LSBs = indx[NB_TRACK_MR102+2] & 0x3;

    tempWord32 =
        L_mult(
            MSBs,
            25,
            pOverflow);

    ia =
        (Word16)
        L_shr(
            tempWord32,
            1,
            pOverflow);

    ia += 12;

    MSBs0_24 = ia >> 5;


    ia =
        mult(
            MSBs0_24,
            6554,
            pOverflow);

    ia &= 1;


    ib =
        mult(
            MSBs0_24,
            6554,
            pOverflow);

    tempWord32 =
        L_mult(
            ib,
            5,
            pOverflow);


    ib = MSBs0_24 - (Word16)(tempWord32 >> 1);

    if (ia == 1)
    {
        ib = 4 - ib;

    }


    ib =
        shl(
            ib,
            1,
            pOverflow);

    ia = LSBs & 0x1;

    pos_indx[3] =
        add_16(
            ib,
            ia,
            pOverflow);

    ia =
        mult(
            MSBs0_24,
            6554,
            pOverflow);

    ia =
        shl(
            ia,
            1,
            pOverflow);

    pos_indx[7] = ia + (LSBs >> 1);

}

/*
------------------------------------------------------------------------------
 FUNCTION NAME: dec_8i40_31bits
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    index   array of type Word16 --  index of 8 pulses (sign+position)

 Outputs:
    cod     array of type Word16 --  algebraic (fixed) codebook excitation
    pOverflow pointer to type Flag -- Flag set when overflow occurs

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 PURPOSE:  Builds the innovative codevector from the received
           index of algebraic codebook.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 d8_31pf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void dec_8i40_31bits(
    Word16 index[],    /* i : index of 8 pulses (sign+position)         */
    Word16 cod[],      /* o : algebraic (fixed) codebook excitation     */
    Flag  *pOverflow   /* o : Flag set when overflow occurs             */
)
{
    Word16 i;
    Word16 j;
    Word16 pos1;
    Word16 pos2;
    Word16 sign;

    Word16 linear_signs[NB_TRACK_MR102];
    Word16 linear_codewords[NB_PULSE];

    for (i = 0; i < L_CODE; i++)
    {
        cod[i] = 0;
    }

    decompress_code(
        index,
        linear_signs,
        linear_codewords,
        pOverflow);

    /* decode the positions and signs of pulses and build the codeword */
    for (j = 0; j < NB_TRACK_MR102; j++)    /* NB_TRACK_MR102 = 4 */
    {
        /* position of pulse "j" */

        pos1 = (linear_codewords[j] << 2) + j;


        if (linear_signs[j] == 0)
        {
            sign = POS_CODE; /* +1.0 */
        }
        else
        {
            sign = -NEG_CODE; /* -1.0 */
        }

        if (pos1 < L_SUBFR)
        {
            cod[pos1] = sign;    /* avoid buffer overflow */
        }

        /* compute index i */
        /* position of pulse "j+4" */

        pos2 = (linear_codewords[j + 4] << 2) + j;


        if (pos2 < pos1)
        {
            sign = negate(sign);
        }

        if (pos2 < L_SUBFR)
        {
            cod[pos2] += sign;     /* avoid buffer overflow */
        }


    } /* for (j = 0; j < NB_TRACK_MR102; j++) */

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/reorder.c
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
 Filename: reorder.c

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "reorder.h"

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
 FUNCTION NAME: Reorder_lsf
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    lsf = vector of LSFs   (range: 0<=val<=0.5)(Word16)
    min_dist = minimum required distance (Word16)
    n = LPC order (Word16)
    pOverflow = pointer to overflow (Flag)

 Outputs:
    pOverflow -> 1 if the add operation called by Reorder_lsf() results in
     overflow
    lsf -> reordered vector of LSFs (Word16)

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function makes sure that the LSFs are properly ordered keeps a certain
 minimum distance between adjacent LSFs.

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] reorder.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void Reorder_lsf (
    Word16 *lsf,        // (i/o)     : vector of LSFs   (range: 0<=val<=0.5)
    Word16 min_dist,    // (i)       : minimum required distance
    Word16 n            // (i)       : LPC order
)
{
    Word16 i;
    Word16 lsf_min;

// The reference ETSI code uses a global flag for Overflow. In the actual
// implementation a pointer to Overflow flag is passed into the function
// for use by the math functions add() and sub()

    lsf_min = min_dist;
    for (i = 0; i < n; i++)
    {
        if (sub (lsf[i], lsf_min) < 0)
        {
            lsf[i] = lsf_min;
        }
        lsf_min = add (lsf[i], min_dist);
    }
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/
OSCL_EXPORT_REF void Reorder_lsf(
    Word16 *lsf,        /* (i/o)    : vector of LSFs   (range: 0<=val<=0.5) */
    Word16 min_dist,    /* (i)      : minimum required distance             */
    Word16 n,           /* (i)      : LPC order                             */
    Flag   *pOverflow   /* (i/o)    : Overflow flag                         */
)
{
    Word16 i;
    Word16 lsf_min;
    Word16 *p_lsf = &lsf[0];
    OSCL_UNUSED_ARG(pOverflow);

    lsf_min = min_dist;
    for (i = 0; i < n; i++)
    {
        if (*(p_lsf) < lsf_min)
        {
            *(p_lsf++) = lsf_min;
            lsf_min +=  min_dist;
        }
        else
        {
            lsf_min = *(p_lsf++) + min_dist;
        }
    }
}
