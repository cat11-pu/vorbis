/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2009             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: normalized modified discrete cosine transform
           power of two length transform only [64 <= n ]

 Original algorithm adapted long ago from _The use of multirate filter
 banks for coding of high quality digital audio_, by T. Sporer,
 K. Brandenburg and B. Edler, collection of the European Signal
 Processing Conference (EUSIPCO), Amsterdam, June 1992, Vol.1, pp
 211-214

 The below code implements an algorithm that no longer looks much like
 that presented in the paper, but the basic structure remains if you
 dig deep enough to see it.

 This module DOES NOT INCLUDE code to generate/apply the window
 function.  Everybody has their own weird favorite including me... I
 happen to like the properties of y=sin(.5PI*sin^2(x)), but others may
 vehemently disagree.

 ********************************************************************/

/* this can also be run as an integer transform by uncommenting a
   define in mdct.h; the integerization is a first pass and although
   it's likely stable for Vorbis, the dynamic range is constrained and
   roundoff isn't done (so it's noisy).  Consider it functional, but
   only a starting point.  There's no point on a machine with an FPU */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vorbis/codec.h"
#include "mdct.h"
#include "os.h"
#include "misc.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#define HAVE_SSE2 1
#endif

/* build lookups for trig functions; also pre-figure scaling and
   some window function algebra. */

void mdct_init(mdct_lookup *lookup,int n){
  int   *bitrev=_ogg_malloc(sizeof(*bitrev)*(n/4));
  DATA_TYPE *T=_ogg_malloc(sizeof(*T)*(n+n/4));

  int i;
  int n2=n>>1;
  int log2n=lookup->log2n=rint(log((float)n)/log(2.f));
  lookup->n=n;
  lookup->trig=T;
  lookup->bitrev=bitrev;

/* trig lookups... */

  for(i=0;i<n/4;i++){
    T[i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i)));
    T[i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i)));
    T[n2+i*2]=FLOAT_CONV(cos((M_PI/(2*n))*(2*i+1)));
    T[n2+i*2+1]=FLOAT_CONV(sin((M_PI/(2*n))*(2*i+1)));
  }
  for(i=0;i<n/8;i++){
    T[n+i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i+2))*.5);
    T[n+i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i+2))*.5);
  }

  /* bitreverse lookup... */

  {
    int mask=(1<<(log2n-1))-1,i,j;
    int msb=1<<(log2n-2);
    for(i=0;i<n/8;i++){
      int acc=0;
      for(j=0;msb>>j;j++)
        if((msb>>j)&i)acc|=1<<j;
      bitrev[i*2]=((~acc)&mask)-1;
      bitrev[i*2+1]=acc;

    }
  }
  lookup->scale=FLOAT_CONV(4.f/n);
}

