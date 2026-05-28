#!/usr/bin/env bash
# test_subscribe.sh — Simulates the Ambxst subscribe flow against the axctl daemon.
#
# Usage: ./test_subscribe.sh [socket_path]
#
# Connects to the daemon, sends System.Subscribe, parses the initial State.Dump,
# and validates the JSON structure matches what QML expects.
# Then listens for subsequent events and validates those too.
#
# Requirements: socat, jq

set -euo pipefail

SOCKET="${1:-/tmp/axctl-$(id -u).sock}"
TIMEOUT=10
PASS=0
FAIL=0
WARN=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; WARN=$((WARN+1)); }

check_deps() {
    for cmd in socat jq; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "ERROR: $cmd is required but not installed."
            echo "  sudo apt install $cmd"
            exit 1
        fi
    done
}

# Validate a single monitor object from the state dump
validate_monitor() {
    local mon="$1" idx="$2"
    local id name focused aw_id

    id=$(echo "$mon" | jq -r '.id // empty')
    name=$(echo "$mon" | jq -r '.name // empty')
    focused=$(echo "$mon" | jq -r '.is_focused // empty')
    aw_id=$(echo "$mon" | jq -r '.metadata.active_workspace // empty')

    if [ -z "$id" ]; then
        fail "Monitor[$idx]: missing .id"
    else
        pass "Monitor[$idx]: id=$id"
    fi

    if [ -z "$name" ]; then
        fail "Monitor[$idx]: missing .name"
    else
        pass "Monitor[$idx]: name=$name"
    fi

    if [ -z "$focused" ]; then
        warn "Monitor[$idx]: missing .is_focused"
    fi

    # Critical: active_workspace must be a valid number string, not ""
    if [ -z "$aw_id" ]; then
        fail "Monitor[$idx]: metadata.active_workspace is EMPTY — will cause negative workspace IDs in QML"
    elif ! [[ "$aw_id" =~ ^[0-9]+$ ]]; then
        warn "Monitor[$idx]: metadata.active_workspace='$aw_id' — non-numeric, parseInt will yield NaN → 0 → negative group"
    else
        pass "Monitor[$idx]: metadata.active_workspace=$aw_id"
    fi
}

# Validate a single workspace object
validate_workspace() {
    local ws="$1" idx="$2"
    local id name mon_id

    id=$(echo "$ws" | jq -r '.id // empty')
    name=$(echo "$ws" | jq -r '.name // empty')
    mon_id=$(echo "$ws" | jq -r '.monitor_id // empty')

    if [ -z "$id" ]; then
        fail "Workspace[$idx]: missing .id"
    else
        pass "Workspace[$idx]: id=$id name=${name:-?}"
    fi

    if [ -z "$mon_id" ]; then
        warn "Workspace[$idx]: missing .monitor_id"
    fi
}

# Validate a single window object
validate_window() {
    local win="$1" idx="$2"
    local id app_id ws_id

    id=$(echo "$win" | jq -r '.id // empty')
    app_id=$(echo "$win" | jq -r '.app_id // empty')
    ws_id=$(echo "$win" | jq -r '.workspace_id // empty')

    if [ -z "$id" ]; then
        fail "Window[$idx]: missing .id"
    else
        pass "Window[$idx]: id=${id:0:16}... app=$app_id ws=$ws_id"
    fi
}

