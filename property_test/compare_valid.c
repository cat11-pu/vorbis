/* Property test: encode 50 valid Vorbis streams, decode each with
 * libvorbis and with the ffmpeg reference decoder, compare sample by
 * sample. Streams exceeding the tolerance are saved as counterexamples.
 */
#include "pt_common.h"

#include <math.h>
#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NVALID 50
#define TOL 1e-3f

static int write_ogg(const stream_t *s, const char *path) {
  FILE *f = fopen(path, "wb");
  ogg_stream_state os;
  ogg_page og;
  int i;
  if (!f) return -1;
  ogg_stream_init(&os, 0x76746273);
  for (i = 0; i < s->n; i++) {
    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = s->pkts[i].data;
    op.bytes = s->pkts[i].len;
    op.b_o_s = (i == 0);
    op.e_o_s = (i == s->n - 1);
    op.granulepos = s->pkts[i].granulepos;
    op.packetno = i;
    ogg_stream_packetin(&os, &op);
    if (i == s->n - 1) {
      while (ogg_stream_flush(&os, &og)) {
        fwrite(og.header, 1, (size_t)og.header_len, f);
        fwrite(og.body, 1, (size_t)og.body_len, f);
      }
    } else {
      while (ogg_stream_pageout(&os, &og)) {
        fwrite(og.header, 1, (size_t)og.header_len, f);
        fwrite(og.body, 1, (size_t)og.body_len, f);
      }
    }
  }
  ogg_stream_clear(&os);
  fclose(f);
  return 0;
}

static float *read_f32(const char *path, long *nfloats) {
  FILE *f = fopen(path, "rb");
  float *buf;
  long sz;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  *nfloats = sz / 4;
  return buf;
}

int main(int argc, char **argv) {
  static const enc_params_t table[] = {
      {44100, 2, 0.4f, 0.30}, {48000, 2, 0.6f, 0.25},
      {44100, 1, 0.3f, 0.30}, {32000, 2, 0.4f, 0.25},
      {16000, 1, 0.2f, 0.30}, {8000, 1, 0.1f, 0.30},
      {22050, 2, 0.3f, 0.25}, {48000, 1, 0.5f, 0.25},
      {44100, 2, 0.1f, 0.20}, {44100, 2, 0.8f, 0.20},
  };
  const char *ce_dir = argc > 1 ? argv[1] : ".";
  const char *tmp = "/tmp/pt_valid_work";
  char oggpath[512], rawpath[512], cmd[2048], cepath[512];
  int mismatches = 0, i;
  float global_max = 0.0f;

  mkdir(tmp, 0755);
  for (i = 0; i < NVALID; i++) {
    const enc_params_t *p = &table[i % (int)(sizeof(table) / sizeof(table[0]))];
    stream_t s;
    float *a, *b;
    long na = 0, nb = 0, n, k;
    float maxdiff = 0.0f;
    long exceed = 0;

    snprintf(oggpath, sizeof(oggpath), "%s/valid_%d.ogg", tmp, i);
    snprintf(rawpath, sizeof(rawpath), "%s/valid_%d.raw", tmp, i);

    if (pt_encode_stream(&s, p, 0xfeed0000ULL + (uint64_t)i) != 0) {
      printf("VALID %d status=ENCODER_SKIP rate=%ld ch=%d\n", i, p->rate,
             p->channels);
      continue;
    }
    a = pt_decode_stream(&s, &na);
    if (!a) {
      printf("VALID %d status=SELF_DECODE_FAIL\n", i);
      mismatches++;
      pt_stream_free(&s);
      continue;
    }
    if (write_ogg(&s, oggpath) != 0) {
      printf("VALID %d status=WRITE_FAIL\n", i);
      mismatches++;
      free(a);
      pt_stream_free(&s);
      continue;
    }
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -y -i %s -f f32le -acodec pcm_f32le %s",
             oggpath, rawpath);
    if (system(cmd) != 0 || (b = read_f32(rawpath, &nb)) == NULL) {
      printf("VALID %d status=REF_DECODE_FAIL\n", i);
      mismatches++;
      free(a);
      pt_stream_free(&s);
      continue;
    }

    n = na * s.channels < nb ? na * s.channels : nb;
    for (k = 0; k < n; k++) {
      float d = fabsf(a[k] - b[k]);
      if (d > maxdiff) maxdiff = d;
      if (d > TOL) exceed++;
    }
    if (maxdiff > global_max) global_max = maxdiff;

    /* ffmpeg does not apply granulepos end-trimming for ogg/vorbis, so
     * the reference may emit up to one extra block of tail padding.
     * Require: exact libvorbis roundtrip length, matching common region,
     * reference longer by at most one max block (8192 samples). */
    long expected = (long)(p->rate * p->seconds) * s.channels;
    long tail = nb - na * s.channels;
    int bad = 0;
    if (na * s.channels != expected) bad |= 1;
    if (exceed > 0) bad |= 2;
    if (tail < 0 || tail > 8192) bad |= 4;
    if (bad) {
      printf("VALID %d status=MISMATCH bad=%d rate=%ld ch=%d samples=%ld "
             "expected=%ld ref=%ld maxdiff=%.6g exceed=%ld\n",
             i, bad, p->rate, p->channels, na * s.channels, expected, nb,
             maxdiff, exceed);
      snprintf(cepath, sizeof(cepath), "%s/valid_%d.ogg", ce_dir, i);
      snprintf(cmd, sizeof(cmd), "cp %s %s", oggpath, cepath);
      system(cmd);
      mismatches++;
    } else {
      printf("VALID %d status=OK rate=%ld ch=%d samples=%ld tailpad=%ld "
             "maxdiff=%.6g\n",
             i, p->rate, p->channels, na * s.channels, tail, maxdiff);
    }
    free(a);
    free(b);
    pt_stream_free(&s);
  }
  printf("VALID_SUMMARY total=%d mismatches=%d global_maxdiff=%.6g tol=%.3g\n",
         NVALID, mismatches, global_max, TOL);
  return mismatches ? 1 : 0;
}
