#!/usr/bin/env bash
# Run both factories + per-primitive micro-benchmarks; tabulate.
#
# Usage:
#   ./bench.sh                   # default: factory + micro
#   ./bench.sh factory [args]    # only factory; args forwarded to both
#   ./bench.sh micro   [-n N]    # only micro
#
set -euo pipefail
cd "$(dirname "$0")"

VENV=.venv
PYTHON=${PYTHON:-python3}

if [ ! -d "$VENV" ]; then
    echo "[bench] creating venv at $VENV"
    "$PYTHON" -m venv "$VENV"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet -r python/requirements.txt
fi

echo "[bench] building C binaries"
(cd c && make --no-print-directory -s)

mode=all
if [ $# -gt 0 ]; then
    case "$1" in
        factory|micro|all) mode=$1; shift ;;
    esac
fi
ARGS=("$@")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --------------------------------------------------------- helpers
get() { awk -F= -v k="$1" '$1==k{print $2}' "$2"; }

run_factory() {
    echo ""
    echo "=== factory (Python / simpy) ==="
    "$VENV/bin/python" python/factory.py ${ARGS[@]+"${ARGS[@]}"} \
        | tee "$TMP/py.out"

    echo ""
    echo "=== factory (C / simpyc) ==="
    ./c/factory ${ARGS[@]+"${ARGS[@]}"} | tee "$TMP/c.out"

    py_wall=$(get  wall_seconds "$TMP/py.out")
    c_wall=$(get   wall_seconds "$TMP/c.out")
    py_mem=$(get   peak_rss_mb  "$TMP/py.out")
    c_mem=$(get    peak_rss_mb  "$TMP/c.out")
    py_prod=$(get  produced     "$TMP/py.out")
    c_prod=$(get   produced     "$TMP/c.out")
    py_ship=$(get  shipped      "$TMP/py.out")
    c_ship=$(get   shipped      "$TMP/c.out")
    py_pre=$(get   preempted    "$TMP/py.out")
    c_pre=$(get    preempted    "$TMP/c.out")
    py_rev=$(get   reviewed     "$TMP/py.out")
    c_rev=$(get    reviewed     "$TMP/c.out")
    speedup=$( "$VENV/bin/python" -c "print(f'{${py_wall}/${c_wall}:.1f}x')")
    memratio=$("$VENV/bin/python" -c "print(f'{${py_mem}/${c_mem}:.1f}x')")

    echo ""
    echo "=== factory comparison ==="
    printf "%-16s %14s %14s %14s\n" metric "simpy(py)" "simpyc(c)" ratio
    printf "%-16s %14s %14s %14s\n" ------ --------- --------- -----
    printf "%-16s %14s %14s %14s\n" "wall seconds"  "$py_wall" "$c_wall" "$speedup faster"
    printf "%-16s %14s %14s %14s\n" "peak RSS (MB)" "$py_mem"  "$c_mem"  "$memratio less"
    printf "%-16s %14s %14s\n"      "produced"      "$py_prod" "$c_prod"
    printf "%-16s %14s %14s\n"      "shipped"       "$py_ship" "$c_ship"
    printf "%-16s %14s %14s\n"      "preempted"     "$py_pre"  "$c_pre"
    printf "%-16s %14s %14s\n"      "reviewed"      "$py_rev"  "$c_rev"

    if [ "$py_prod" != "$c_prod" ] || [ "$py_ship" != "$c_ship" ] \
       || [ "$py_pre" != "$c_pre" ] || [ "$py_rev" != "$c_rev" ]; then
        echo ""
        echo "WARN: implementations disagree on counts."
    fi
}

run_micro() {
    echo ""
    echo "=== micro (Python / simpy) ==="
    "$VENV/bin/python" python/micro.py ${ARGS[@]+"${ARGS[@]}"} \
        | tee "$TMP/py.micro"

    echo ""
    echo "=== micro (C / simpyc) ==="
    ./c/micro ${ARGS[@]+"${ARGS[@]}"} | tee "$TMP/c.micro"

    echo ""
    echo "=== per-primitive breakdown ==="
    "$VENV/bin/python" - "$TMP/py.micro" "$TMP/c.micro" <<'PY'
import sys
py_file, c_file = sys.argv[1], sys.argv[2]

def parse(path):
    out = {}
    for line in open(path):
        if not line.startswith("bench="):
            continue
        kv = dict(p.split("=", 1) for p in line.strip().split())
        out[kv["bench"]] = {
            "n":    int(kv["ops"]),
            "wall": float(kv["wall_seconds"]),
            "ns":   float(kv["ns_per_op"]),
        }
    return out

py = parse(py_file)
c  = parse(c_file)

print(f"{'primitive':<22} {'ops':>9} {'py ns/op':>11} {'c ns/op':>11} {'speedup':>11}")
print(f"{'-'*22} {'-'*9} {'-'*11} {'-'*11} {'-'*11}")
total_py = total_c = 0
for name in py:
    if name not in c: continue
    pn, cn = py[name]['ns'], c[name]['ns']
    sp = pn / cn if cn > 0 else float('inf')
    total_py += py[name]['wall']
    total_c  += c[name]['wall']
    print(f"{name:<22} {py[name]['n']:>9d} {pn:>11.1f} {cn:>11.1f} {sp:>10.1f}x")

print(f"{'-'*22} {'-'*9} {'-'*11} {'-'*11} {'-'*11}")
print(f"{'TOTAL wall (s)':<22} {'':>9} {total_py:>11.3f} {total_c:>11.3f} "
      f"{total_py/total_c:>10.1f}x")
PY
}

case "$mode" in
    factory) run_factory ;;
    micro)   run_micro ;;
    all)     run_factory; echo ""; run_micro ;;
esac
