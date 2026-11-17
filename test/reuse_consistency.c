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

 function: verify codebook/residual-context reuse against fresh decodes

 ********************************************************************/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vorbis/codec.h>
#include <vorbis/reuse.h>
#include <vorbis/vorbisenc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAME_TRACKS 1000
#define ALT_SWITCHES 1000
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define BASE_FRAMES 512
#define OUTPUT_SLACK 8192

typedef struct{
  unsigned char *data;
  long           bytes;
  ogg_int64_t    granulepos;
  long           packetno;
  int            b_o_s;
  int            e_o_s;
} packet_copy;

typedef struct{
  int          channels;
  long         rate;
  int          frames;
  packet_copy  header[3];
  packet_copy *audio;
  int          audio_count;
  int          audio_cap;
} test_track;

typedef struct{
  unsigned long long samples;
  unsigned long long blocks;
  unsigned long long hash;
  int                failures;
} run_result;

static const unsigned long long FNV_OFFSET=1469598103934665603ULL;
static const unsigned long long FNV_PRIME=1099511628211ULL;

static void hash_bytes(unsigned long long *hash,const void *data,size_t bytes){
  const unsigned char *p=data;
  size_t i;
  for(i=0;i<bytes;i++){
    *hash^=p[i];
    *hash*=FNV_PRIME;
  }
}

static void free_packet(packet_copy *p){
  free(p->data);
  memset(p,0,sizeof(*p));
}

static int copy_packet(packet_copy *dst,const ogg_packet *src){
  if(src->bytes<=0)return -1;
  memset(dst,0,sizeof(*dst));
  dst->data=malloc((size_t)src->bytes);
  if(!dst->data)return -1;
  memcpy(dst->data,src->packet,(size_t)src->bytes);
  dst->bytes=src->bytes;
  dst->granulepos=src->granulepos;
  dst->packetno=src->packetno;
  dst->b_o_s=src->b_o_s;
  dst->e_o_s=src->e_o_s;
  return 0;
}

static ogg_packet packet_view(const packet_copy *p){
  ogg_packet op;
  memset(&op,0,sizeof(op));
  op.packet=p->data;
  op.bytes=p->bytes;
  op.granulepos=p->granulepos;
  op.packetno=p->packetno;
  op.b_o_s=p->b_o_s;
  op.e_o_s=p->e_o_s;
  return op;
}

static void free_track(test_track *t){
  int i;
  for(i=0;i<3;i++)free_packet(&t->header[i]);
  for(i=0;i<t->audio_count;i++)free_packet(&t->audio[i]);
  free(t->audio);
  memset(t,0,sizeof(*t));
}

static int append_audio(test_track *t,const ogg_packet *op){
  if(t->audio_count==t->audio_cap){
    int cap=t->audio_cap?t->audio_cap*2:16;
    packet_copy *p=realloc(t->audio,(size_t)cap*sizeof(*p));
    if(!p)return -1;
    t->audio=p;
    t->audio_cap=cap;
  }
  if(copy_packet(t->audio+t->audio_count,op))return -1;
  t->audio_count++;
  return 0;
}

static int encode_track(test_track *t,int channels,long rate,float quality,
                        int frames,int seed){
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_packet op;
  char tag[64];
  int i,j;
  int ret=0;

  memset(t,0,sizeof(*t));
  t->channels=channels;
  t->rate=rate;
  t->frames=frames;

  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);
  if(vorbis_encode_init_vbr(&vi,channels,rate,quality)){
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return -1;
  }

  snprintf(tag,sizeof(tag),"track-%d",seed);
  vorbis_comment_add_tag(&vc,"TRACK",tag);
  if(vorbis_analysis_init(&vd,&vi) || vorbis_block_init(&vd,&vb)){
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return -1;
  }

  {
    ogg_packet h1,h2,h3;
    if(vorbis_analysis_headerout(&vd,&vc,&h1,&h2,&h3)){
      ret=-1;
      goto done;
    }
    if(copy_packet(&t->header[0],&h1) ||
       copy_packet(&t->header[1],&h2) ||
       copy_packet(&t->header[2],&h3)){
      ret=-1;
      goto done;
    }
  }

  if(frames>0){
    float **buffer=vorbis_analysis_buffer(&vd,frames);
    if(!buffer){
      ret=-1;
      goto done;
    }
    for(j=0;j<channels;j++){
      double phase=0.0701*seed+0.31*j;
      double freq=173.0+((seed*37+j*53)%1200);
      for(i=0;i<frames;i++){
        double s=0.48*sin(phase)+0.12*sin(phase*2.713);
        phase+=2.0*M_PI*freq/rate;
        if(((i+seed)%257)<3)s+=0.22*((i+seed)%257-1);
        buffer[j][i]=(float)s;
      }
    }
    vorbis_analysis_wrote(&vd,frames);
    vorbis_analysis_wrote(&vd,0);
  }

  while(vorbis_analysis_blockout(&vd,&vb)==1){
    vorbis_analysis(&vb,NULL);
    vorbis_bitrate_addblock(&vb);
    while(vorbis_bitrate_flushpacket(&vd,&op)){
      if(append_audio(t,&op)){
        ret=-1;
        goto done;
      }
    }
  }

