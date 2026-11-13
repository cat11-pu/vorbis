/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2026             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: verify sample-accurate seek consistency.

 Encodes a deterministic test signal, decodes it sequentially as the
 reference, then re-decodes via random sample-position seeks and
 verifies the output is bit-identical to the reference at the exact
 requested offsets.  The position error after each seek must be zero
 samples (spec requires it to be well under one 2048-sample frame).

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "write_read.h"
#include "../examples/seek_decode.h"

#define SRATE 44100
#define CHANNELS 2
#define FRAMES 200000            /* ~4.5 s */
#define LONG_BLOCK 2048          /* one "frame" of error tolerance */
#define CHUNK 8192               /* frames read after each seek */
#define RANDOM_SEEKS 512

static const char *testfile="seek_consistency_test.ogg";

/* deterministic 64-bit LCG so runs are reproducible */
static unsigned long long rng_state=0x9e3779b97f4a7c15ULL;
static unsigned long long rng_next(void){
  rng_state=rng_state*6364136223846793005ULL+1442695040888963407ULL;
  return rng_state;
}

static void make_signal(float *data,int n){
  /* swept sine + periodic clicks (force short blocks) + noise:
     exercises long/short block switching and all lap types */
  double phase=0.0;
  int i;
  for(i=0;i<n;i++){
    double freq=220.0*pow(4000.0/220.0,(double)i/n);
    double s;
    phase+=2.0*M_PI*freq/SRATE;
    s=0.55*sin(phase);
    if(i%17333<4)s+=0.3*((i%17333)-2); /* transient */
    s+=0.04*((double)(rng_next()>>33)/2147483648.0-1.0);
    if(s>0.99)s=0.99;
    if(s<-0.99)s=-0.99;
    data[i]=(float)s;
  }
}

