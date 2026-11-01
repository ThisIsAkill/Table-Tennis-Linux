#!/usr/bin/env bash
# scripts/sweep.sh PREFIX SCENARIO FLAGSFILE
# One autotest run per line of FLAGSFILE (engine flags, space separated; "-" = none).
# Writes logs/runs/PREFIX-sweep.done when finished, with one result line per run.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${1:?prefix}"; scenario="${2:?scenario}"; flagsfile="${3:?flags file}"
report="$root/logs/runs/$prefix-sweep.txt"
mkdir -p "$root/logs/runs"; : > "$report"
i=0
while read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  i=$((i + 1)); name="$prefix-$(printf %02d $i)"
  [[ "$line" == "-" ]] && line=""
  # shellcheck disable=SC2086
  timeout 600 "$root/scripts/autotest.sh" "$name" "$scenario" $line > /dev/null 2>&1
  echo "$name [$line] $(sed -n 2p "$root/logs/runs/$name/summary.txt" 2>/dev/null)" >> "$report"
done < "$flagsfile"
command mv -f "$report" "$root/logs/runs/$prefix-sweep.done"
