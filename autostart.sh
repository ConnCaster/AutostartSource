#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="/etc/systemd/system"
PREFIX="${PREFIX:-autostart-monitor-test}"
PAUSE_SEC="${PAUSE_SEC:-1}"
EVENT_TIMEOUT_SEC="${EVENT_TIMEOUT_SEC:-10}"
MONITOR_CMD="${MONITOR_CMD:-}"
KEEP="${KEEP:-0}"
DRY_RUN=0

usage() {
  cat <<USAGE
Usage: sudo [MONITOR_CMD='./path/to/autostart_monitor'] $0 [options]

Options:
  --prefix NAME      Prefix for temporary test units/dirs. Default: ${PREFIX}
  --pause SEC        Delay between operations. Default: ${PAUSE_SEC}
  --timeout SEC      Timeout for log checks when MONITOR_CMD is set. Default: ${EVENT_TIMEOUT_SEC}
  --keep             Do not clean test files at the end.
  --dry-run          Print actions without changing files.
  -h, --help         Show this help.

What it does:
  1) Creates a temporary .service unit under /etc/systemd/system.
  2) Creates new dependency dirs with required suffixes: *.wants and *.requires.
  3) Creates/removes symlinks inside those dirs to trigger AutostartCreateTask/AutostartRemoveTask.
  4) If /etc/systemd/system/multi-user.target.wants exists, also tests that existing watched dir.

If MONITOR_CMD is set, this script starts the monitor, writes its output to a temp log,
then checks that expected event/path pairs appeared in the log.
USAGE
}

log()  { printf '[%(%H:%M:%S)T] %s\n' -1 "$*"; }
warn() { printf '[%(%H:%M:%S)T] WARN: %s\n' -1 "$*" >&2; }
die()  { printf '[%(%H:%M:%S)T] ERROR: %s\n' -1 "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix requires value}"; shift 2 ;;
    --pause) PAUSE_SEC="${2:?--pause requires value}"; shift 2 ;;
    --timeout) EVENT_TIMEOUT_SEC="${2:?--timeout requires value}"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ "$PREFIX" != *"/"* && -n "$PREFIX" ]] || die "prefix must be a non-empty name without slash"
[[ -d "$ROOT" ]] || die "$ROOT does not exist"

if [[ "$DRY_RUN" -eq 0 && "${EUID}" -ne 0 ]]; then
  die "run as root: sudo $0"
fi

run() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
  else
    "$@"
  fi
}

write_file() {
  local path="$1"
  shift
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '+ cat > %q <<EOF\n%s\nEOF\n' "$path" "$*"
  else
    cat > "$path" <<< "$*"
  fi
}

TS="$(date +%Y%m%d-%H%M%S)-$$"
BASE="${PREFIX}-${TS}"
UNIT="${BASE}.service"
UNIT_PATH="${ROOT}/${UNIT}"
NEW_WANTS_DIR="${ROOT}/${BASE}.wants"
NEW_REQUIRES_DIR="${ROOT}/${BASE}.requires"
WANTS_LINK="${NEW_WANTS_DIR}/${UNIT}"
REQUIRES_LINK="${NEW_REQUIRES_DIR}/${UNIT}"
EXISTING_WANTS_DIR="${ROOT}/multi-user.target.wants"
EXISTING_WANTS_LINK="${EXISTING_WANTS_DIR}/${UNIT}"
TMP_LINK="/tmp/${UNIT}.link.$$"
MONITOR_PID=""
MONITOR_LOG=""
FAILED_CHECKS=0