/* 8 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_8(DATA_TYPE *x){
  REG_TYPE r0   = x[6] + x[2];
  REG_TYPE r1   = x[6] - x[2];
  REG_TYPE r2   = x[4] + x[0];
  REG_TYPE r3   = x[4] - x[0];

           x[6] = r0   + r2;
           x[4] = r0   - r2;

           r0   = x[5] - x[1];
           r2   = x[7] - x[3];
           x[0] = r1   + r0;
           x[2] = r1   - r0;

           r0   = x[5] + x[1];
           r1   = x[7] + x[3];
           x[3] = r2   + r3;
           x[1] = r2   - r3;
           x[7] = r1   + r0;
           x[5] = r1   - r0;

}

/* 16 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_16(DATA_TYPE *x){
  REG_TYPE r0     = x[1]  - x[9];
  REG_TYPE r1     = x[0]  - x[8];

           x[8]  += x[0];
           x[9]  += x[1];
           x[0]   = MULT_NORM((r0   + r1) * cPI2_8);
           x[1]   = MULT_NORM((r0   - r1) * cPI2_8);

           r0     = x[3]  - x[11];
           r1     = x[10] - x[2];
           x[10] += x[2];
           x[11] += x[3];
           x[2]   = r0;
           x[3]   = r1;

           r0     = x[12] - x[4];
           r1     = x[13] - x[5];
           x[12] += x[4];
           x[13] += x[5];
           x[4]   = MULT_NORM((r0   - r1) * cPI2_8);
           x[5]   = MULT_NORM((r0   + r1) * cPI2_8);

           r0     = x[14] - x[6];
           r1     = x[15] - x[7];
           x[14] += x[6];
           x[15] += x[7];
           x[6]  = r0;
           x[7]  = r1;

           mdct_butterfly_8(x);
           mdct_butterfly_8(x+8);
}

/* 32 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_32(DATA_TYPE *x){
  REG_TYPE r0     = x[30] - x[14];
  REG_TYPE r1     = x[31] - x[15];

           x[30] +=         x[14];
           x[31] +=         x[15];
           x[14]  =         r0;
           x[15]  =         r1;

           r0     = x[28] - x[12];
           r1     = x[29] - x[13];
           x[28] +=         x[12];
           x[29] +=         x[13];
           x[12]  = MULT_NORM( r0 * cPI1_8  -  r1 * cPI3_8 );
           x[13]  = MULT_NORM( r0 * cPI3_8  +  r1 * cPI1_8 );

           r0     = x[26] - x[10];
           r1     = x[27] - x[11];
           x[26] +=         x[10];
           x[27] +=         x[11];
           x[10]  = MULT_NORM(( r0  - r1 ) * cPI2_8);
           x[11]  = MULT_NORM(( r0  + r1 ) * cPI2_8);

           r0     = x[24] - x[8];
           r1     = x[25] - x[9];
           x[24] += x[8];
           x[25] += x[9];
           x[8]   = MULT_NORM( r0 * cPI3_8  -  r1 * cPI1_8 );
           x[9]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );

           r0     = x[22] - x[6];
           r1     = x[7]  - x[23];
           x[22] += x[6];
           x[23] += x[7];
           x[6]   = r1;
           x[7]   = r0;

           r0     = x[4]  - x[20];
           r1     = x[5]  - x[21];
           x[20] += x[4];
           x[21] += x[5];
           x[4]   = MULT_NORM( r1 * cPI1_8  +  r0 * cPI3_8 );
           x[5]   = MULT_NORM( r1 * cPI3_8  -  r0 * cPI1_8 );

           r0     = x[2]  - x[18];
           r1     = x[3]  - x[19];
           x[18] += x[2];
           x[19] += x[3];
           x[2]   = MULT_NORM(( r1  + r0 ) * cPI2_8);
           x[3]   = MULT_NORM(( r1  - r0 ) * cPI2_8);

           r0     = x[0]  - x[16];
           r1     = x[1]  - x[17];
           x[16] += x[0];
           x[17] += x[1];
           x[0]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );
           x[1]   = MULT_NORM( r1 * cPI1_8  -  r0 * cPI3_8 );

           mdct_butterfly_16(x);
           mdct_butterfly_16(x+16);

}

/* N point first stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_first(DATA_TYPE *T,
                                        DATA_TYPE *x,
                                        int points){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;

#if defined(HAVE_SSE2)
  do{
    /* 4 sub-blocks per iteration; split even/odd lanes, apply the same
       rotations, then interleave back. Trig laid out as 4 (c,s) pairs. */
    __m128 a1lo=_mm_loadu_ps(x1), a1hi=_mm_loadu_ps(x1+4);
    __m128 a2lo=_mm_loadu_ps(x2), a2hi=_mm_loadu_ps(x2+4);
    __m128 x1e=_mm_shuffle_ps(a1lo,a1hi,_MM_SHUFFLE(2,0,2,0));
    __m128 x1o=_mm_shuffle_ps(a1lo,a1hi,_MM_SHUFFLE(3,1,3,1));
    __m128 x2e=_mm_shuffle_ps(a2lo,a2hi,_MM_SHUFFLE(2,0,2,0));
    __m128 x2o=_mm_shuffle_ps(a2lo,a2hi,_MM_SHUFFLE(3,1,3,1));
    /* lanes are blocks e0,e2,e4,e6; scalar gives each block a fixed
       (c,s) pair at stride 4: e0->T[12,13] ... e6->T[0,1]. Gather to
       match lanes exactly (no implicit block reversal). */
    __m128 c=_mm_set_ps(T[0], T[4], T[8],  T[12]);
    __m128 s=_mm_set_ps(T[1], T[5], T[9],  T[13]);
    __m128 d0=_mm_sub_ps(x1e,x2e), d1=_mm_sub_ps(x1o,x2o);
    __m128 n1e=_mm_add_ps(x1e,x2e), n1o=_mm_add_ps(x1o,x2o);
    __m128 n2a=_mm_add_ps(_mm_mul_ps(d1,s),_mm_mul_ps(d0,c));
    __m128 n2b=_mm_sub_ps(_mm_mul_ps(d1,c),_mm_mul_ps(d0,s));
    /* lanes are blocks e0,e2,e4,e6; write through a small temp so the
       output lands in ascending memory order. */
    float te[4], to[4], ue[4], uo[4];
    _mm_storeu_ps(te, n1e); _mm_storeu_ps(to, n1o);
    _mm_storeu_ps(ue, n2a); _mm_storeu_ps(uo, n2b);
    for(int k=0;k<4;k++){
      x1[2*k]=te[k]; x1[2*k+1]=to[k];
      x2[2*k]=ue[k]; x2[2*k+1]=uo[k];
    }
    x1-=8;
    x2-=8;
    T+=16;
  }while(x2>=x);
