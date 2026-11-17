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

 function: reuse a decoder across consecutive compatible tracks

 ********************************************************************/

#ifndef _vorbis_reuse_h_
#define _vorbis_reuse_h_

#include <ogg/ogg.h>
#include <vorbis/codec.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

typedef struct vorbis_reuse_decoder vorbis_reuse_decoder;

typedef struct vorbis_reuse_stats{
  unsigned long long tracks;        /* completed header sets */
  unsigned long long setup_reused;  /* tracks that reused the old setup */
  unsigned long long setup_rebuilt; /* tracks that rebuilt the setup */
  unsigned long long blocks;        /* audio packets synthesized */
  unsigned long long samples;       /* per-channel samples handed to caller */
} vorbis_reuse_stats;

/* Allocate a reusable decoder.  The returned object owns one active
   decoding engine.  Feed complete header sets in stream order with
   vorbis_reuse_headerin(); the first audio packet of a new track is
   accepted only after the setup packet. */
extern vorbis_reuse_decoder *vorbis_reuse_new(void);

/* Release all active and pending state.  Safe with NULL. */
extern void vorbis_reuse_free(vorbis_reuse_decoder *r);

/* Consume one Vorbis header packet (identification, comment or setup).
   Headers must arrive in the normal order.  A new identification
   packet starts a new track and is rejected with OV_EINVAL while PCM
   from the preceding track is still unread.

   When the setup packet is byte-identical to the active setup and the
   identification parameters (version, channels, rate and both block
   sizes) match, the codebooks and decoder lookups are retained, and
   only PCM/overlap and block scratch state is reset.  Any change
   rebuilds the setup.  A failed changed setup leaves the previous
   active track intact. */
extern int vorbis_reuse_headerin(vorbis_reuse_decoder *r,
                                 ogg_packet *op);

/* The track currently being assembled, or the active track after its
   setup packet has been accepted.  May be NULL before the first ID
   header. */
extern const vorbis_info *vorbis_reuse_info(const vorbis_reuse_decoder *r);

/* Synthesize and block in one audio packet.  Pending PCM must have been
   consumed with vorbis_reuse_pcmout()/vorbis_reuse_read() first.
   Returns 0 on success or a libvorbis error code. */
extern int vorbis_reuse_decode(vorbis_reuse_decoder *r, ogg_packet *op);

/* Pull decoded PCM, exactly like vorbis_synthesis_pcmout(). */
extern int vorbis_reuse_pcmout(vorbis_reuse_decoder *r, float ***pcm);

/* Mark n per-channel samples as consumed, exactly like
   vorbis_synthesis_read(). */
extern int vorbis_reuse_read(vorbis_reuse_decoder *r, int n);

/* Copy counters describing reuse.  Either argument may be NULL. */
extern void vorbis_reuse_getstats(const vorbis_reuse_decoder *r,
                                  vorbis_reuse_stats *stats);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* _vorbis_reuse_h_ */
