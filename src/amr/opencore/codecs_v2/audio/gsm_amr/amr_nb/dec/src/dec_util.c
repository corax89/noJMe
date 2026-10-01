/* ==================================================================
 * dec_util.c - merged AMR-NB decoder implementation file.
 *
 * Merged from the following per-function sources of the vendored
 * opencore-amrnb decoder (converted from C++ to plain C):
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/a_refl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/ex_ctrl.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/int_lsf.c
 *   opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/lsp_avg.c
 *
 * Every section keeps its original content verbatim; the module
 * headers (.h) and all function signatures are unchanged.
 * ==================================================================
 */



/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/a_refl.c
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



 Filename: a_refl.c
 Functions: a_refl

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "a_refl.h"
#include "typedef.h"
#include "cnst.h"
#include "basic_op.h"

/*----------------------------------------------------------------------------
; MACROS [optional]
; [Define module specific macros here]
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; DEFINES [optional]
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
 FUNCTION NAME: AMREncode
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    a[] = pointer to directform coefficients of type Word16
    refl[] = pointer to reflection coefficients of type Word16

 Outputs:
    pOverflow = 1 if overflow exists in the math operations else zero.

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

     File             : a_refl.c
     Purpose          : Convert from direct form coefficients to
                        reflection coefficients

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 [1] a_refl.c , 3GPP TS 26.101 version 4.1.0 Release 4, June 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


void A_Refl(
   Word16 a[],        // i   : Directform coefficients
   Word16 refl[]      // o   : Reflection coefficients
)
{
   // local variables
   Word16 i,j;
   Word16 aState[M];
   Word16 bState[M];
   Word16 normShift;
   Word16 normProd;
   Word32 L_acc;
   Word16 scale;
   Word32 L_temp;
   Word16 temp;
   Word16 mult;

   // initialize states
   for (i = 0; i < M; i++)
   {
      aState[i] = a[i];
   }

   // backward Levinson recursion
   for (i = M-1; i >= 0; i--)
   {
      if (sub(abs_s(aState[i]), 4096) >= 0)
      {
         goto ExitRefl;
      }

      refl[i] = shl(aState[i], 3);

      L_temp = L_mult(refl[i], refl[i]);
      L_acc = L_sub(MAX_32, L_temp);

      normShift = norm_l(L_acc);
      scale = sub(15, normShift);

      L_acc = L_shl(L_acc, normShift);
      normProd = pv_round(L_acc);

      mult = div_s(16384, normProd);

      for (j = 0; j < i; j++)
      {
         L_acc = L_deposit_h(aState[j]);
         L_acc = L_msu(L_acc, refl[i], aState[i-j-1]);

         temp = pv_round(L_acc);
         L_temp = L_mult(mult, temp);
         L_temp = L_shr_r(L_temp, scale);

         if (L_sub(L_abs(L_temp), 32767) > 0)
         {
            goto ExitRefl;
         }

         bState[j] = extract_l(L_temp);
      }

      for (j = 0; j < i; j++)
      {
         aState[j] = bState[j];
      }
   }
   return;

ExitRefl:
   for (i = 0; i < M; i++)
   {
      refl[i] = 0;
   }
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void A_Refl(
    Word16 a[],        /* i   : Directform coefficients */
    Word16 refl[],     /* o   : Reflection coefficients */
    Flag   *pOverflow
)
{
    /* local variables */
    Word16 i;
    Word16 j;
    Word16 k;
    Word16 aState[M];
    Word16 bState[M];
    Word16 normShift;
    Word16 normProd;
    Word32 L_acc;
    Word16 scale;
    Word32 L_temp;
    Word16 temp;
    Word16 mult;

    /* initialize states */
    for (i = 0; i < M; i++)
    {
        aState[i] = a[i];
    }

    /* backward Levinson recursion */
    for (i = M - 1; i >= 0; i--)
    {
        if (abs_s(aState[i]) >= 4096)
        {
            for (j = 0; j < M; j++)
            {
                refl[j] = 0;
            }
            break;
        }

        refl[i] = shl(aState[i], 3, pOverflow);

        L_temp = L_mult(refl[i], refl[i], pOverflow);
        L_acc = L_sub(MAX_32, L_temp, pOverflow);

        normShift = norm_l(L_acc);
        scale = 15 - normShift;

        L_acc = L_shl(L_acc, normShift, pOverflow);
        normProd = pv_round(L_acc, pOverflow);

        mult = div_s(16384, normProd);

        for (j = 0; j < i; j++)
        {
            L_acc = ((Word32)aState[j] << 16);
            L_acc = L_msu(L_acc, refl[i], aState[i-j-1], pOverflow);

            temp = pv_round(L_acc, pOverflow);
            L_temp = L_mult(mult, temp, pOverflow);
            L_temp = L_shr_r(L_temp, scale, pOverflow);


            Word32 L_tmp_abs = L_temp - (L_temp < 0);
            L_tmp_abs = L_tmp_abs ^(L_tmp_abs >> 31);

            if (L_tmp_abs > 32767)
            {
                for (k = 0; k < M; k++)
                {
                    refl[k] = 0;
                }
                break;
            }

            bState[j] = (Word16)(L_temp);
        }

        for (j = 0; j < i; j++)
        {
            aState[j] = bState[j];
        }
    }
    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/ex_ctrl.c
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



 Filename: ex_ctrl.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "ex_ctrl.h"
#include "typedef.h"
#include "cnst.h"
#include "set_zero.h"
#include "gmed_n.h"
#include "sqrt_l.h"
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
; LOCAL VARIABLE DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: ex_ctrl
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
 excitation = pointer to current subframe excitation of type Word16
 excEnergy = Exc. Energy, sqrt(totEx*totEx) of type Word16
 exEnergyHist = pointer to history of subframe energies of type Word16
 voicedHangover = # of fr. after last voiced fr  of type Word16
 carefulFlag = restrict dynamic in scaling of type Word16
 pOverflow = pointer to overflow indicator

 Outputs:
 pOverflow = 1 if overflow exists in the math functions called by this function.

 Returns:
    None

 Global Variables Used:
    None

 Local Variables Needed:
    None

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 Function    : Ex_ctrl
 Purpose     : Charaterice synthesis speech and detect background noise
 Returns     : background noise decision; 0 = no bgn, 1 = bgn

------------------------------------------------------------------------------
 REQUIREMENTS

 None

------------------------------------------------------------------------------
 REFERENCES

 ex_ctrl.c, 3GPP TS 26.101 version 4.1.0 Release 4, June 2001

------------------------------------------------------------------------------
 PSEUDO-CODE



------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/
Word16 Ex_ctrl(Word16 excitation[],    /*i/o: Current subframe excitation   */
               Word16 excEnergy,      /* i : Exc. Energy, sqrt(totEx*totEx)*/
               Word16 exEnergyHist[], /* i : History of subframe energies  */
               Word16 voicedHangover, /* i : # of fr. after last voiced fr.*/
               Word16 prevBFI,        /* i : Set i previous BFI            */
               Word16 carefulFlag,    /* i : Restrict dymamic in scaling   */
               Flag   *pOverflow
              )
{
    Word16 i, exp;
    Word16 testEnergy, scaleFactor, avgEnergy, prevEnergy;
    Word32 t0;

    /* get target level */
    avgEnergy = gmed_n(exEnergyHist, 9);

    prevEnergy = (exEnergyHist[7] + exEnergyHist[8]) >> 1;
    if (exEnergyHist[8] < prevEnergy)
    {
        prevEnergy = exEnergyHist[8];
    }

    /* upscaling to avoid too rapid energy rises  for some cases */
    if ((excEnergy < avgEnergy) && (excEnergy > 5))
    {
        testEnergy = shl(prevEnergy, 2, pOverflow);  /* testEnergy = 4*prevEnergy; */

        if ((voicedHangover < 7) || prevBFI != 0)
        {
            /* testEnergy = 3*prevEnergy */
            testEnergy = sub(testEnergy, prevEnergy, pOverflow);
        }

        if (avgEnergy > testEnergy)
        {
            avgEnergy = testEnergy;
        }

        /* scaleFactor=avgEnergy/excEnergy in Q0 (const 29 below)*/
        exp = norm_s(excEnergy);
        excEnergy = shl(excEnergy, exp, pOverflow);
        excEnergy = div_s((Word16) 16383, excEnergy);
        t0 = L_mult(avgEnergy, excEnergy, pOverflow);
        t0 = L_shr(t0, sub(20, exp, pOverflow), pOverflow);
        /* const=30 for t0 in Q0, 20 for Q10 */
        if (t0 > 32767)
        {
            t0 = 32767; /* saturate  */
        }
        scaleFactor = (Word16)(t0);

        /* test if scaleFactor > 3.0 */
        if (carefulFlag != 0 && (scaleFactor > 3072))
        {
            scaleFactor = 3072;
        }

        /* scale the excitation by scaleFactor */
        for (i = 0; i < L_SUBFR; i++)
        {
            t0 = L_mult(scaleFactor, excitation[i], pOverflow);
            t0 = L_shr(t0, 11, pOverflow);
            excitation[i] = (Word16)(t0);
        }
    }

    return 0;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/int_lsf.c
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



 Filename: int_lsf.c

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include    "int_lsf.h"
#include    "typedef.h"
#include    "basic_op.h"
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
 FUNCTION NAME: Int_lsf
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    lsf_old = LSF vector at the 4th SF of past frame (Word16)
    lsf_new = LSF vector at the 4th SF of present frame (Word16)
    i_subfr = Current subframe (equal to 0,40,80 or 120) (Word16)
    lsf_out = interpolated LSF parameters for current subframe (Word16)

 Outputs:
    lsf_out   = new interpolated LSF parameters for current subframe
    pOverflow = pointer of type Flag * to overflow indicator.

 Returns:
    None.

 Global Variables Used:
    None.

 Local Variables Needed:
    None.

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

 This function interpolates the LSFs for selected subframe.
 The 20 ms speech frame is divided into 4 subframes. The LSFs are
 interpolated at the 1st, 2nd and 3rd subframe and only forwarded
 at the 4th subframe.

                      |------|------|------|------|
                         sf1    sf2    sf3    sf4
                   F0                          F1

                 sf1:   3/4 F0 + 1/4 F1         sf3:   1/4 F0 + 3/4 F1
                 sf2:   1/2 F0 + 1/2 F1         sf4:       F1

------------------------------------------------------------------------------
 REQUIREMENTS

 None.

------------------------------------------------------------------------------
 REFERENCES

 int_lsf.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

void Int_lsf(
    Word16 lsf_old[], // i : LSF vector at the 4th SF of past frame
    Word16 lsf_new[], // i : LSF vector at the 4th SF of present frame
    Word16 i_subfr,   // i : Pointer to current sf (equal to 0,40,80 or 120)
    Word16 lsf_out[]  // o : interpolated LSF parameters for current sf
)
{
    Word16 i;

    if ( i_subfr == 0 )
    {
       for (i = 0; i < M; i++) {
          lsf_out[i] = add(sub(lsf_old[i], shr(lsf_old[i], 2)),
                           shr(lsf_new[i], 2));
       }
    }
    else if ( sub(i_subfr, 40) == 0 )
    {
       for (i = 0; i < M; i++) {
          lsf_out[i] = add(shr(lsf_old[i],1), shr(lsf_new[i], 1) );
       }
    }
    else if ( sub(i_subfr, 80) == 0 )
    {
       for (i = 0; i < M; i++) {
          lsf_out[i] = add(shr(lsf_old[i], 2),
                           sub(lsf_new[i], shr(lsf_new[i], 2)));
       }
    }
    else if ( sub(i_subfr, 120) == 0 )
    {
       for (i = 0; i < M; i++) {
          lsf_out[i] = lsf_new[i];
       }
    }

    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void Int_lsf(
    Word16 lsf_old[], /* i : LSF vector at the 4th SF of past frame         */
    Word16 lsf_new[], /* i : LSF vector at the 4th SF of present frame      */
    Word16 i_subfr,   /* i : Current sf (equal to 0,40,80 or 120)           */
    Word16 lsf_out[], /* o : interpolated LSF parameters for current sf     */
    Flag  *pOverflow  /* o : flag set if overflow occurs                    */
)
{
    register Word16 i;
    register Word16 temp1;
    register Word16 temp2;

    if (i_subfr == 0)
    {
        for (i = M - 1; i >= 0; i--)
        {
            if (*(lsf_old + i) < 0)
            {
                temp1 = ~(~(*(lsf_old + i)) >> 2);
            }
            else
            {
                temp1 = *(lsf_old + i) >> 2;
            }
            if (*(lsf_new + i) < 0)
            {
                temp2 = ~(~(*(lsf_new + i)) >> 2);
            }
            else
            {
                temp2 = *(lsf_new + i) >> 2;
            }
            *(lsf_out + i) = add_16((Word16)(*(lsf_old + i) - temp1),
                                    (Word16)temp2,
                                    pOverflow);
        }
    }

    else if (i_subfr == 40)
    {
        for (i = M - 1; i >= 0; i--)
        {
            if (*(lsf_old + i) < 0)
            {
                temp1 = ~(~(*(lsf_old + i)) >> 1);
            }
            else
            {
                temp1 = *(lsf_old + i) >> 1;
            }
            if (*(lsf_new + i) < 0)
            {
                temp2 = ~(~(*(lsf_new + i)) >> 1);
            }
            else
            {
                temp2 = *(lsf_new + i) >> 1;
            }
            *(lsf_out + i) = temp1 + temp2;
        }
    }

    else if (i_subfr == 80)
    {
        for (i = M - 1; i >= 0; i--)
        {
            if (*(lsf_old + i) < 0)
            {
                temp1 = ~(~(*(lsf_old + i)) >> 2);
            }
            else
            {
                temp1 = *(lsf_old + i) >> 2;
            }
            if (*(lsf_new + i) < 0)
            {
                temp2 = ~(~(*(lsf_new + i)) >> 2);
            }
            else
            {
                temp2 = *(lsf_new + i) >> 2;
            }
            *(lsf_out + i) = add_16((Word16)temp1,
                                    (Word16)(*(lsf_new + i) - temp2),
                                    pOverflow);

        }
    }

    else if (i_subfr == 120)
    {
        for (i = M - 1; i >= 0; i--)
        {
            *(lsf_out + i) = *(lsf_new + i);
        }
    }

    return;
}


/* ==================================================================
 * Section: opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/lsp_avg.c
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



 Filename: lsp_avg.c

------------------------------------------------------------------------------
 MODULE DESCRIPTION

    LSP averaging and history
------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "lsp_avg.h"
#include "basic_op.h"
#include "oper_32b.h"
#include "oscl_mem.h"
#include "q_plsf_5_tbl.h"

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
; LOCAL VARIABLE DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL FUNCTION REFERENCES
; Declare functions defined elsewhere and referenced in this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL VARIABLES REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*
------------------------------------------------------------------------------
 FUNCTION NAME: lsp_avg_reset
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    st = pointer to structure of type lsp_avgState

 Outputs:
    fields of the structure pointed to by state are initialized.

 Returns:
    return_value = 0, if reset was successful; -1, otherwise (int)

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

lsp_avg.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE

int lsp_avg_reset (lsp_avgState *st)
{
  if (st == (lsp_avgState *) NULL){
      // fprintf(stderr, "lsp_avg_reset: invalid parameter\n");
      return -1;
  }

  Copy(mean_lsf, &st->lsp_meanSave[0], M);

  return 0;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

Word16 lsp_avg_reset(lsp_avgState *st, const Word16* mean_lsf_5_ptr)
{
    if (st == (lsp_avgState *) NULL)
    {
        /* fprintf(stderr, "lsp_avg_reset: invalid parameter\n"); */
        return -1;
    }

    oscl_memmove((void *)&st->lsp_meanSave[0], mean_lsf_5_ptr, M*sizeof(*mean_lsf_5_ptr));

    return 0;
}


/*
------------------------------------------------------------------------------
 FUNCTION NAME: lsp_avg
------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

 Inputs:
    st  = pointer to structure of type lsp_avgState
    lsp = pointer to Word16, which reflects the state of the state machine

 Outputs:
    st = pointer to structure of type lsp_avgState
    pOverflow = pointer to type Flag -- overflow indicator

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

lsp_avg.c, UMTS GSM AMR speech codec, R99 - Version 3.2.0, March 2, 2001

------------------------------------------------------------------------------
 PSEUDO-CODE


void lsp_avg (
    lsp_avgState *st,         // i/o : State struct                 Q15
    Word16 *lsp               // i   : state of the state machine   Q15
)
{
    Word16 i;
    Word32 L_tmp;            // Q31

    for (i = 0; i < M; i++) {

       // mean = 0.84*mean
       L_tmp = L_deposit_h(st->lsp_meanSave[i]);
       L_tmp = L_msu(L_tmp, EXPCONST, st->lsp_meanSave[i]);

       // Add 0.16 of newest LSPs to mean
       L_tmp = L_mac(L_tmp, EXPCONST, lsp[i]);

       // Save means
       st->lsp_meanSave[i] = pv_round(L_tmp);   // Q15
    }

    return;
}

------------------------------------------------------------------------------
 CAUTION [optional]
 [State any special notes, constraints or cautions for users of this function]

------------------------------------------------------------------------------
*/

void lsp_avg(
    lsp_avgState *st,         /* i/o : State struct                 Q15 */
    Word16 *lsp,              /* i   : state of the state machine   Q15 */
    Flag   *pOverflow         /* o   : Flag set when overflow occurs    */
)
{
    Word16 i;
    Word32 L_tmp;            /* Q31 */

    for (i = 0; i < M; i++)
    {

        /* mean = 0.84*mean */
        L_tmp = ((Word32)st->lsp_meanSave[i] << 16);
        L_tmp = L_msu(L_tmp, EXPCONST, st->lsp_meanSave[i], pOverflow);

        /* Add 0.16 of newest LSPs to mean */
        L_tmp = L_mac(L_tmp, EXPCONST, lsp[i], pOverflow);

        /* Save means */
        st->lsp_meanSave[i] = pv_round(L_tmp, pOverflow);   /* Q15 */
    }

    return;
}