int main(void){
  float *signal=NULL;
  short *ref=NULL,*got=NULL;
  ogg_int64_t total=0;
  int channels=0;
  long rate=0;
  VsdDecoder *d;
  ogg_int64_t max_pos_err=0;
  long max_sample_diff=0;
  unsigned long long samples_compared=0;
  int seeks_done=0,failures=0;
  int i;

  signal=malloc(sizeof(float)*FRAMES);
  ref=malloc(sizeof(short)*FRAMES*CHANNELS);
  got=malloc(sizeof(short)*CHUNK*CHANNELS);
  if(!signal||!ref||!got){
    fprintf(stderr,"out of memory\n");
    return 1;
  }

  /* 1. encode deterministic test stream */
  make_signal(signal,FRAMES);
  write_vorbis_data_or_die(testfile,SRATE,0.4f,signal,FRAMES,CHANNELS);

  /* 2. sequential decode = reference */
  d=vsd_open(testfile,&channels,&rate,&total);
  if(!d){
    fprintf(stderr,"FAIL: cannot open %s\n",testfile);
    return 1;
  }
  if(channels!=CHANNELS||rate!=SRATE||total!=FRAMES){
    fprintf(stderr,"FAIL: stream params ch=%d rate=%ld total=%lld "
            "(expected %d/%d/%d)\n",channels,rate,(long long)total,
            CHANNELS,SRATE,FRAMES);
    return 1;
  }
  {
    ogg_int64_t off=0;
    while(off<total){
      long r=vsd_read(d,ref+off*channels,(long)(total-off));
      if(r<=0)break;
      off+=r;
    }
    if(off!=total){
      fprintf(stderr,"FAIL: sequential decode got %lld of %lld frames\n",
              (long long)off,(long long)total);
      return 1;
    }
  }

  /* 3. seek to fixed edge positions + random positions, decode a
   *    chunk from each, compare bit-exactly against the reference */
  {
    ogg_int64_t fixed[]={0,1,2,255,256,257,511,512,1023,1024,
                         2047,2048,2049,4095,4096,4097,
                         FRAMES/4,FRAMES/2,3*FRAMES/4,
                         FRAMES-CHUNK,FRAMES-1,FRAMES};
    int nfixed=sizeof(fixed)/sizeof(fixed[0]);
    int npos=nfixed+RANDOM_SEEKS;
    for(i=0;i<npos;i++){
      ogg_int64_t pos;
      ogg_int64_t tell;
      long want,gotframes;
      if(i<nfixed)
        pos=fixed[i];
      else
        pos=(ogg_int64_t)(rng_next()%(unsigned long long)(FRAMES+1));

      if(vsd_seek(d,pos)!=0){
        fprintf(stderr,"FAIL: vsd_seek(%lld) returned error\n",
                (long long)pos);
        failures++;
        continue;
      }
      tell=vsd_tell(d);
      if(tell!=pos){
        ogg_int64_t err=tell>pos?tell-pos:pos-tell;
        if(err>max_pos_err)max_pos_err=err;
        fprintf(stderr,"FAIL: position after seek(%lld) is %lld\n",
                (long long)pos,(long long)tell);
        failures++;
        continue;
      }
      want=(long)(total-pos<CHUNK?total-pos:CHUNK);
      gotframes=vsd_read(d,got,want);
      if(gotframes!=want){
        fprintf(stderr,"FAIL: read after seek(%lld) got %ld of %ld\n",
                (long long)pos,gotframes,want);
        failures++;
        continue;
      }
      {
        long j,n=gotframes*channels;
        short *rp=ref+pos*channels;
        for(j=0;j<n;j++){
          long diff=got[j]-rp[j];
          if(diff<0)diff=-diff;
          if(diff>max_sample_diff)max_sample_diff=diff;
          if(diff){
            fprintf(stderr,"FAIL: sample mismatch at frame %lld ch %d: "
                    "seek=%d seq=%d\n",(long long)(pos+j/channels),
                    (int)(j%channels),got[j],rp[j]);
            failures++;
            break;
          }
        }
        samples_compared+=n;
      }
      seeks_done++;
    }
  }

  /* 4. random-walk pass: cover every sample of the file exactly once,
   *    each chunk beginning immediately after a fresh seek */
  {
    ogg_int64_t pos=0;
    while(pos<total){
      long want=1+(long)(rng_next()%4096);
      long r;
      if(pos+want>total)want=(long)(total-pos);
      if(vsd_seek(d,pos)!=0){
        fprintf(stderr,"FAIL: walk seek(%lld) error\n",(long long)pos);
        failures++;
        break;
      }
      r=vsd_read(d,got,want);
      if(r!=want||memcmp(got,ref+pos*channels,
                         (size_t)r*channels*sizeof(short))!=0){
        fprintf(stderr,"FAIL: walk mismatch at %lld\n",(long long)pos);
        failures++;
        break;
      }
      samples_compared+=(unsigned long long)r*channels;
      pos+=r;
    }
  }

  /* 5. out-of-range seeks must fail cleanly */
  if(vsd_seek(d,total+1)==0||vsd_seek(d,-1)==0){
    fprintf(stderr,"FAIL: out-of-range seek accepted\n");
    failures++;
  }

  vsd_close(d);
  remove(testfile);

  printf("seek consistency data:\n");
  printf("  stream:              %d ch, %ld Hz, %lld frames\n",
         channels,rate,(long long)total);
  printf("  seeks performed:     %d (fixed edges + random + walk)\n",
         seeks_done);
  printf("  samples compared:    %llu\n",samples_compared);
  printf("  max position error:  %lld samples (%.4f of a %d-sample frame)\n",
         (long long)max_pos_err,(double)max_pos_err/LONG_BLOCK,LONG_BLOCK);
  printf("  max sample diff:     %ld / 32768\n",max_sample_diff);
  printf("  result:              %s\n",failures?"FAIL":"PASS");

  free(signal);
  free(ref);
  free(got);
  return failures?1:0;
}
