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

 function: sample-accurate seekable decoding interface.

 Implemented on top of libvorbisfile.  ov_pcm_seek() performs the
 actual work: it bisects the stream on page granulepos to find the
 last page whose granulepos is at or before the target (the nearest
 reconstruction point), backs up one page so the first decoded block
 has its left neighbour for MDCT lapping, then decodes forward and
 discards samples until the exact target is reached.  Decoding is
 deterministic, so the result is bit-identical to sequential decode.

 ********************************************************************/

#include <stdlib.h>
#include <string.h>

#include <vorbis/vorbisfile.h>

#include "seek_decode.h"

struct VsdDecoder {
  OggVorbis_File vf;
  int channels;
  long rate;
  ogg_int64_t total;
};

VsdDecoder *vsd_open(const char *path,
                     int *channels, long *rate,
                     ogg_int64_t *total_samples){
  VsdDecoder *d=calloc(1,sizeof(*d));
  vorbis_info *vi;
  if(!d) return NULL;
  if(ov_fopen(path,&d->vf)<0){
    free(d);
    return NULL;
  }
  vi=ov_info(&d->vf,-1);
  d->channels=vi->channels;
  d->rate=vi->rate;
  d->total=ov_pcm_total(&d->vf,-1);
  if(channels)*channels=d->channels;
  if(rate)*rate=d->rate;
  if(total_samples)*total_samples=d->total;
  return d;
}

int vsd_seek(VsdDecoder *d, ogg_int64_t sample){
  if(!d || sample<0 || sample>d->total) return -1;
  return ov_pcm_seek(&d->vf,sample);
}

long vsd_read(VsdDecoder *d, short *out, long frames){
  long got=0;
  if(!d || !out || frames<0) return -1;
  while(got<frames){
    int section;
    long ret=ov_read(&d->vf,(char *)(out+(long)got*d->channels),
                     (int)((frames-got)*d->channels*sizeof(short)),
                     0 /* little endian */,2 /* 16 bit */,1 /* signed */,
                     &section);
    if(ret<=0){
      if(got>0)break;
      return ret; /* 0 = EOF, <0 = stream error */
    }
    got+=ret/(2*d->channels);
  }
  return got;
}

ogg_int64_t vsd_tell(VsdDecoder *d){
  if(!d) return -1;
  return ov_pcm_tell(&d->vf);
}

void vsd_close(VsdDecoder *d){
  if(!d)return;
  ov_clear(&d->vf);
  free(d);
}
