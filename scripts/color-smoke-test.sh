#!/bin/sh
# Smoke test for colored, streamed output.  Usage: color-smoke-test.sh MAKE
#
# Checks that:
#  - with --color=always, errors and warnings in recipe output are colored
#    and each line shows while the recipe is still running;
#  - with the default --color=auto and output to a pipe, nothing is colored;
#  - a background process that keeps the pipe open does not hang make.

set -u

make=${1:?usage: $0 MAKE}
tmp=${TMPDIR:-/tmp}/color-smoke.$$
mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' 0

esc=$(printf '\033')
fail=0

report () {
  if [ "$1" -eq 0 ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi
}

cat > "$tmp/color.mk" <<'EOF'
all: a b
a:
	@echo "a1: file.c:1: error E1: one"; sleep 2; echo "a2: file.c:2: warning W2: two"
b:
	@echo "b1 start" >&2; sleep 1; echo "b2: x error error y"
bg:
	@(sleep 5 &); echo "background started"
EOF

# Prefix each line with the seconds since START.
stamp () {
  start=$1
  while IFS= read -r line; do
    echo "$(( $(date +%s) - start )) $line"
  done
}

start=$(date +%s)
"$make" -f "$tmp/color.mk" -j2 --color=always 2>&1 | stamp "$start" > "$tmp/always.txt"
cat "$tmp/always.txt"

grep "a1:" "$tmp/always.txt" | grep -q "${esc}\[91merror${esc}\[0m"
report $? "errors are colored with --color=always"

grep "a2:" "$tmp/always.txt" | grep -q "${esc}\[95mwarning${esc}\[0m"
report $? "warnings are colored with --color=always"

grep "b2:" "$tmp/always.txt" | grep -q "${esc}\[91merror${esc}\[0m ${esc}\[91merror${esc}\[0m"
report $? "repeated words are all colored"

# a1 is printed two seconds before its recipe ends.
a1=$(grep "a1:" "$tmp/always.txt" | cut -d' ' -f1)
a2=$(grep "a2:" "$tmp/always.txt" | cut -d' ' -f1)
[ -n "$a1" ] && [ -n "$a2" ] && [ "$a1" -lt "$a2" ]
report $? "output streams while the recipe runs (a1 at ${a1:-?}s, a2 at ${a2:-?}s)"

"$make" -f "$tmp/color.mk" -j2 2>&1 | cat > "$tmp/auto.txt"
! grep -q "$esc" "$tmp/auto.txt"
report $? "no colors with --color=auto when output is a pipe"

NO_COLOR=1 "$make" -f "$tmp/color.mk" -j2 --color=auto 2>&1 | cat > "$tmp/nocolor.txt"
! grep -q "$esc" "$tmp/nocolor.txt"
report $? "NO_COLOR is honored"

start=$(date +%s)
"$make" -f "$tmp/color.mk" bg --color=always > "$tmp/bg.txt" 2>&1
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 4 ]
report $? "a background process holding the pipe does not hang make (${elapsed}s)"

exit $fail
