/* Property test: feed 200 malformed Vorbis packet streams (truncation,
 * illegal codebook, duplicate headers, oversized blocks - interleaved)
 * through the libvorbis decoder. Built with ASan/UBSan/LSan: any crash,
 * out-of-bounds access or leak aborts the process and the mutated input
 * (already dumped to disk) becomes a counterexample.
 *
 * Every decode rejection is logged with the byte offset of the mutation.
 */
#include "pt_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCASES 200

static const char *cat_names[4] = {"trunc", "codebook", "duphdr", "bigblock"};

typedef struct {
  int cat;
  int pkt_idx;    /* packet the mutation applies to / insert position */
  long mut_off;   /* primary byte offset of the mutation */
  long mut_off2;  /* secondary offset (last corruption point / size) */
  int count;      /* number of corrupted bytes / inserted packets */
} mut_t;

static uint64_t rng;

static uint32_t rnd32(void) { return (uint32_t)(pt_rng_next(&rng) >> 32); }

static void mutate_trunc(stream_t *s, mut_t *m) {
  int idx = (int)(rnd32() % (uint32_t)s->n);
  long newlen = (long)(rnd32() % (uint32_t)(s->pkts[idx].len + 1));
  m->pkt_idx = idx;
  m->mut_off = newlen; /* truncation point */
  m->mut_off2 = s->pkts[idx].len;
  m->count = 1;
  s->pkts[idx].len = newlen;
}

static void mutate_codebook(stream_t *s, mut_t *m) {
  /* Corrupt the setup header (packet 2), which carries the codebooks.
   * Bias offsets toward the front where the codebook definitions live. */
  int idx = 2;
  long len = s->pkts[idx].len;
  int k = 1 + (int)(rnd32() % 16);
  int j;
  m->pkt_idx = idx;
  m->count = k;
  m->mut_off = -1;
  m->mut_off2 = -1;
  for (j = 0; j < k; j++) {
    long off;
    if ((rnd32() & 1) && len > 512)
      off = (long)(rnd32() % 512);
    else
      off = (long)(rnd32() % (uint32_t)len);
    s->pkts[idx].data[off] ^= (unsigned char)(1u << (rnd32() % 8));
    if (m->mut_off < 0) m->mut_off = off;
    m->mut_off2 = off;
  }
}

static void stream_insert_copy(stream_t *s, int pos, const pkt_t *src) {
  int i;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->pkts = realloc(s->pkts, (size_t)s->cap * sizeof(pkt_t));
  }
  for (i = s->n; i > pos; i--) s->pkts[i] = s->pkts[i - 1];
  s->pkts[pos].data = malloc(src->len > 0 ? (size_t)src->len : 1);
  if (src->len > 0) memcpy(s->pkts[pos].data, src->data, (size_t)src->len);
  s->pkts[pos].len = src->len;
  s->pkts[pos].granulepos = src->granulepos;
  s->n++;
}

static void mutate_duphdr(stream_t *s, mut_t *m) {
  int hdr = (int)(rnd32() % 3);
  int dups = 1 + (int)(rnd32() % 3);
  int j;
  m->pkt_idx = hdr;
  m->mut_off = 0;
  m->mut_off2 = s->pkts[hdr].len;
  m->count = dups;
  for (j = 0; j < dups; j++) stream_insert_copy(s, hdr + 1, &s->pkts[hdr]);
}

static void mutate_bigblock(stream_t *s, mut_t *m) {
  static const long sizes[4] = {4096, 65536, 262144, 1048576};
  long sz = sizes[rnd32() % 4];
  int nbig = 1 + (int)(rnd32() % 3);
  /* half the cases: oversized header (insert before packet 3);
   * other half: oversized audio block (insert after the headers) */
  int pos = (rnd32() & 1) ? (1 + (int)(rnd32() % 2))
                          : (3 + (int)(rnd32() % 3));
  int j;
  m->pkt_idx = pos;
  m->mut_off = 0;
  m->mut_off2 = sz;
  m->count = nbig;
  for (j = 0; j < nbig; j++) {
    pkt_t big;
    long k;
    big.data = malloc((size_t)sz);
    big.len = sz;
    big.granulepos = 0;
    for (k = 0; k < sz; k++) big.data[k] = (unsigned char)rnd32();
    /* 50%: keep a valid packet prefix so the block type parses */
    if ((rnd32() & 1) && s->n > 3) {
      long plen = s->pkts[3].len < sz ? s->pkts[3].len : sz;
      memcpy(big.data, s->pkts[3].data, (size_t)plen);
    }
    stream_insert_copy(s, pos + j, &big);
    free(big.data);
  }
}