#else
  REG_TYPE r0;
  REG_TYPE r1;
  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[5]  +  r0 * T[4]);
               x2[5]   = MULT_NORM(r1 * T[4]  -  r0 * T[5]);

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[9]  +  r0 * T[8]);
               x2[3]   = MULT_NORM(r1 * T[8]  -  r0 * T[9]);

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[13] +  r0 * T[12]);
               x2[1]   = MULT_NORM(r1 * T[12] -  r0 * T[13]);

    x1-=8;
    x2-=8;
    T+=16;

  }while(x2>=x);
#endif
}

/* N/stage point generic N stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_generic(DATA_TYPE *T,
                                          DATA_TYPE *x,
                                          int points,
                                          int trigint){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;

#if defined(HAVE_SSE2)
  do{
    __m128 a1lo=_mm_loadu_ps(x1), a1hi=_mm_loadu_ps(x1+4);
    __m128 a2lo=_mm_loadu_ps(x2), a2hi=_mm_loadu_ps(x2+4);
    __m128 x1e=_mm_shuffle_ps(a1lo,a1hi,_MM_SHUFFLE(2,0,2,0));
    __m128 x1o=_mm_shuffle_ps(a1lo,a1hi,_MM_SHUFFLE(3,1,3,1));
    __m128 x2e=_mm_shuffle_ps(a2lo,a2hi,_MM_SHUFFLE(2,0,2,0));
    __m128 x2o=_mm_shuffle_ps(a2lo,a2hi,_MM_SHUFFLE(3,1,3,1));
    /* scalar consumes the four (c,s) pairs in e6..e0 order, so gather
       them reversed onto lanes (e0,e2,e4,e6); output stores forward. */
    __m128 c=_mm_set_ps(T[0], T[trigint], T[2*trigint], T[3*trigint]);
    __m128 s=_mm_set_ps(T[1], T[trigint+1], T[2*trigint+1], T[3*trigint+1]);
    __m128 d0=_mm_sub_ps(x1e,x2e), d1=_mm_sub_ps(x1o,x2o);
    __m128 n1e=_mm_add_ps(x1e,x2e), n1o=_mm_add_ps(x1o,x2o);
    __m128 n2a=_mm_add_ps(_mm_mul_ps(d1,s),_mm_mul_ps(d0,c));
    __m128 n2b=_mm_sub_ps(_mm_mul_ps(d1,c),_mm_mul_ps(d0,s));
    float te[4], to[4], ue[4], uo[4];
    _mm_storeu_ps(te, n1e); _mm_storeu_ps(to, n1o);
    _mm_storeu_ps(ue, n2a); _mm_storeu_ps(uo, n2b);
    for(int k=0;k<4;k++){
      x1[2*k]=te[k]; x1[2*k+1]=to[k];
      x2[2*k]=ue[k]; x2[2*k+1]=uo[k];
    }
    T+=4*trigint;
    x1-=8;
    x2-=8;
  }while(x2>=x);
#else
  REG_TYPE r0;
  REG_TYPE r1;
  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[5]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[3]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[1]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;
    x1-=8;
    x2-=8;

  }while(x2>=x);
#endif
}

STIN void mdct_butterflies(mdct_lookup *init,
                             DATA_TYPE *x,
                             int points){

  DATA_TYPE *T=init->trig;
  int stages=init->log2n-5;
  int i,j;

  if(--stages>0){
    mdct_butterfly_first(T,x,points);
  }

  for(i=1;--stages>0;i++){
    for(j=0;j<(1<<i);j++)
      mdct_butterfly_generic(T,x+(points>>i)*j,points>>i,4<<i);
  }

  for(j=0;j<points;j+=32)
    mdct_butterfly_32(x+j);

}

