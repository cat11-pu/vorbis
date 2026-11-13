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

 Vorbis audio cannot be decoded starting at an arbitrary packet: each
 MDCT block overlaps its neighbours, so reconstruction requires the
 previous block's output.  This interface hides that detail.  A seek
 locates the nearest reconstruction point at or before the target
 sample (the page boundary found by bisecting on page granulepos),
 decodes forward from there (priming the overlap/lapping state), and
 discards output up to the target.  The first sample returned after a
 seek is exactly the requested sample, bit-identical to straight
 sequential decoding from the beginning of the stream.

 ********************************************************************/

#ifndef _VSD_SEEK_DECODE_H_
#define _VSD_SEEK_DECODE_H_

#include <ogg/ogg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VsdDecoder VsdDecoder;

/* Open a seekable Ogg Vorbis file.  Returns NULL on error.
 * channels/rate/total_samples may be NULL if not wanted. */
VsdDecoder *vsd_open(const char *path,
                     int *channels, long *rate,
                     ogg_int64_t *total_samples);

/* Seek to an exact sample position (per-channel PCM frame index,
 * 0 <= sample <= total_samples).  Returns 0 on success, nonzero on
 * failure (out of range or unseekable stream).  After a successful
 * seek, the next vsd_read() returns the sample at exactly this
 * position. */
int vsd_seek(VsdDecoder *d, ogg_int64_t sample);

/* Read up to `frames` per-channel samples starting at the current
 * position into `out` (interleaved 16-bit PCM, must hold
 * frames*channels shorts).  Returns the number of frames actually
 * read, 0 at end of stream, or a negative value on error. */
long vsd_read(VsdDecoder *d, short *out, long frames);

/* Current sample position (frame index of the next sample returned
 * by vsd_read). */
ogg_int64_t vsd_tell(VsdDecoder *d);

void vsd_close(VsdDecoder *d);

#ifdef __cplusplus
}
#endif

#endif /* _VSD_SEEK_DECODE_H_ */
