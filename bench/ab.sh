#!/bin/bash
# Noise-resistant A/B comparison of decoder settings.
# Runs every variant REPS times, interleaved (A B C A B C ...), with a cool-down
# pause before each run, and prints median / min / max of the average ms/image
# plus the all-core clock measured just before each run.
#
#   bench/ab.sh [REPS] [PAUSE_S] -- "label:ENV=val ENV2=val" "label2:..." ...
# Example:
#   bench/ab.sh 5 3 -- "own:NJ_ENGINE=0" "old:NJ_ENGINE=1"
set -e
cd "$(dirname "$0")/.."
REPS=${1:-5}; PAUSE=${2:-3}; shift 2; [ "$1" = "--" ] && shift
FILES=${FILES:-samples/*}
declare -a LABELS ENVS
for v in "$@"; do LABELS+=("${v%%:*}"); ENVS+=("${v#*:}"); done
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ab.XXXXXX")
for ((r = 1; r <= REPS; r++)); do
  for i in "${!LABELS[@]}"; do
    sleep "$PAUSE"
    ghz=$(bench/freqprobe 16 0.2 | awk '/t= 0.2s/{print $4}')
    ms=$(env ${ENVS[$i]} ONLY="parallel -> YUV" bench/bench $FILES 2>/dev/null | tail -1 | awk '{print $9}')
    echo "$ms $ghz" >> "$tmp/$i"
    printf "  run %d %-12s %6s ms/img  (clock before: %s GHz)\n" "$r" "${LABELS[$i]}" "$ms" "$ghz"
  done
done
echo
printf "%-12s %8s %8s %8s   %s\n" variant median min max "clock before (median)"
med() { sort -n | awk '{a[NR]=$1} END {n=NR; printf "%.2f %.2f %.2f", (n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2, a[1], a[n]}'; }
for i in "${!LABELS[@]}"; do
  read m mn mx <<< "$(cut -d' ' -f1 "$tmp/$i" | med)"
  read gm _ _ <<< "$(cut -d' ' -f2 "$tmp/$i" | med)"
  printf "%-12s %8s %8s %8s   %s GHz\n" "${LABELS[$i]}" "$m" "$mn" "$mx" "$gm"
done
rm -rf "$tmp"