void mdct_clear(mdct_lookup *l){
  if(l){
    if(l->trig)_ogg_free(l->trig);
    if(l->bitrev)_ogg_free(l->bitrev);
    memset(l,0,sizeof(*l));
  }
}

STIN void mdct_bitreverse(mdct_lookup *init,
                            DATA_TYPE *x){
  int        n       = init->n;
  int       *bit     = init->bitrev;
  DATA_TYPE *w0      = x;
  DATA_TYPE *w1      = x = w0+(n>>1);
  DATA_TYPE *T       = init->trig+n;

#if defined(HAVE_SSE2)
  const __m128 half = _mm_set1_ps(0.5f);
  do{
    /* two independent (x0,x1) pairs per scalar iteration; gather the
       four [0] and [1] operands into 4-lane vectors, then evaluate the
       exact same sub/add/mul sequence per lane. */
    float *a0 = x+bit[0], *a1 = x+bit[1];
    float *b0 = x+bit[2], *b1 = x+bit[3];
    __m128 x0v = _mm_set_ps(b0[1], b0[0], a0[1], a0[0]);
    __m128 x1v = _mm_set_ps(b1[1], b1[0], a1[1], a1[0]);
    __m128 tv = _mm_loadu_ps(T);                          /* T0 T1 T2 T3 */
    __m128 tA = _mm_shuffle_ps(tv,tv,_MM_SHUFFLE(2,2,0,0)); /* T0 T0 T2 T2 */
    __m128 tB = _mm_shuffle_ps(tv,tv,_MM_SHUFFLE(3,3,1,1)); /* T1 T1 T3 T3 */

    __m128 x0lo = _mm_shuffle_ps(x0v,x0v,_MM_SHUFFLE(2,2,0,0)); /* x0[0] pair */
    __m128 x0hi = _mm_shuffle_ps(x0v,x0v,_MM_SHUFFLE(3,3,1,1)); /* x0[1] pair */
    __m128 x1lo = _mm_shuffle_ps(x1v,x1v,_MM_SHUFFLE(2,2,0,0));
    __m128 x1hi = _mm_shuffle_ps(x1v,x1v,_MM_SHUFFLE(3,3,1,1));

    __m128 d = _mm_sub_ps(x0hi, x1hi);           /* r0 = x0[1]-x1[1] */
    __m128 s = _mm_add_ps(x0lo, x1lo);           /* r1 = x0[0]+x1[0] */
    __m128 r2 = _mm_add_ps(_mm_mul_ps(s,tA), _mm_mul_ps(d,tB));
    __m128 r3 = _mm_sub_ps(_mm_mul_ps(s,tB), _mm_mul_ps(d,tA));

    w1 -= 4;
    __m128 r0h = _mm_mul_ps(_mm_add_ps(x0hi,x1hi), half);
    __m128 r1h = _mm_mul_ps(_mm_sub_ps(x0lo,x1lo), half);

    /* repack (a,b) halves then interleave into scalar output order */
    __m128 z0  = _mm_shuffle_ps(r0h, r1h, _MM_SHUFFLE(2,0,2,0));
    __m128 z2  = _mm_shuffle_ps(r2,  r3,  _MM_SHUFFLE(2,0,2,0));
    __m128 h01 = _mm_shuffle_ps(z0, z0, _MM_SHUFFLE(3,1,2,0)); /* r0a r1a r0b r1b */
    __m128 r23 = _mm_shuffle_ps(z2, z2, _MM_SHUFFLE(3,1,2,0)); /* r2a r3a r2b r3b */
    _mm_storeu_ps(w0, _mm_add_ps(h01, r23));
    __m128 subv = _mm_sub_ps(r23, h01);
    subv = _mm_shuffle_ps(subv, subv, _MM_SHUFFLE(1,0,3,2));
    subv = _mm_xor_ps(subv, _mm_set_ps(0.f, -0.f, 0.f, -0.f));
    _mm_storeu_ps(w1, subv);

    T     += 4;
    bit   += 4;
    w0    += 4;
  }while(w0<w1);
#else
  do{
    DATA_TYPE *x0    = x+bit[0];
    DATA_TYPE *x1    = x+bit[1];

    REG_TYPE  r0     = x0[1]  - x1[1];
    REG_TYPE  r1     = x0[0]  + x1[0];
    REG_TYPE  r2     = MULT_NORM(r1     * T[0]   + r0 * T[1]);
    REG_TYPE  r3     = MULT_NORM(r1     * T[1]   - r0 * T[0]);

              w1    -= 4;

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[0]  = r0     + r2;
              w1[2]  = r0     - r2;
              w0[1]  = r1     + r3;
              w1[3]  = r3     - r1;

              x0     = x+bit[2];
              x1     = x+bit[3];

              r0     = x0[1]  - x1[1];
              r1     = x0[0]  + x1[0];
              r2     = MULT_NORM(r1     * T[2]   + r0 * T[3]);
              r3     = MULT_NORM(r1     * T[3]   - r0 * T[2]);

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[2]  = r0     + r2;
              w1[0]  = r0     - r2;
              w0[3]  = r1     + r3;
              w1[1]  = r3     - r1;

              T     += 4;
              bit   += 4;
              w0    += 4;

  }while(w0<w1);
#endif
}

