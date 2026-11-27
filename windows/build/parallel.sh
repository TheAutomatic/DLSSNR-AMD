# Sourced by the build scripts: their independent steps (compiler runs, shader sets) in parallel. par <command...> starts
# one in the background, at most NR_JOBS at a time (default: half the CPUs - a large unit takes 1-2 GB in the compiler);
# par_wait waits for every one and stops the build if any failed (set -e does not see a background step's status). What
# reads their output comes after par_wait; no step reads another's, so the outputs are those of one at a time.
if [[ -z "${NR_JOBS:-}" ]]; then NR_JOBS=$(( $(nproc 2>/dev/null || echo 2) / 2 )); (( NR_JOBS > 0 )) || NR_JOBS=1; fi
_par_pids=()
par() {
    while (( $(jobs -rp | wc -l) >= NR_JOBS )); do sleep 0.1; done
    "$@" &
    _par_pids+=($!)
}
par_wait() {
    local p rc=0
    for p in "${_par_pids[@]}"; do wait "$p" || rc=1; done
    _par_pids=()
    (( rc == 0 )) || { echo "a parallel build step failed (its messages above)" >&2; exit 1; }
}