static void apply_mutation(stream_t *s, int cat, mut_t *m) {
  m->cat = cat;
  switch (cat) {
    case 0: mutate_trunc(s, m); break;
    case 1: mutate_codebook(s, m); break;
    case 2: mutate_duphdr(s, m); break;
    default: mutate_bigblock(s, m); break;
  }
}

static void dump_stream(const stream_t *s, const char *path) {
  FILE *f = fopen(path, "wb");
  int i;
  if (!f) return;
  fwrite("PTPK", 1, 4, f);
  fwrite(&s->n, sizeof(s->n), 1, f);
  fwrite(&s->channels, sizeof(s->channels), 1, f);
  fwrite(&s->rate, sizeof(s->rate), 1, f);
  for (i = 0; i < s->n; i++) {
    fwrite(&s->pkts[i].len, sizeof(long), 1, f);
    fwrite(&s->pkts[i].granulepos, sizeof(long), 1, f);
    fwrite(s->pkts[i].data, 1, (size_t)s->pkts[i].len, f);
  }
  fclose(f);
}

static const enc_params_t *params_for(int c) {
  static const enc_params_t table[] = {
      {44100, 2, 0.4f, 0.30}, {48000, 2, 0.5f, 0.25},
      {44100, 1, 0.3f, 0.30}, {32000, 2, 0.4f, 0.25},
      {16000, 1, 0.2f, 0.30}, {8000, 1, 0.1f, 0.30},
      {22050, 2, 0.3f, 0.25}, {48000, 1, 0.6f, 0.25},
  };
  return &table[(c / 4) % (int)(sizeof(table) / sizeof(table[0]))];
}

static int run_case(int idx, const char *dump_path) {
  stream_t s;
  mut_t m;
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  int hdr = 0, dsp = 0, vbi = 0, fails = 0, i;

  rng = 0x123456789abcdefULL ^ ((uint64_t)idx * 0x9e3779b97f4a7c15ULL);
  if (pt_encode_stream(&s, params_for(idx), rng) != 0) {
    fprintf(stderr, "case %d: encoder rejected params\n", idx);
    return 2;
  }
  apply_mutation(&s, idx % 4, &m);
  if (dump_path) dump_stream(&s, dump_path);

  printf("CASE %d cat=%s pkts=%d mutpkt=%d mutoff=%ld mutoff2=%ld mutcnt=%d\n",
         idx, cat_names[m.cat], s.n, m.pkt_idx, m.mut_off, m.mut_off2,
         m.count);

  vorbis_info_init(&vi);
  vorbis_comment_init(&vc);

  for (i = 0; i < s.n; i++) {
    ogg_packet op;
    memset(&op, 0, sizeof(op));
    op.packet = s.pkts[i].data;
    op.bytes = s.pkts[i].len;
    op.b_o_s = (i == 0);
    op.e_o_s = (i == s.n - 1);
    op.granulepos = s.pkts[i].granulepos;
    op.packetno = i;

    if (hdr < 3) {
      int rc = vorbis_synthesis_headerin(&vi, &vc, &op);
      if (rc != 0) {
        printf("FAIL case=%d cat=%s pkt=%d stage=header rc=%d mutoff=%ld "
               "pktlen=%ld\n",
               idx, cat_names[m.cat], i, rc, m.mut_off, s.pkts[i].len);
        fails++;
      } else {
        hdr++;
        if (hdr == 3) {
          if (vorbis_synthesis_init(&vd, &vi) == 0) {
            dsp = 1;
            vorbis_block_init(&vd, &vb);
            vbi = 1;
          }
        }
      }
    } else if (dsp) {
      if (vorbis_synthesis(&vb, &op) == 0) {
        float **pcm;
        int n;
        vorbis_synthesis_blockin(&vd, &vb);
        while ((n = vorbis_synthesis_pcmout(&vd, &pcm)) > 0)
          vorbis_synthesis_read(&vd, n);
      } else {
        printf("FAIL case=%d cat=%s pkt=%d stage=synth rc=OV_EBADPACKET "
               "mutoff=%ld pktlen=%ld\n",
               idx, cat_names[m.cat], i, m.mut_off, s.pkts[i].len);
        fails++;
      }
    }
  }

  if (vbi) vorbis_block_clear(&vb);
  if (dsp) vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);

  printf("DONE case=%d cat=%s fails=%d\n", idx, cat_names[m.cat], fails);
  pt_stream_free(&s);
  return 0;
}

int main(int argc, char **argv) {
  int i;
  if (argc >= 3 && strcmp(argv[1], "--case") == 0) {
    int idx = atoi(argv[2]);
    const char *dump = argc >= 5 && strcmp(argv[3], "--dump") == 0
                           ? argv[4]
                           : NULL;
    if (idx < 0 || idx >= NCASES) return 2;
    return run_case(idx, dump);
  }
  for (i = 0; i < NCASES; i++) run_case(i, NULL);
  return 0;
}
