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

 function: reuse codebooks and decoder lookups across compatible tracks

 ********************************************************************/

#include <stdlib.h>
#include <string.h>

#include <vorbis/codec.h>
#include <vorbis/reuse.h>

#include "codec_internal.h"

struct vorbis_reuse_decoder{
  vorbis_info    *active_info;
  vorbis_comment *active_comment;
  vorbis_dsp_state dsp;
  vorbis_block     block;
  int              have_engine;
  int              track_ready;

  vorbis_info    *next_info;
  vorbis_comment *next_comment;

  unsigned char  *setup;
  long            setup_bytes;

  vorbis_reuse_stats stats;
};

static vorbis_info *new_info(void){
  vorbis_info *vi=malloc(sizeof(*vi));
  if(vi){
    vorbis_info_init(vi);
    if(!vi->codec_setup){
      free(vi);
      return NULL;
    }
  }
  return vi;
}

static vorbis_comment *new_comment(void){
  vorbis_comment *vc=malloc(sizeof(*vc));
  if(vc)vorbis_comment_init(vc);
  return vc;
}

static void free_info(vorbis_info *vi){
  if(vi){
    vorbis_info_clear(vi);
    free(vi);
  }
}

static void free_comment(vorbis_comment *vc){
  if(vc){
    vorbis_comment_clear(vc);
    free(vc);
  }
}

static void reset_next(vorbis_reuse_decoder *r){
  free_info(r->next_info);
  free_comment(r->next_comment);
  r->next_info=new_info();
  r->next_comment=new_comment();
}

static void clear_engine(vorbis_reuse_decoder *r){
  if(r->have_engine){
    vorbis_block_clear(&r->block);
    vorbis_dsp_clear(&r->dsp);
    free_comment(r->active_comment);
    free_info(r->active_info);
  }
  memset(&r->dsp,0,sizeof(r->dsp));
  memset(&r->block,0,sizeof(r->block));
  r->active_info=NULL;
  r->active_comment=NULL;
  r->have_engine=0;
  r->track_ready=0;
}

/* Reset every piece of state that a fresh vorbis_synthesis_init() would
   start from, without touching the codebooks or backend lookups. */
static void restart_engine(vorbis_reuse_decoder *r){
  int i;

  vorbis_synthesis_restart(&r->dsp);

  if(r->dsp.pcm && r->active_info){
    for(i=0;i<r->active_info->channels;i++)
      if(r->dsp.pcm[i])
        memset(r->dsp.pcm[i],0,
               (size_t)r->dsp.pcm_storage*sizeof(*r->dsp.pcm[i]));
  }

  r->dsp.lW=0;
  r->dsp.W=0;
  r->dsp.nW=-1;
  r->dsp.glue_bits=0;
  r->dsp.time_bits=0;
  r->dsp.floor_bits=0;
  r->dsp.res_bits=0;

  vorbis_block_clear(&r->block);
  vorbis_block_init(&r->dsp,&r->block);
  r->block.vd=&r->dsp;
}

static int same_setup(const vorbis_reuse_decoder *r,
                      const vorbis_info *next,
                      const ogg_packet *op){
  const vorbis_info *active=r->active_info;
  codec_setup_info *ca;
  codec_setup_info *cn;

  if(!r->have_engine || !active || !next ||
     !active->codec_setup || !next->codec_setup)
    return 0;

  if(active->version!=next->version ||
     active->channels!=next->channels ||
     active->rate!=next->rate)
    return 0;

  ca=(codec_setup_info *)active->codec_setup;
  cn=(codec_setup_info *)next->codec_setup;
  if(ca->blocksizes[0]!=cn->blocksizes[0] ||
     ca->blocksizes[1]!=cn->blocksizes[1])
    return 0;

  if(!r->setup || r->setup_bytes!=op->bytes)
    return 0;

  return memcmp(r->setup,op->packet,(size_t)op->bytes)==0;
}

static unsigned char *copy_packet(const ogg_packet *op){
  unsigned char *ret;
  if(op->bytes<=0)return NULL;
  ret=malloc((size_t)op->bytes);
  if(ret)memcpy(ret,op->packet,(size_t)op->bytes);
  return ret;
}

vorbis_reuse_decoder *vorbis_reuse_new(void){
  vorbis_reuse_decoder *r=calloc(1,sizeof(*r));
  if(!r)return NULL;

  r->next_info=new_info();
  r->next_comment=new_comment();
  if(!r->next_info || !r->next_comment){
    vorbis_reuse_free(r);
    return NULL;
  }
  return r;
}

void vorbis_reuse_free(vorbis_reuse_decoder *r){
  if(r){
    clear_engine(r);
    free_info(r->next_info);
    free_comment(r->next_comment);
    free(r->setup);
    free(r);
  }
}

