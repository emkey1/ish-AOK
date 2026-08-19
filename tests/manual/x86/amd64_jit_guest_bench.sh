#!/bin/sh
set -eu

out_dir=${ISH_AOK_BENCH_OUT:-/tmp/amd64-jit-bench-$(date +%Y%m%d-%H%M%S)}
iterations=1
cases=

usage() {
    cat <<'EOF'
Usage: amd64_jit_guest_bench.sh [options]

Run a small guest-side amd64 JIT benchmark from inside iSH-AOK.

Options:
  -n N        Run each case N times. Default: 1
  -o DIR      Output directory. Default: /tmp/amd64-jit-bench-<timestamp>
  -c SPEC     Add a case as name=command
  -h          Show this help

Examples:
  sh /AOK/tests/x86/amd64_jit_guest_bench.sh
  sh /AOK/tests/x86/amd64_jit_guest_bench.sh -n 3
  sh /AOK/tests/x86/amd64_jit_guest_bench.sh -c 'ssh_v=ssh -V >/dev/null'
EOF
}

add_case() {
    spec=$1
    name=${spec%%=*}
    command=${spec#*=}
    if [ -z "$name" ] || [ "$name" = "$spec" ] || [ -z "$command" ]; then
        echo "invalid case spec: $spec" >&2
        exit 2
    fi
    if [ -z "$cases" ]; then
        cases="${name}|${command}"
    else
        cases="${cases}
${name}|${command}"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        -n)
            shift
            iterations=$1
            ;;
        -o)
            shift
            out_dir=$1
            ;;
        -c)
            shift
            add_case "$1"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ ! -w /proc/ish/amd64_jit ]; then
    echo "/proc/ish/amd64_jit is not available" >&2
    exit 1
fi

if [ -z "$cases" ]; then
    cases='true|/bin/true
uname|uname -a >/dev/null
busybox_help|busybox --help >/dev/null
apk_help|apk --help >/dev/null || true'
fi

mkdir -p "$out_dir"
summary="$out_dir/summary.tsv"
printf 'case\titeration\tmode\trc\twall_s\n' >"$summary"

run_one() {
    case_name=$1
    iteration=$2
    mode_name=$3
    mode_value=$4
    command=$5

    echo "$mode_value" >/proc/ish/amd64_jit
    start_s=$(date +%s)
    set +e
    sh -lc "$command" >/dev/null 2>/dev/null
    rc=$?
    set -e
    end_s=$(date +%s)
    wall_s=$((end_s - start_s))
    printf '%s\t%s\t%s\t%s\t%s\n' "$case_name" "$iteration" "$mode_name" "$rc" "$wall_s" >>"$summary"
}

printf '%s\n' "$cases" | while IFS='|' read -r case_name command; do
    i=1
    while [ "$i" -le "$iterations" ]; do
        run_one "$case_name" "$i" interp 0 "$command"
        run_one "$case_name" "$i" jit 1 "$command"
        i=$((i + 1))
    done
done

echo "amd64 JIT guest benchmark results: $out_dir"
cat "$summary"