cleanup() {
  local rc=$?
  if [[ "$KEEP" -eq 0 ]]; then
    log "cleanup"
    run rm -f "$WANTS_LINK" "$REQUIRES_LINK" "$EXISTING_WANTS_LINK" "$TMP_LINK"
    run rm -f "$UNIT_PATH"
    run rmdir "$NEW_WANTS_DIR" "$NEW_REQUIRES_DIR" 2>/dev/null || true
  else
    warn "--keep enabled; test files were left in place with prefix: $BASE"
  fi

  if [[ -n "$MONITOR_PID" ]]; then
    log "stopping monitor pid=$MONITOR_PID"
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
    [[ -n "$MONITOR_LOG" ]] && log "monitor log: $MONITOR_LOG"
  fi

  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_for_event() {
  local event_type="$1"
  local path="$2"
  local label="$3"

  if [[ -z "$MONITOR_LOG" || "$DRY_RUN" -eq 1 ]]; then
    log "expect: event=${event_type} path=${path} (${label})"
    return 0
  fi

  local end=$((SECONDS + EVENT_TIMEOUT_SEC))
  while (( SECONDS <= end )); do
    if grep -F "event=${event_type}" "$MONITOR_LOG" 2>/dev/null | grep -Fq "path=${path}"; then
      log "ok: ${label}"
      return 0
    fi
    sleep 0.2
  done

  warn "not found in monitor log: event=${event_type} path=${path} (${label})"
  FAILED_CHECKS=$((FAILED_CHECKS + 1))
  return 1
}

sleep_between_steps() {
  sleep "$PAUSE_SEC"
}

if [[ -n "$MONITOR_CMD" ]]; then
  MONITOR_LOG="/tmp/${BASE}.monitor.log"
  log "starting monitor: $MONITOR_CMD"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    bash -c "$MONITOR_CMD" > "$MONITOR_LOG" 2>&1 &
    MONITOR_PID=$!
    sleep 1
    if ! kill -0 "$MONITOR_PID" 2>/dev/null; then
      cat "$MONITOR_LOG" >&2 || true
      die "monitor process exited early"
    fi
  else
    log "dry-run: monitor would write to $MONITOR_LOG"
  fi
else
  log "MONITOR_CMD is not set; run your app in another terminal and compare with 'expect' lines."
fi

log "creating unit file: $UNIT_PATH"
write_file "$UNIT_PATH" "[Unit]
Description=Temporary unit for autostart monitor test (${BASE})

[Service]
Type=oneshot
ExecStart=/bin/true

[Install]
WantedBy=multi-user.target"
sleep_between_steps

log "creating new dependency dirs: $NEW_WANTS_DIR and $NEW_REQUIRES_DIR"
run mkdir -p "$NEW_WANTS_DIR" "$NEW_REQUIRES_DIR"
# The monitor marks newly created *.wants/*.requires dirs after seeing the directory event.
# Give it enough time to refresh watches before creating files inside them.
sleep_between_steps

log "creating task in new *.wants dir"
run ln -s "$UNIT_PATH" "$WANTS_LINK"
wait_for_event "AutostartCreateTask" "$WANTS_LINK" "create symlink inside newly-created .wants" || true
sleep_between_steps

log "removing task from new *.wants dir"
run rm -f "$WANTS_LINK"
wait_for_event "AutostartRemoveTask" "$WANTS_LINK" "remove symlink from newly-created .wants" || true
sleep_between_steps

log "moving task into new *.requires dir"
run ln -s "$UNIT_PATH" "$TMP_LINK"
run mv "$TMP_LINK" "$REQUIRES_LINK"
wait_for_event "AutostartCreateTask" "$REQUIRES_LINK" "move symlink into newly-created .requires" || true
sleep_between_steps

log "moving task out of new *.requires dir"
run mv "$REQUIRES_LINK" "$TMP_LINK"
wait_for_event "AutostartRemoveTask" "$REQUIRES_LINK" "move symlink out of newly-created .requires" || true
run rm -f "$TMP_LINK"
sleep_between_steps

if [[ -d "$EXISTING_WANTS_DIR" ]]; then
  log "creating/removing task in existing dir: $EXISTING_WANTS_DIR"
  run ln -s "$UNIT_PATH" "$EXISTING_WANTS_LINK"
  wait_for_event "AutostartCreateTask" "$EXISTING_WANTS_LINK" "create symlink inside existing .wants" || true
  sleep_between_steps

  run rm -f "$EXISTING_WANTS_LINK"
  wait_for_event "AutostartRemoveTask" "$EXISTING_WANTS_LINK" "remove symlink from existing .wants" || true
  sleep_between_steps
else
  warn "skip existing-dir test: $EXISTING_WANTS_DIR not found"
fi

if [[ "$FAILED_CHECKS" -eq 0 ]]; then
  log "done: all generated checks passed or were printed as expected events"
else
  warn "done with $FAILED_CHECKS missing expected event(s); inspect monitor output"
  [[ -n "$MONITOR_LOG" ]] && tail -n 80 "$MONITOR_LOG" >&2 || true
  exit 2
fi