int vorbis_reuse_headerin(vorbis_reuse_decoder *r, ogg_packet *op){
  int packet_type;
  int ret;

  if(!r || !op || !op->packet || op->bytes<=0)
    return OV_EINVAL;
  if(!r->next_info || !r->next_comment)
    return OV_EFAULT;

  packet_type=op->packet[0];

  if(packet_type==0x01){
    /* Starting a new track while old samples are still unread would make
       the caller unable to tell which track those samples belong to. */
    if(r->have_engine && r->dsp.pcm_returned!=-1 &&
       r->dsp.pcm_current>r->dsp.pcm_returned)
      return OV_EINVAL;

    reset_next(r);
    if(!r->next_info || !r->next_comment)
      return OV_EFAULT;
    r->track_ready=0;
  }

  if(packet_type==0x05 && r->have_engine &&
     same_setup(r,r->next_info,op)){
    /* The setup is the same, so all codebooks and floor/residue lookups
       remain valid.  Only per-track timeline and overlap state changes. */
    free_info(r->next_info);
    free_comment(r->next_comment);
    r->next_info=new_info();
    r->next_comment=new_comment();
    if(!r->next_info || !r->next_comment)
      return OV_EFAULT;

    restart_engine(r);
    r->track_ready=1;
    r->stats.tracks++;
    r->stats.setup_reused++;
    return 0;
  }

  ret=vorbis_synthesis_headerin(r->next_info,r->next_comment,op);
  if(ret){
    /* _vorbis_unpack_* clears partially built structures on failure.
       Recreate an empty header pair so a later valid track can start. */
    reset_next(r);
    return ret;
  }

  if(packet_type==0x05){
    unsigned char *new_setup;
    vorbis_info *fresh_info;
    vorbis_comment *fresh_comment;
    vorbis_dsp_state new_dsp;
    vorbis_block new_block;

    new_setup=copy_packet(op);
    fresh_info=new_info();
    fresh_comment=new_comment();
    if(!new_setup || !fresh_info || !fresh_comment){
      free(new_setup);
      free_info(fresh_info);
      free_comment(fresh_comment);
      reset_next(r);
      return OV_EFAULT;
    }

    memset(&new_dsp,0,sizeof(new_dsp));
    memset(&new_block,0,sizeof(new_block));
    if(vorbis_synthesis_init(&new_dsp,r->next_info)){
      free(new_setup);
      free_info(fresh_info);
      free_comment(fresh_comment);
      reset_next(r);
      return OV_EBADHEADER;
    }
    vorbis_block_init(&new_dsp,&new_block);
    new_block.vd=&new_dsp;

    clear_engine(r);

    r->dsp=new_dsp;
    r->block=new_block;
    r->block.vd=&r->dsp;
    r->active_info=r->next_info;
    r->active_comment=r->next_comment;
    r->next_info=fresh_info;
    r->next_comment=fresh_comment;
    r->have_engine=1;
    r->track_ready=1;

    free(r->setup);
    r->setup=new_setup;
    r->setup_bytes=op->bytes;

    r->stats.tracks++;
    r->stats.setup_rebuilt++;
  }

  return 0;
}

const vorbis_info *vorbis_reuse_info(const vorbis_reuse_decoder *r){
  if(!r)return NULL;
  if(r->track_ready)return r->active_info;
  if(r->next_info && r->next_info->rate)return r->next_info;
  return r->active_info;
}

int vorbis_reuse_decode(vorbis_reuse_decoder *r, ogg_packet *op){
  int ret;
  if(!r || !r->track_ready || !op)
    return OV_EINVAL;

  ret=vorbis_synthesis(&r->block,op);
  if(ret)return ret;
  ret=vorbis_synthesis_blockin(&r->dsp,&r->block);
  if(ret)return ret;

  r->stats.blocks++;
  return 0;
}

int vorbis_reuse_pcmout(vorbis_reuse_decoder *r, float ***pcm){
  if(!r || !r->track_ready)return 0;
  return vorbis_synthesis_pcmout(&r->dsp,pcm);
}

int vorbis_reuse_read(vorbis_reuse_decoder *r, int n){
  int ret;
  if(!r || !r->track_ready)return OV_EINVAL;
  ret=vorbis_synthesis_read(&r->dsp,n);
  if(ret==0 && n>0)r->stats.samples+=(unsigned long long)n;
  return ret;
}

void vorbis_reuse_getstats(const vorbis_reuse_decoder *r,
                           vorbis_reuse_stats *stats){
  if(stats){
    if(r)*stats=r->stats;
    else memset(stats,0,sizeof(*stats));
  }
}
