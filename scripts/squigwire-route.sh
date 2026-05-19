#!/usr/bin/env bash
set -euo pipefail

ACTION="${1:-up}"
PEQ_SINK_NAME="${PEQ_SINK_NAME:-peq_in}"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/squigwire"
REAL_SINK_FILE="$STATE_DIR/real_sink_node"

get_default_sink_node() {
  wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null \
    | sed -n 's/.*node.name = "\(.*\)"/\1/p' \
    | head -n 1
}

find_real_sink_node() {
  local node="${SQW_REAL_SINK_NODE:-}"
  if [[ -n "$node" ]]; then
    printf '%s\n' "$node"
    return
  fi

  node="$(get_default_sink_node || true)"
  if [[ -n "$node" && "$node" != "$PEQ_SINK_NAME" ]]; then
    printf '%s\n' "$node"
    return
  fi

  if [[ -f "$REAL_SINK_FILE" ]]; then
    cat "$REAL_SINK_FILE"
    return
  fi

  pw-link -i \
    | sed -n 's/^\(.*\):playback_FL$/\1/p' \
    | grep -v "^$PEQ_SINK_NAME$" \
    | head -n 1
}

ensure_peq_sink() {
  if ! pactl list short sinks | awk '{print $2}' | grep -qx "$PEQ_SINK_NAME"; then
    pactl load-module module-null-sink \
      sink_name="$PEQ_SINK_NAME" \
      sink_properties="device.description=PEQ-In" >/dev/null
  fi
}

unload_peq_sink_modules() {
  local module_ids
  module_ids="$(pactl list short modules | awk -v name="$PEQ_SINK_NAME" '$2=="module-null-sink" && index($0, "sink_name=" name) { print $1 }')"
  if [[ -n "$module_ids" ]]; then
    while IFS= read -r id; do
      [[ -n "$id" ]] && pactl unload-module "$id" || true
    done <<< "$module_ids"
  fi
}

port_exists() {
  pw-link -i | grep -qx "$1" || pw-link -o | grep -qx "$1"
}

wait_for_port() {
  local port="$1"
  local i
  for ((i = 0; i < 100; i++)); do
    if port_exists "$port"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for port: $port" >&2
  return 1
}

safe_link() {
  pw-link "$1" "$2" 2>/dev/null || true
}

safe_unlink() {
  pw-link -d "$1" "$2" 2>/dev/null || true
}

sink_id_by_node_name() {
  local node_name="$1"
  wpctl status -n | awk -v n="$node_name" '
    {
      for (i = 1; i < NF; ++i) {
        if ($i ~ /^[0-9]+\.$/ && $(i + 1) == n) {
          id = $i
          sub(/\.$/, "", id)
          print id
          exit
        }
      }
    }
  '
}

set_default_sink_node() {
  local node_name="$1"
  local sink_id
  sink_id="$(sink_id_by_node_name "$node_name")"
  if [[ -z "$sink_id" ]]; then
    echo "could not resolve sink id for node: $node_name" >&2
    return 1
  fi
  wpctl set-default "$sink_id"
}

route_up() {
  local real_sink
  mkdir -p "$STATE_DIR"

  real_sink="$(find_real_sink_node || true)"
  if [[ -z "$real_sink" ]]; then
    echo "could not determine real sink; set SQW_REAL_SINK_NODE in env" >&2
    exit 1
  fi

  printf '%s\n' "$real_sink" > "$REAL_SINK_FILE"
  ensure_peq_sink
  set_default_sink_node "$PEQ_SINK_NAME"

  wait_for_port "squigwire:input_FL"
  wait_for_port "squigwire:input_FR"
  wait_for_port "squigwire:output_FL"
  wait_for_port "squigwire:output_FR"
  wait_for_port "$PEQ_SINK_NAME:monitor_FL"
  wait_for_port "$PEQ_SINK_NAME:monitor_FR"
  wait_for_port "$real_sink:playback_FL"
  wait_for_port "$real_sink:playback_FR"

  # Clean legacy links from the old single-port topology.
  safe_unlink "$PEQ_SINK_NAME:monitor_FL" "squigwire:input"
  safe_unlink "$PEQ_SINK_NAME:monitor_FR" "squigwire:input"
  safe_unlink "squigwire:output" "$real_sink:playback_FL"
  safe_unlink "squigwire:output" "$real_sink:playback_FR"

  safe_link "$PEQ_SINK_NAME:monitor_FL" "squigwire:input_FL"
  safe_link "$PEQ_SINK_NAME:monitor_FR" "squigwire:input_FR"
  safe_link "squigwire:output_FL" "$real_sink:playback_FL"
  safe_link "squigwire:output_FR" "$real_sink:playback_FR"
}

route_down() {
  local real_sink
  real_sink="${SQW_REAL_SINK_NODE:-}"
  if [[ -z "$real_sink" && -f "$REAL_SINK_FILE" ]]; then
    real_sink="$(cat "$REAL_SINK_FILE")"
  fi

  safe_unlink "$PEQ_SINK_NAME:monitor_FL" "squigwire:input_FL"
  safe_unlink "$PEQ_SINK_NAME:monitor_FR" "squigwire:input_FR"
  safe_unlink "$PEQ_SINK_NAME:monitor_FL" "squigwire:input"
  safe_unlink "$PEQ_SINK_NAME:monitor_FR" "squigwire:input"
  if [[ -n "$real_sink" ]]; then
    safe_unlink "squigwire:output_FL" "$real_sink:playback_FL"
    safe_unlink "squigwire:output_FR" "$real_sink:playback_FR"
    safe_unlink "squigwire:output" "$real_sink:playback_FL"
    safe_unlink "squigwire:output" "$real_sink:playback_FR"
    set_default_sink_node "$real_sink" || true
  fi

  unload_peq_sink_modules
}

case "$ACTION" in
  up) route_up ;;
  down) route_down ;;
  *)
    echo "usage: $0 {up|down}" >&2
    exit 2
    ;;
esac
