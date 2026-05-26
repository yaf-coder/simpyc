#!/usr/bin/env bash
# Build + run both factories, tabulate wall time and peak RSS.
#
# Usage:  ./bench.sh [--sim-time=N] [--production-target=N] [...]
#
# Forwards all args to both runs so they exercise the same workload.
set -euo pipefail

cd "$(dirname "$0")"

VENV=.venv
PYTHON=${PYTHON:-python3}

# --- 1. ensure venv + simpy are present ------------------------------
if [ ! -d "$VENV" ]; then
    echo "[bench] creating venv at $VENV"
    "$PYTHON" -m venv "$VENV"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet -r python/requirements.txt
fi

# --- 2. build the C factory (and libsimpyc.a) ------------------------
echo "[bench] building C factory"
(cd c && make --no-print-directory -s)

ARGS=("$@")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- 3. run both implementations -------------------------------------
echo ""
echo "=== python / simpy ==="
"$VENV/bin/python" python/factory.py ${ARGS[@]+"${ARGS[@]}"} | tee "$TMP/py.out"

echo ""
echo "=== c / simpyc ==="
./c/factory ${ARGS[@]+"${ARGS[@]}"} | tee "$TMP/c.out"

# --- 4. tabulate ------------------------------------------------------
get() { awk -F= -v k="$1" '$1==k{print $2}' "$2"; }

py_wall=$(get wall_seconds "$TMP/py.out")
c_wall=$(get  wall_seconds "$TMP/c.out")
py_mem=$(get  peak_rss_mb  "$TMP/py.out")
c_mem=$(get   peak_rss_mb  "$TMP/c.out")
py_prod=$(get produced     "$TMP/py.out")
c_prod=$(get  produced     "$TMP/c.out")
py_ship=$(get shipped      "$TMP/py.out")
c_ship=$(get  shipped      "$TMP/c.out")

speedup=$("$VENV/bin/python" -c "print(f'{${py_wall}/${c_wall}:.1f}x')")
memratio=$("$VENV/bin/python" -c "print(f'{${py_mem}/${c_mem}:.1f}x')")

echo ""
echo "=== comparison ==="
printf "%-18s %14s %14s %12s\n" "metric" "simpy(py)" "simpyc(c)" "ratio"
printf "%-18s %14s %14s %12s\n" "------" "---------" "---------" "-----"
printf "%-18s %14s %14s %12s\n" "wall seconds"  "$py_wall" "$c_wall" "$speedup faster"
printf "%-18s %14s %14s %12s\n" "peak RSS (MB)" "$py_mem"  "$c_mem"  "$memratio less"
printf "%-18s %14s %14s\n"      "produced"      "$py_prod" "$c_prod"
printf "%-18s %14s %14s\n"      "shipped"       "$py_ship" "$c_ship"

# Sanity: both implementations should produce/ship identical counts.
if [ "$py_prod" != "$c_prod" ] || [ "$py_ship" != "$c_ship" ]; then
    echo ""
    echo "WARN: implementations disagree on produced/shipped counts."
fi