void mdct_backward(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out){
  int n=init->n;
  int n2=n>>1;
  int n4=n>>2;

  /* rotate */

  DATA_TYPE *iX = in+n2-7;
  DATA_TYPE *oX = out+n2+n4;
  DATA_TYPE *T  = init->trig+n4;

#if defined(HAVE_SSE2)
  do{
    oX         -= 4;
    /* Lane k holds exactly the two scalar products of oX[k], then one
       add; identical operation order to the scalar path. */
    __m128 pa = _mm_set_ps( iX[4]*T[1], -iX[6]*T[1],  iX[0]*T[3], -iX[2]*T[3]);
    __m128 pb = _mm_set_ps(-iX[6]*T[0], -iX[4]*T[0], -iX[2]*T[2], -iX[0]*T[2]);
    _mm_storeu_ps(oX, _mm_add_ps(pa, pb));
    iX         -= 8;
    T          += 4;
  }while(iX>=in);
#else
  do{
    oX         -= 4;
    oX[0]       = MULT_NORM(-iX[2] * T[3] - iX[0]  * T[2]);
    oX[1]       = MULT_NORM (iX[0] * T[3] - iX[2]  * T[2]);
    oX[2]       = MULT_NORM(-iX[6] * T[1] - iX[4]  * T[0]);
    oX[3]       = MULT_NORM (iX[4] * T[1] - iX[6]  * T[0]);
    iX         -= 8;
    T          += 4;
  }while(iX>=in);
#endif

  iX            = in+n2-8;
  oX            = out+n2+n4;
  T             = init->trig+n4;

#if defined(HAVE_SSE2)
  do{
    T          -= 4;
    __m128 pa  = _mm_set_ps(iX[0]*T[0], iX[0]*T[1], iX[4]*T[2], iX[4]*T[3]);
    __m128 pb  = _mm_set_ps(-iX[2]*T[1], iX[2]*T[0], -iX[6]*T[3], iX[6]*T[2]);
    _mm_storeu_ps(oX, _mm_add_ps(pa, pb));
    iX         -= 8;
    oX         += 4;
  }while(iX>=in);
#else
  do{
    T          -= 4;
    oX[0]       =  MULT_NORM (iX[4] * T[3] + iX[6] * T[2]);
    oX[1]       =  MULT_NORM (iX[4] * T[2] - iX[6] * T[3]);
    oX[2]       =  MULT_NORM (iX[0] * T[1] + iX[2] * T[0]);
    oX[3]       =  MULT_NORM (iX[0] * T[0] - iX[2] * T[1]);
    iX         -= 8;
    oX         += 4;
  }while(iX>=in);
#endif

  mdct_butterflies(init,out+n2,n2);
  mdct_bitreverse(init,out);

  /* roatate + window */

  {
    DATA_TYPE *oX1=out+n2+n4;
    DATA_TYPE *oX2=out+n2+n4;
    DATA_TYPE *iX =out;
    T             =init->trig+n2;

#if defined(HAVE_SSE2)
    do{
      oX1-=4;
      /* oX1 lanes [3..0] and oX2 lanes [0..3], same products/order as scalar */
      __m128 a1 = _mm_set_ps(iX[0]*T[1], iX[2]*T[3], iX[4]*T[5], iX[6]*T[7]);
      __m128 b1 = _mm_set_ps(-iX[1]*T[0], -iX[3]*T[2], -iX[5]*T[4], -iX[7]*T[6]);
      _mm_storeu_ps(oX1, _mm_add_ps(a1, b1));
      __m128 a2 = _mm_set_ps(-iX[6]*T[6], -iX[4]*T[4], -iX[2]*T[2], -iX[0]*T[0]);
      __m128 b2 = _mm_set_ps(-iX[7]*T[7], -iX[5]*T[5], -iX[3]*T[3], -iX[1]*T[1]);
      _mm_storeu_ps(oX2, _mm_add_ps(a2, b2));
      oX2+=4;
      iX    +=   8;
      T     +=   8;
    }while(iX<oX1);
#else
    do{
      oX1-=4;

      oX1[3]  =  MULT_NORM (iX[0] * T[1] - iX[1] * T[0]);
      oX2[0]  = -MULT_NORM (iX[0] * T[0] + iX[1] * T[1]);

      oX1[2]  =  MULT_NORM (iX[2] * T[3] - iX[3] * T[2]);
      oX2[1]  = -MULT_NORM (iX[2] * T[2] + iX[3] * T[3]);

      oX1[1]  =  MULT_NORM (iX[4] * T[5] - iX[5] * T[4]);
      oX2[2]  = -MULT_NORM (iX[4] * T[4] + iX[5] * T[5]);

      oX1[0]  =  MULT_NORM (iX[6] * T[7] - iX[7] * T[6]);
      oX2[3]  = -MULT_NORM (iX[6] * T[6] + iX[7] * T[7]);

      oX2+=4;
      iX    +=   8;
      T     +=   8;
    }while(iX<oX1);
#endif

    iX=out+n2+n4;
    oX1=out+n4;
    oX2=oX1;

#if defined(HAVE_SSE2)
    do{
      oX1-=4;
      iX-=4;
      /* oX1 and oX2 alias the same region; replicate the scalar's
         in-place reversed assignment exactly, element by element. */
      oX1[3] = iX[3]; oX2[0] = -oX1[3];
      oX1[2] = iX[2]; oX2[1] = -oX1[2];
      oX1[1] = iX[1]; oX2[2] = -oX1[1];
      oX1[0] = iX[0]; oX2[3] = -oX1[0];
      oX2+=4;
    }while(oX2<iX);
#else
    do{
      oX1-=4;
      iX-=4;

      oX2[0] = -(oX1[3] = iX[3]);
      oX2[1] = -(oX1[2] = iX[2]);
      oX2[2] = -(oX1[1] = iX[1]);
      oX2[3] = -(oX1[0] = iX[0]);

      oX2+=4;
    }while(oX2<iX);
#endif

    iX=out+n2+n4;
    oX1=out+n2+n4;
    oX2=out+n2;
    do{
      oX1-=4;
      oX1[0]= iX[3];
      oX1[1]= iX[2];
      oX1[2]= iX[1];
      oX1[3]= iX[0];
      iX+=4;
    }while(oX1>oX2);
  }
}

