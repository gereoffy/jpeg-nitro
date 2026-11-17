#!/bin/sh
# nitrojxl against libjxl's djxl (float output) over sample files: jxltest.sh djxl jxldec refdir files...
DJXL=$1; DEC=$2; REF=$3; shift 3
mkdir -p "$REF"
for f in "$@"; do
    r="$REF/$(echo "$f" | tr '/ ' '__').pfm"
    [ -f "$r" ] || "$DJXL" "$f" "$r" >/dev/null 2>&1
    out=$("$DEC" "$f" "$REF/ours.pam" 2>&1 | tail -1)
    case "$out" in *"rc 0,"*)
        # djxl writes the image's own encoding: linear ones are compared after sRGB encoding
        cmp=$(python3 "$(dirname "$0")/jxlcompare.py" "$REF/ours.pam" "$r" 2>&1 | tail -1)
        case "$cmp" in "max 0.0"*) ;; *) lin=$(python3 "$(dirname "$0")/jxlcompare.py" "$REF/ours.pam" "$r" --linear 2>&1 | tail -1)
            case "$lin" in "max 0.0"*) cmp="$lin (linear)" ;; esac ;; esac ;;
    *) cmp="-" ;; esac
    echo "$out | $cmp"
    rm -f "$REF/ours.pam"
done