# Validate the full state dump
validate_state() {
    local state="$1" label="$2"

    log "--- Validating state from: $label ---"

    # Check top-level keys
    for key in windows workspaces monitors; do
        if echo "$state" | jq -e ".$key" >/dev/null 2>&1; then
            local count
            count=$(echo "$state" | jq ".$key | length")
            pass "$label: .$key present (count=$count)"
        else
            fail "$label: .$key MISSING — QML will get undefined!"
        fi
    done

    # Validate each monitor
    local mon_count
    mon_count=$(echo "$state" | jq '.monitors | length')
    for ((i=0; i<mon_count; i++)); do
        validate_monitor "$(echo "$state" | jq ".monitors[$i]")" "$i"
    done

    # Validate each workspace
    local ws_count
    ws_count=$(echo "$state" | jq '.workspaces | length')
    for ((i=0; i<ws_count; i++)); do
        validate_workspace "$(echo "$state" | jq ".workspaces[$i]")" "$i"
    done

    # Validate each window (only first 5 to keep output manageable)
    local win_count
    win_count=$(echo "$state" | jq '.windows | length')
    local max=$((win_count > 5 ? 5 : win_count))
    for ((i=0; i<max; i++)); do
        validate_window "$(echo "$state" | jq ".windows[$i]")" "$i"
    done
    if [ "$win_count" -gt 5 ]; then
        log "  ... and $((win_count - 5)) more windows"
    fi

    # Cross-check: each workspace's monitor_id should exist in monitors
    for ((i=0; i<ws_count; i++)); do
        local ws_mon
        ws_mon=$(echo "$state" | jq -r ".workspaces[$i].monitor_id // empty")
        if [ -n "$ws_mon" ]; then
            local found
            found=$(echo "$state" | jq --arg m "$ws_mon" '[.monitors[] | select(.id == $m or .name == $m)] | length')
            if [ "$found" -eq 0 ]; then
                warn "Workspace[$i] monitor_id='$ws_mon' not found in monitors list"
            fi
        fi
    done
}

# ---- Main ----
check_deps

if [ ! -S "$SOCKET" ]; then
    echo -e "${RED}ERROR: Socket not found at $SOCKET${NC}"
    echo "Is the axctl daemon running?"
    echo "  axctl daemon &"
    echo ""
    echo "To test with a mock, you can pipe sample JSON:"
    echo "  echo '{\"jsonrpc\":\"2.0\",\"method\":\"State.Dump\",\"state\":{\"windows\":[],\"workspaces\":[],\"monitors\":[]}}' | jq ."
    exit 1
fi

log "Connecting to $SOCKET..."

# Send System.Subscribe and read the initial response
RESPONSE=$(echo '{"jsonrpc":"2.0","id":1,"method":"System.Subscribe","params":{}}' | \
    socat - UNIX-CONNECT:"$SOCKET" | \
    head -2 | tail -1)

if [ -z "$RESPONSE" ]; then
    fail "No response from daemon"
    exit 1
fi

log "Got response (${#RESPONSE} bytes)"

# First line is the RPC result, second line (if present) might be the initial state
# Actually the subscribe response comes in two parts:
# 1. The State.Dump notification (method: State.Dump, has .state)
# 2. The RPC response (has .id and .result: "subscribed")

# Try to find the State.Dump in the combined output
COMBINED=$(echo '{"jsonrpc":"2.0","id":1,"method":"System.Subscribe","params":{}}' | \
    timeout "$TIMEOUT" socat - UNIX-CONNECT:"$SOCKET" 2>/dev/null || true)

log "Raw output lines: $(echo "$COMBINED" | wc -l)"

# Process each line
echo "$COMBINED" | while IFS= read -r line; do
    [ -z "$line" ] && continue

    method=$(echo "$line" | jq -r '.method // empty' 2>/dev/null)
    has_state=$(echo "$line" | jq -e '.state' >/dev/null 2>&1&& echo "yes" || echo "no")
    has_result=$(echo "$line" | jq -r '.result // empty' 2>/dev/null)

    if [ "$has_state" = "yes" ]; then
        log "Found state dump (method=$method)"
        state=$(echo "$line" | jq '.state')
        validate_state "$state" "$method"
    elif [ -n "$has_result" ]; then
        pass "RPC response: result=$has_result"
    else
        log "Unknown message: ${line:0:200}..."
    fi
done

echo ""
echo -e "=========================================="
echo -e "  Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, ${YELLOW}$WARN warnings${NC}"
echo -e "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}Some checks failed — review above output${NC}"
    exit 1
fi