void mdct_forward(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out){
  int n=init->n;
  int n2=n>>1;
  int n4=n>>2;
  int n8=n>>3;
  DATA_TYPE *w=alloca(n*sizeof(*w)); /* forward needs working space */
  DATA_TYPE *w2=w+n2;

  /* rotate */

  /* window + rotate + step 1 */

  REG_TYPE r0;
  REG_TYPE r1;
  DATA_TYPE *x0=in+n2+n4;
  DATA_TYPE *x1=x0+1;
  DATA_TYPE *T=init->trig+n2;

  int i=0;

  for(i=0;i<n8;i+=2){
    x0 -=4;
    T-=2;
    r0= x0[2] + x1[0];
    r1= x0[0] + x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }

  x1=in+1;

  for(;i<n2-n8;i+=2){
    T-=2;
    x0 -=4;
    r0= x0[2] - x1[0];
    r1= x0[0] - x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }

  x0=in+n;

  for(;i<n2;i+=2){
    T-=2;
    x0 -=4;
    r0= -x0[2] - x1[0];
    r1= -x0[0] - x1[2];
    w2[i]=   MULT_NORM(r1*T[1] + r0*T[0]);
    w2[i+1]= MULT_NORM(r1*T[0] - r0*T[1]);
    x1 +=4;
  }


  mdct_butterflies(init,w+n2,n2);
  mdct_bitreverse(init,w);

  /* roatate + window */

  T=init->trig+n2;
  x0=out+n2;

  for(i=0;i<n4;i++){
    x0--;
    out[i] =MULT_NORM((w[0]*T[0]+w[1]*T[1])*init->scale);
    x0[0]  =MULT_NORM((w[0]*T[1]-w[1]*T[0])*init->scale);
    w+=2;
    T+=2;
  }
}
