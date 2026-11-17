#!/bin/bash
# Property test driver: 200 malformed-packet cases + 50 valid-stream
# reference comparisons. Writes summary and counterexamples to results/.
set -u
cd "$(dirname "$0")"

RES=results
CE=$RES/counterexamples
rm -rf "$RES"
mkdir -p "$CE" "$RES/logs"

export ASAN_OPTIONS="abort_on_error=1:detect_leaks=1:print_stacktrace=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

PASS=0
CRASH=0
SAN=0

for i in $(seq 0 199); do
  log="$RES/logs/case_$i.log"
  dump="$CE/case_$i.pktbin"
  ./fuzz_malformed --case "$i" --dump "$dump" >"$log" 2>&1
  rc=$?
  if [ $rc -ne 0 ] || grep -qE "AddressSanitizer|LeakSanitizer|runtime error|ERROR: " "$log"; then
    SAN=$((SAN+1))
    echo "SANITIZER_HIT case=$i rc=$rc log=$log dump=$dump" >> "$RES/sanitizer_hits.txt"
  else
    rm -f "$dump"
    PASS=$((PASS+1))
  fi
done

{
  echo "=== malformed packet property test (200 cases) ==="
  echo "clean cases (no crash/OOB/leak/UB): $PASS / 200"
  echo "sanitizer hits: $SAN"
  for c in trunc codebook duphdr bigblock; do
    n=$(grep -h "^CASE [0-9]* cat=$c " "$RES"/logs/case_*.log 2>/dev/null | wc -l)
    f=$(grep -h "^FAIL .*cat=$c " "$RES"/logs/case_*.log 2>/dev/null | wc -l)
    echo "category $c: cases=$n decode_rejections=$f"
  done
  echo
  echo "=== decode rejections with offsets (first 30) ==="
  grep -h "^FAIL " "$RES"/logs/case_*.log 2>/dev/null | head -30
  echo
  echo "total decode rejections: $(grep -h '^FAIL ' "$RES"/logs/case_*.log 2>/dev/null | wc -l)"
} > "$RES/summary.txt"

# --- valid streams vs reference decoder ---
./compare_valid "$CE" > "$RES/valid.log" 2>&1
vrc=$?
{
  echo
  echo "=== valid streams vs ffmpeg reference (50 streams) ==="
  grep "^VALID_SUMMARY" "$RES/valid.log"
  echo "maxdiff distribution (per-stream, top 5):"
  grep "^VALID .* status=OK" "$RES/valid.log" | sed 's/.*maxdiff=//' | sort -g | tail -5
  grep "status=MISMATCH\|status=.*FAIL" "$RES/valid.log" || echo "no mismatches"
} >> "$RES/summary.txt"

echo "counterexamples: $(ls "$CE" | wc -l) file(s) in $CE" >> "$RES/summary.txt"
cat "$RES/summary.txt"
exit $((SAN > 0 || vrc != 0))
