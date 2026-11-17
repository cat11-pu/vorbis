#ifndef PT_COMMON_H
#define PT_COMMON_H

#include <stdint.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

typedef struct {
  unsigned char *data;
  long len;
  long granulepos;
} pkt_t;

typedef struct {
  pkt_t *pkts;
  int n;
  int cap;
  int channels;
  long rate;
} stream_t;

typedef struct {
  long rate;
  int channels;
  float quality;
  double seconds;
} enc_params_t;

uint64_t pt_rng_next(uint64_t *state);

/* Encode a deterministic synthetic stream into a packet list.
   Returns 0 on success, -1 if the encoder rejects the params. */
int pt_encode_stream(stream_t *s, const enc_params_t *p, uint64_t seed);

/* Decode a packet list through the raw libvorbis synthesis API.
   Returns malloc'd interleaved float PCM (NULL on header failure),
   *nsamples receives per-channel sample count. */
float *pt_decode_stream(const stream_t *s, long *nsamples);

void pt_stream_free(stream_t *s);
void pt_stream_push(stream_t *s, const unsigned char *data, long len,
                    long granulepos);

#endif