done:
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  if(ret)free_track(t);
  return ret;
}

static int drain_full(vorbis_dsp_state *vd,float *out,int out_cap,int *frames){
  float **pcm;
  int got=*frames;
  int samples;

  while((samples=vorbis_synthesis_pcmout(vd,&pcm))>0){
    int i;
    if(got+samples>out_cap)return -1;
    for(i=0;i<vd->vi->channels;i++)
      memcpy(out+(size_t)i*out_cap+got,pcm[i],
             (size_t)samples*sizeof(*out));
    got+=samples;
    if(vorbis_synthesis_read(vd,samples))return -1;
  }
  *frames=got;
  return 0;
}

static int decode_full(const test_track *t,float *out,int out_cap,int *frames){
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  int i;
  int got=0;
  int ret=-1;

  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);
  for(i=0;i<3;i++){
    ogg_packet op=packet_view(&t->header[i]);
    if(vorbis_synthesis_headerin(&vi,&vc,&op))goto done;
  }
  if(vorbis_synthesis_init(&vd,&vi))goto done;
  if(vorbis_block_init(&vd,&vb)){
    vorbis_dsp_clear(&vd);
    goto done;
  }

  for(i=0;i<t->audio_count;i++){
    ogg_packet op=packet_view(&t->audio[i]);
    if(vorbis_synthesis(&vb,&op))goto decode_done;
    if(vorbis_synthesis_blockin(&vd,&vb))goto decode_done;
    if(drain_full(&vd,out,out_cap,&got))goto decode_done;
  }
  *frames=got;
  ret=0;

decode_done:
  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
done:
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  return ret;
}

static int drain_reuse(vorbis_reuse_decoder *r,float *out,int out_cap,
                       int *frames){
  float **pcm;
  int got=*frames;
  int samples;

  while((samples=vorbis_reuse_pcmout(r,&pcm))>0){
    int i;
    const vorbis_info *vi=vorbis_reuse_info(r);
    if(got+samples>out_cap)return -1;
    for(i=0;i<vi->channels;i++)
      memcpy(out+(size_t)i*out_cap+got,pcm[i],
             (size_t)samples*sizeof(*out));
    got+=samples;
    if(vorbis_reuse_read(r,samples))return -1;
  }
  *frames=got;
  return 0;
}

static int decode_reuse(vorbis_reuse_decoder *r,const test_track *t,
                        float *out,int out_cap,int *frames){
  int i;
  int got=0;

  for(i=0;i<3;i++){
    ogg_packet op=packet_view(&t->header[i]);
    if(vorbis_reuse_headerin(r,&op))return -1;
  }
  for(i=0;i<t->audio_count;i++){
    ogg_packet op=packet_view(&t->audio[i]);
    if(vorbis_reuse_decode(r,&op))return -1;
    if(drain_reuse(r,out,out_cap,&got))return -1;
  }
  *frames=got;
  return 0;
}

static int compare_output(const test_track *t,const float *ref,int ref_frames,
                          const float *got,int got_frames,int stride,
                          run_result *result){
  size_t samples;
  int c;

  if(ref_frames!=got_frames){
    fprintf(stderr,"FAIL: frame count %d != %d (track seed stream)\n",
            ref_frames,got_frames);
    result->failures++;
    return -1;
  }

  samples=(size_t)ref_frames*t->channels;
  for(c=0;c<t->channels;c++){
    const float *r=ref+(size_t)c*stride;
    const float *g=got+(size_t)c*stride;
    if(memcmp(r,g,(size_t)ref_frames*sizeof(*r))){
      int i;
      for(i=0;i<ref_frames;i++){
        if(memcmp(r+i,g+i,sizeof(*r))){
          fprintf(stderr,
                  "FAIL: sample mismatch at frame %d ch %d: "
                  "%.9g != %.9g\n",i,c,r[i],g[i]);
          break;
        }
      }
      result->failures++;
      return -1;
    }
    hash_bytes(&result->hash,r,(size_t)ref_frames*sizeof(*r));
  }

  result->samples+=samples;
  return 0;
}

