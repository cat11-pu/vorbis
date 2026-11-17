#include "pt_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint64_t pt_rng_next(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

void pt_stream_push(stream_t *s, const unsigned char *data, long len,
                    long granulepos) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->pkts = realloc(s->pkts, (size_t)s->cap * sizeof(pkt_t));
  }
  s->pkts[s->n].data = malloc(len > 0 ? (size_t)len : 1);
  if (len > 0) memcpy(s->pkts[s->n].data, data, (size_t)len);
  s->pkts[s->n].len = len;
  s->pkts[s->n].granulepos = granulepos;
  s->n++;
}

void pt_stream_free(stream_t *s) {
  int i;
  for (i = 0; i < s->n; i++) free(s->pkts[i].data);
  free(s->pkts);
  memset(s, 0, sizeof(*s));
}

int pt_encode_stream(stream_t *s, const enc_params_t *p, uint64_t seed) {
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_packet hdr, hdr_comm, hdr_code, op;
  long nsamples, i;
  uint64_t rng = seed ? seed : 1;
  double f1, f2;
  int ret = -1;

  memset(s, 0, sizeof(*s));
  s->channels = p->channels;
  s->rate = p->rate;

  vorbis_info_init(&vi);
  if (vorbis_encode_init_vbr(&vi, p->channels, p->rate, p->quality)) {
    vorbis_info_clear(&vi);
    return -1;
  }
  vorbis_comment_init(&vc);
  vorbis_comment_add_tag(&vc, "ENCODER", "property_test");
  vorbis_analysis_init(&vd, &vi);
  vorbis_block_init(&vd, &vb);

  vorbis_analysis_headerout(&vd, &vc, &hdr, &hdr_comm, &hdr_code);
  pt_stream_push(s, hdr.packet, hdr.bytes, hdr.granulepos);
  pt_stream_push(s, hdr_comm.packet, hdr_comm.bytes, hdr_comm.granulepos);
  pt_stream_push(s, hdr_code.packet, hdr_code.bytes, hdr_code.granulepos);

  nsamples = (long)(p->rate * p->seconds);
  f1 = 220.0 + (double)(pt_rng_next(&rng) % 500);
  f2 = 900.0 + (double)(pt_rng_next(&rng) % 1500);

  i = 0;
  while (i < nsamples) {
    long chunk = nsamples - i > 1024 ? 1024 : nsamples - i;
    float **buf = vorbis_analysis_buffer(&vd, (int)chunk);
    long j;
    int ch;
    for (j = 0; j < chunk; j++) {
      double t = (double)(i + j) / (double)p->rate;
      for (ch = 0; ch < p->channels; ch++) {
        double detune = 1.0 + 0.01 * ch;
        double v = 0.4 * sin(2.0 * M_PI * f1 * detune * t) +
                   0.15 * sin(2.0 * M_PI * f2 * t) +
                   0.02 * ((double)(pt_rng_next(&rng) % 1000) / 1000.0 - 0.5);
        buf[ch][j] = (float)v;
      }
    }
    vorbis_analysis_wrote(&vd, (int)chunk);
    i += chunk;
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {
      vorbis_analysis(&vb, NULL);
      vorbis_bitrate_addblock(&vb);
      while (vorbis_bitrate_flushpacket(&vd, &op))
        pt_stream_push(s, op.packet, op.bytes, op.granulepos);
    }
  }

  vorbis_analysis_wrote(&vd, 0);
  while (vorbis_analysis_blockout(&vd, &vb) == 1) {
    vorbis_analysis(&vb, NULL);
    vorbis_bitrate_addblock(&vb);
    while (vorbis_bitrate_flushpacket(&vd, &op))
      pt_stream_push(s, op.packet, op.bytes, op.granulepos);
  }

  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  ret = 0;
  return ret;
}

float *pt_decode_stream(const stream_t *s, long *nsamples) {
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  float *out = NULL;
  long out_len = 0, out_cap = 0;
  int i, hdr = 0, dsp = 0;

  *nsamples = 0;
  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);

  for (i = 0; i < s->n; i++) {
    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = s->pkts[i].data;
    op.bytes = s->pkts[i].len;
    op.b_o_s = (i == 0);
    op.e_o_s = (i == s->n - 1);
    op.granulepos = s->pkts[i].granulepos;
    op.packetno = i;

    if (hdr < 3) {
      if (vorbis_synthesis_headerin(&vi, &vc, &op) != 0) goto fail;
      hdr++;
      if (hdr == 3) {
        if (vorbis_synthesis_init(&vd, &vi) != 0) goto fail;
        vorbis_block_init(&vd, &vb);
        dsp = 1;
      }
    } else {
      if (vorbis_synthesis(&vb, &op) == 0) {
        float **pcm;
        int n;
        vorbis_synthesis_blockin(&vd, &vb);
        while ((n = vorbis_synthesis_pcmout(&vd, &pcm)) > 0) {
          int j, ch;
          if (out_len + n > out_cap) {
            out_cap = (out_len + n) * 2 + 1024;
            out = realloc(out, (size_t)out_cap * vi.channels * sizeof(float));
          }
          for (j = 0; j < n; j++)
            for (ch = 0; ch < vi.channels; ch++)
              out[(out_len + j) * vi.channels + ch] = pcm[ch][j];
          out_len += n;
          vorbis_synthesis_read(&vd, n);
        }
      }
    }
  }

  if (dsp) {
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
  }
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  *nsamples = out_len;
  return out;

fail:
  if (dsp) {
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
  }
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
  free(out);
  return NULL;
}
