# Vorbis packet property tests

Two test programs, built against the in-tree libvorbis compiled with
ASan + UBSan + LSan (`build/`):

- `fuzz_malformed` — 200 malformed packet streams, categories strictly
  interleaved by case index (`case % 4`): truncation, illegal codebook
  (setup-header bit flips), duplicate headers, oversized blocks
  (4 KiB – 1 MiB). Every decode rejection is logged with the mutation
  byte offset. Any crash / OOB / leak / UB aborts the per-case process
  and the mutated input is kept as a counterexample.
- `compare_valid` — 50 valid streams (8k–48k Hz, mono/stereo,
  q0.1–0.8) decoded by libvorbis and by ffmpeg's reference decoder,
  compared sample by sample (tolerance 1e-3; ffmpeg does not apply
  granulepos end-trimming, so up to one block of reference tail
  padding is tolerated).

## Run

    ./run.sh

Rebuilds nothing; assumes `build/` exists (created with
`cmake -DCMAKE_C_FLAGS="-fsanitize=address,undefined ..."`).
Results land in `results/`:

- `results/summary.txt` — aggregate counts
- `results/logs/case_N.log` — per-case decode-rejection offsets
- `results/valid.log` — per-stream maxdiff
- `results/counterexamples/` — mutated inputs / ogg files that
  triggered a sanitizer or a sample mismatch (empty = none found)

Single case reproduction: `./fuzz_malformed --case N --dump out.pktbin`