static int decode_pair(vorbis_reuse_decoder *r,const test_track *t,
                       float *ref,float *got,int cap,run_result *result){
  int ref_frames=0;
  int got_frames=0;

  if(decode_full(t,ref,cap,&ref_frames)){
    fprintf(stderr,"FAIL: full decode failed\n");
    result->failures++;
    return -1;
  }
  if(decode_reuse(r,t,got,cap,&got_frames)){
    fprintf(stderr,"FAIL: reuse decode failed\n");
    result->failures++;
    return -1;
  }
  return compare_output(t,ref,ref_frames,got,got_frames,cap,result);
}

static int setup_equal(const packet_copy *a,const packet_copy *b){
  return a->bytes==b->bytes &&
    memcmp(a->data,b->data,(size_t)a->bytes)==0;
}

static int edge_header_order(void){
  vorbis_reuse_decoder *r=vorbis_reuse_new();
  test_track t;
  ogg_packet op;
  int failures=0;

  if(!r || encode_track(&t,CHANNELS,SAMPLE_RATE,.1f,64,7001)){
    if(r)vorbis_reuse_free(r);
    return 1;
  }

  op=packet_view(&t.header[2]);
  if(vorbis_reuse_headerin(r,&op)!=OV_EBADHEADER){
    fprintf(stderr,"FAIL: setup accepted before ID/comment\n");
    failures++;
  }
  op=packet_view(&t.header[0]);
  if(vorbis_reuse_headerin(r,&op)){
    fprintf(stderr,"FAIL: valid ID rejected after early setup\n");
    failures++;
  }
  op=packet_view(&t.header[1]);
  if(vorbis_reuse_headerin(r,&op)){
    fprintf(stderr,"FAIL: valid comment rejected after early setup\n");
    failures++;
  }
  op=packet_view(&t.header[2]);
  if(vorbis_reuse_headerin(r,&op)){
    fprintf(stderr,"FAIL: valid setup rejected after early setup\n");
    failures++;
  }
  if(vorbis_reuse_headerin(r,&op)!=OV_EBADHEADER){
    fprintf(stderr,"FAIL: duplicate setup accepted\n");
    failures++;
  }

  vorbis_reuse_free(r);
  free_track(&t);
  return failures;
}

static int edge_pending_pcm(void){
  vorbis_reuse_decoder *r=vorbis_reuse_new();
  test_track a,b;
  int failures=0;
  int i;

  if(!r || encode_track(&a,CHANNELS,SAMPLE_RATE,.1f,512,7101) ||
     encode_track(&b,CHANNELS,SAMPLE_RATE,.1f,512,7102)){
    if(r)vorbis_reuse_free(r);
    return 1;
  }

  for(i=0;i<3;i++){
    ogg_packet op=packet_view(&a.header[i]);
    if(vorbis_reuse_headerin(r,&op))failures++;
  }
  for(i=0;i<a.audio_count;i++){
    ogg_packet op=packet_view(&a.audio[i]);
    if(vorbis_reuse_decode(r,&op)){
      failures++;
      break;
    }
    if(vorbis_reuse_pcmout(r,NULL)>0)break;
  }
  if(vorbis_reuse_pcmout(r,NULL)<=0){
    fprintf(stderr,"FAIL: expected pending PCM in edge test\n");
    failures++;
  }

  {
    ogg_packet op=packet_view(&b.header[0]);
    if(vorbis_reuse_headerin(r,&op)!=OV_EINVAL){
      fprintf(stderr,"FAIL: track switch accepted with pending PCM\n");
      failures++;
    }
  }

  while(vorbis_reuse_pcmout(r,NULL)>0){
    int n=vorbis_reuse_pcmout(r,NULL);
    if(vorbis_reuse_read(r,n))failures++;
  }
  {
    ogg_packet op=packet_view(&b.header[0]);
    if(vorbis_reuse_headerin(r,&op)){
      fprintf(stderr,"FAIL: track switch rejected after drain\n");
      failures++;
    }
  }

  vorbis_reuse_free(r);
  free_track(&a);
  free_track(&b);
  return failures;
}

