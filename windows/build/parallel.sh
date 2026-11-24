#!/usr/bin/env bash
# Bounded parallel job runner using a FIFO semaphore (safe under set -euo pipefail).
# Usage:
#   source windows/build/parallel.sh
#   nr_par_init [max_jobs]
#   nr_par_run cmd arg1 ...
#   nr_par_wait

_NR_PAR_FD=${_NR_PAR_FD:-}
if [[ -z "${_NR_PAR_PIDS+x}" ]]; then
    _NR_PAR_PIDS=()
fi

nr_jobs_default() {
    local n="${NR_JOBS:-}"
    if [[ "$n" =~ ^[1-9][0-9]*$ ]]; then
        printf '%s\n' "$n"
        return
    fi
    n=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    [[ "$n" =~ ^[1-9][0-9]*$ ]] || n=4
    printf '%s\n' "$n"
}

nr_par_init() {
    local jobs="${1:-$(nr_jobs_default)}"
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || jobs=1
    if [[ -n "${_NR_PAR_FD:-}" ]]; then
        eval "exec ${_NR_PAR_FD}>&-"
        _NR_PAR_FD=""
    fi
    local dir
    dir=$(mktemp -d "${TMPDIR:-/tmp}/nrpar.XXXXXX")
    mkfifo -- "$dir/fifo"
    exec {_NR_PAR_FD}<>"$dir/fifo"
    rm -rf -- "$dir"
    local i
    for ((i = 0; i < jobs; i++)); do
        printf '\n' >&"$_NR_PAR_FD"
    done
    _NR_PAR_PIDS=()
}

nr_par_run() {
    local _tok=""
    IFS= read -r -u "$_NR_PAR_FD" _tok
    (
        local rc=0
        "$@" || rc=$?
        printf '\n' >&"$_NR_PAR_FD"
        exit "$rc"
    ) &
    _NR_PAR_PIDS+=($!)
}

nr_par_wait() {
    local fail=0 pid
    if ((${#_NR_PAR_PIDS[@]})); then
        for pid in "${_NR_PAR_PIDS[@]}"; do
            wait "$pid" || fail=1
        done
    fi
    if [[ -n "${_NR_PAR_FD:-}" ]]; then
        eval "exec ${_NR_PAR_FD}>&-"
        _NR_PAR_FD=""
    fi
    _NR_PAR_PIDS=()
    return "$fail"
}
