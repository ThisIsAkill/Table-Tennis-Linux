#!/usr/bin/env bash
# System link test on one machine: two game instances that see each other as separate hosts.
#
#   scripts/lan-test.sh NAME HOST_SCENARIO JOIN_SCENARIO [extra engine flags...]
#
# Runs scripts/autotest.sh twice in parallel under one GPU lock, each with its own virtual
# display (:95 host, :94 joiner), build folder copy and user data folder. The instances use the
# loopback addresses 127.0.0.2 (host) and 127.0.0.3 (joiner): a loopback net_address makes the
# engine bind sockets to it, and net_peers turns the system link broadcast into packets to the
# other instance, so no root, network namespace or real network is needed. The players are
# named Host and Guest (the session layer tells players apart by gamertag).
#
# Output: logs/runs/NAME/host and logs/runs/NAME/join (frames, logs, summary.txt, as autotest).
# TT_JOIN_DELAY=seconds starts the joiner later (default 20), so the host is in its lobby first.
# TT_SPEC_SCENARIO=FILE adds a third instance, Spectator (display :93, 127.0.0.4), started
# TT_SPEC_DELAY seconds after the joiner (default 60); output in logs/runs/NAME/spec.
# A match with a spectator (Table Tennis Network > Exhibition):
#   TT_WINDOW=640x360 TT_JOIN_DELAY=15 TT_SPEC_DELAY=40 \
#   TT_SPEC_SCENARIO=scripts/scenarios/lan-spectate.txt \
#   scripts/lan-test.sh spectate scripts/scenarios/lan-host.txt scripts/scenarios/lan-join.txt
# TT_HOST_FLAGS / TT_JOIN_FLAGS / TT_SPEC_FLAGS add engine flags to one instance only, e.g.
# TT_JOIN_FLAGS="--online_test_delay_ms=300 --online_utc_offset=330" for a distant player.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="${1:?run name}"; host_scenario="${2:?host scenario}"; join_scenario="${3:?join scenario}"
shift 3
out="$root/logs/runs/$name"
[[ -f "$root/scripts/autotest.local" ]] && source "$root/scripts/autotest.local"
if [[ -z "${TT_LOCKED:-}" && -n "${TT_GPU_LOCK:-}" ]]; then
  [[ -e "$out" ]] && { echo "$out exists; pick another name"; exit 2; }
  exec env TT_LOCKED=1 flock -w 1800 "$TT_GPU_LOCK" "$0" "$name" "$host_scenario" "$join_scenario" "$@"
fi
[[ -z "${TT_LOCKED:-}" && -e "$out" ]] && { echo "$out exists; pick another name"; exit 2; }
pgrep -x tabletennis >/dev/null && { echo "tabletennis already running"; exit 1; }
mkdir -p "$out"

# The engine finds its settings, logs and libraries next to the real executable, so each
# instance gets its own folder with hard links to the binaries (no extra space); the large
# read-only folders are symlinked. Delete logs/runs/NAME when done.
stage() {
  local role="$1" dist="$out/dist-$1"
  mkdir -p "$dist"
  for f in tabletennis librexruntime.so librexgpu-xenos.so tt-recomp.sh; do
    command cp -lf "$root/dist/$f" "$dist/" 2>/dev/null || command cp -f "$root/dist/$f" "$dist/"
  done
  command cp -f "$root/dist/tt-recomp.ini" "$dist/"
  for d in assets fonts licenses; do ln -sfn "$root/dist/$d" "$dist/$d"; done
  mkdir -p "$out/user-$role"
  # Saves and shader cache of the normal test build (see tabletennis_app.h), so both skip the
  # first-run save prompt and start with compiled shaders.
  [[ -d "$root/user_data" ]] && command cp -r "$root/user_data/." "$out/user-$role/"
  [[ -d "$root/cache" ]] && command cp -r "$root/cache" "$out/user-$role/"
}
stage host
stage join
spec_scenario="${TT_SPEC_SCENARIO:-}"
[[ -n "$spec_scenario" ]] && stage spec

run() {
  local role="$1" display="$2" scenario="$3" self="$4" peer="$5" player="$6"
  shift 6
  TT_LOCKED=1 TT_PARALLEL=1 TT_DISPLAY="$display" TT_DIST="$out/dist-$role" \
    "$root/scripts/autotest.sh" "$name/$role" "$scenario" \
    "--user_data_root=$out/user-$role" "--net_address=$self" "--net_peers=$peer" "--user_name=$player" "$@" \
    > "$out/$role.out" 2>&1
}
read -r -a host_flags <<< "${TT_HOST_FLAGS:-}"
read -r -a join_flags <<< "${TT_JOIN_FLAGS:-}"
read -r -a spec_flags <<< "${TT_SPEC_FLAGS:-}"
host_peers=127.0.0.3 join_peers=127.0.0.2
if [[ -n "$spec_scenario" ]]; then
  host_peers=127.0.0.3,127.0.0.4 join_peers=127.0.0.2,127.0.0.4
fi
run host :95 "$host_scenario" 127.0.0.2 "$host_peers" Host "$@" "${host_flags[@]}" &
host_pid=$!
sleep "${TT_JOIN_DELAY:-20}"
run join :94 "$join_scenario" 127.0.0.3 "$join_peers" Guest "$@" "${join_flags[@]}" &
join_pid=$!
spec_pid=
if [[ -n "$spec_scenario" ]]; then
  sleep "${TT_SPEC_DELAY:-60}"
  run spec :93 "$spec_scenario" 127.0.0.4 127.0.0.2,127.0.0.3 Spectator "$@" "${spec_flags[@]}" &
  spec_pid=$!
fi
wait "$host_pid"; wait "$join_pid"
[[ -n "$spec_pid" ]] && wait "$spec_pid"

cat "$out/host.out" "$out/join.out"
[[ -n "$spec_pid" ]] && cat "$out/spec.out"