static int edge_bad_setup_preserves_engine(void){
  vorbis_reuse_decoder *r=vorbis_reuse_new();
  test_track a,b;
  packet_copy bad;
  ogg_packet op;
  int failures=0;
  int i;

  if(!r || encode_track(&a,CHANNELS,SAMPLE_RATE,.1f,256,7201) ||
     encode_track(&b,CHANNELS,SAMPLE_RATE,.65f,256,7202)){
    if(r)vorbis_reuse_free(r);
    return 1;
  }

  for(i=0;i<3;i++){
    op=packet_view(&a.header[i]);
    if(vorbis_reuse_headerin(r,&op))failures++;
  }
  for(i=0;i<2;i++){
    op=packet_view(&b.header[i]);
    if(vorbis_reuse_headerin(r,&op))failures++;
  }

  memset(&bad,0,sizeof(bad));
  bad.bytes=b.header[2].bytes-5;
  bad.data=malloc((size_t)bad.bytes);
  if(!bad.data){
    vorbis_reuse_free(r);
    free_track(&a);
    free_track(&b);
    return 1;
  }
  memcpy(bad.data,b.header[2].data,(size_t)bad.bytes);
  op=packet_view(&bad);
  if(vorbis_reuse_headerin(r,&op)==0){
    fprintf(stderr,"FAIL: truncated changed setup accepted\n");
    failures++;
  }

  /* Resubmitting the previous setup must still hit the retained engine. */
  for(i=0;i<3;i++){
    op=packet_view(&a.header[i]);
    if(vorbis_reuse_headerin(r,&op)){
      fprintf(stderr,"FAIL: retry of retained setup rejected\n");
      failures++;
    }
  }
  if(vorbis_reuse_pcmout(r,NULL)!=0){
    fprintf(stderr,"FAIL: retained engine produced stale PCM\n");
    failures++;
  }

  free_packet(&bad);
  vorbis_reuse_free(r);
  free_track(&a);
  free_track(&b);
  return failures;
}

static int edge_empty_track(void){
  vorbis_reuse_decoder *r=vorbis_reuse_new();
  test_track empty,next;
  ogg_packet op;
  int failures=0;
  int i;

  if(!r || encode_track(&empty,CHANNELS,SAMPLE_RATE,.1f,0,7301) ||
     encode_track(&next,CHANNELS,SAMPLE_RATE,.1f,128,7302)){
    if(r)vorbis_reuse_free(r);
    return 1;
  }

  if(empty.audio_count!=0){
    fprintf(stderr,"FAIL: empty track unexpectedly has audio packets\n");
    failures++;
  }
  for(i=0;i<3;i++){
    op=packet_view(&empty.header[i]);
    if(vorbis_reuse_headerin(r,&op))failures++;
  }
  if(vorbis_reuse_pcmout(r,NULL)!=0){
    fprintf(stderr,"FAIL: empty track produced PCM\n");
    failures++;
  }
  for(i=0;i<3;i++){
    op=packet_view(&next.header[i]);
    if(vorbis_reuse_headerin(r,&op))failures++;
  }

  vorbis_reuse_free(r);
  free_track(&empty);
  free_track(&next);
  return failures;
}

int main(void){
  vorbis_reuse_decoder *r=NULL;
  test_track t,a,b;
  float *ref=NULL;
  float *got=NULL;
  int cap=BASE_FRAMES+OUTPUT_SLACK;
  int i;
  int failures=0;
  run_result same={0,0,FNV_OFFSET,0};
  run_result alt={0,0,FNV_OFFSET,0};
  vorbis_reuse_stats same_stats={0,0,0,0,0};
  vorbis_reuse_stats alt_stats={0,0,0,0,0};
  clock_t begin=clock();

  memset(&t,0,sizeof(t));
  memset(&a,0,sizeof(a));
  memset(&b,0,sizeof(b));
  ref=calloc((size_t)cap*CHANNELS,sizeof(*ref));
  got=calloc((size_t)cap*CHANNELS,sizeof(*got));
  if(!ref || !got){
    fprintf(stderr,"out of memory\n");
    free(ref);
    free(got);
    return 1;
  }

  /* 1000 independent tracks.  The comment header intentionally changes
     every track; the identification parameters and setup do not. */
  r=vorbis_reuse_new();
  if(!r){
    fprintf(stderr,"out of memory\n");
    free(ref);
    free(got);
    return 1;
  }
  for(i=0;i<SAME_TRACKS;i++){
    int frames=BASE_FRAMES+(i%5)*64;
    if(encode_track(&t,CHANNELS,SAMPLE_RATE,.1f,frames,i)){
      fprintf(stderr,"FAIL: encode track %d\n",i);
      failures++;
      break;
    }
    if(decode_pair(r,&t,ref,got,cap,&same)){
      fprintf(stderr,"FAIL: reuse mismatch on track %d\n",i);
      failures++;
      break;
    }
    free_track(&t);
  }
  free_track(&t);
  vorbis_reuse_getstats(r,&same_stats);
  if(same_stats.tracks!=SAME_TRACKS ||
     same_stats.setup_reused!=SAME_TRACKS-1 ||
     same_stats.setup_rebuilt!=1){
    fprintf(stderr,
            "FAIL: same-setup stats tracks=%llu reused=%llu rebuilt=%llu\n",
            same_stats.tracks,same_stats.setup_reused,
            same_stats.setup_rebuilt);
    failures++;
  }
  vorbis_reuse_free(r);
  r=NULL;

  /* Two different setups, switched every track.  A fresh reference is
     decoded once for each; the reused decoder must match both. */
  if(encode_track(&a,CHANNELS,SAMPLE_RATE,.1f,BASE_FRAMES,7401) ||
     encode_track(&b,CHANNELS,48000,.65f,BASE_FRAMES+37,7402) ||
     setup_equal(&a.header[2],&b.header[2])){
    fprintf(stderr,"FAIL: could not construct distinct setup streams\n");
    failures++;
  }else{
    int aframes=0,bframes=0;
    float *aref=calloc((size_t)cap*CHANNELS,sizeof(*aref));
    float *bref=calloc((size_t)cap*CHANNELS,sizeof(*bref));
    if(!aref || !bref ||
       decode_full(&a,aref,cap,&aframes) ||
       decode_full(&b,bref,cap,&bframes)){
      fprintf(stderr,"FAIL: alternating reference decode\n");
      failures++;
    }else{
      r=vorbis_reuse_new();
      for(i=0;i<ALT_SWITCHES;i++){
        const test_track *cur=(i&1)?&b:&a;
        const float *cur_ref=(i&1)?bref:aref;
        int cur_frames=(i&1)?bframes:aframes;
        int got_frames=0;
        if(decode_reuse(r,cur,got,cap,&got_frames) ||
           compare_output(cur,cur_ref,cur_frames,got,got_frames,cap,&alt)){
          fprintf(stderr,"FAIL: alternating switch %d\n",i);
          failures++;
          break;
        }
      }
      vorbis_reuse_getstats(r,&alt_stats);
      if(alt_stats.tracks!=ALT_SWITCHES ||
         alt_stats.setup_rebuilt!=ALT_SWITCHES){
        fprintf(stderr,
                "FAIL: alternating stats tracks=%llu rebuilt=%llu\n",
                alt_stats.tracks,alt_stats.setup_rebuilt);
        failures++;
      }
      vorbis_reuse_free(r);
      r=NULL;
    }
    free(aref);
    free(bref);
  }

  /* Header/order, pending-PCM, rollback and empty-track boundaries. */
  failures+=edge_header_order();
  failures+=edge_pending_pcm();
  failures+=edge_bad_setup_preserves_engine();
  failures+=edge_empty_track();

  printf("reuse consistency data:\n");
  printf("  same-setup tracks:    %d\n",SAME_TRACKS);
  printf("  setup reused/rebuilt: %llu / %llu\n",
         same_stats.setup_reused,same_stats.setup_rebuilt);
  printf("  alternating switches: %d (distinct setup each time)\n",
         ALT_SWITCHES);
  printf("  samples compared:     %llu\n",same.samples+alt.samples);
  printf("  audio blocks decoded: %llu\n",
         same_stats.blocks+alt_stats.blocks);
  printf("  max sample diff:      %s\n",failures?"see failures":"0");
  printf("  pcm hash same-setup:  %016llx\n",same.hash);
  printf("  pcm hash alternating: %016llx\n",alt.hash);
  printf("  edge cases:           header order, duplicate setup, "
         "pending PCM,\n");
  printf("                        failed setup rollback, empty track\n");
  printf("  result:               %s\n",failures?"FAIL":"PASS");
  printf("  elapsed:              %.3f s\n",
         (double)(clock()-begin)/CLOCKS_PER_SEC);

  free(ref);
  free(got);
  free_track(&a);
  free_track(&b);
  return failures?1:0;
}
