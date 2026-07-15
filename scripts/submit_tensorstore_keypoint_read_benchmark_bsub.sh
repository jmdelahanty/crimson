#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_ROOT=""
BINARY="${REPO_DIR}/release/tensorstore_keypoint_read_benchmark"
SUBMIT_HOST="${PALETTE_LSF_SUBMIT_HOST:-login1-citrus-poller}"
QUEUE="short"
NCORES=2
WALLTIME="1:00"
REPETITIONS=3
WAIT=0
APPTAINER_IMAGE="docker://ubuntu:24.04"

usage() {
  cat <<'EOF'
Usage: scripts/submit_tensorstore_keypoint_read_benchmark_bsub.sh \
  --output-root PATH [options]

The login host only submits the job. All benchmark reads execute inside the
LSF allocation.

Options:
  --binary PATH          Shared-filesystem benchmark executable
  --repo-dir PATH        Crimson checkout visible to compute nodes
  --submit-host HOST     SSH host used when bsub is unavailable locally
  --queue NAME           LSF queue (default: short)
  --ncores N             LSF slots (default: 2)
  --walltime H:MM        LSF wall time (default: 1:00)
  --repetitions N        Reversed-order benchmark repetitions (default: 3)
  --apptainer-image URI  Compute runtime for workstation-built binary
                         (default: docker://ubuntu:24.04)
  --wait                 Pass -K to bsub and wait for completion
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-root) OUTPUT_ROOT="$2"; shift 2 ;;
    --binary) BINARY="$2"; shift 2 ;;
    --repo-dir) REPO_DIR="$2"; shift 2 ;;
    --submit-host) SUBMIT_HOST="$2"; shift 2 ;;
    --queue) QUEUE="$2"; shift 2 ;;
    --ncores) NCORES="$2"; shift 2 ;;
    --walltime) WALLTIME="$2"; shift 2 ;;
    --repetitions) REPETITIONS="$2"; shift 2 ;;
    --apptainer-image) APPTAINER_IMAGE="$2"; shift 2 ;;
    --wait) WAIT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$OUTPUT_ROOT" ]] || { echo "--output-root is required" >&2; exit 2; }
[[ -x "$BINARY" ]] || { echo "Benchmark binary is not executable: $BINARY" >&2; exit 2; }
[[ "$NCORES" =~ ^[1-9][0-9]*$ ]] || { echo "--ncores must be positive" >&2; exit 2; }
[[ "$REPETITIONS" =~ ^[1-9][0-9]*$ ]] || { echo "--repetitions must be positive" >&2; exit 2; }

RUN_DIR="${OUTPUT_ROOT}/lsf"
mkdir -p "$RUN_DIR"
JOB_SCRIPT="${RUN_DIR}/run_compute_benchmark.sh"
STAGED_BINARY="${RUN_DIR}/tensorstore_keypoint_read_benchmark"
STAGED_RUNNER="${RUN_DIR}/run_tensorstore_keypoint_read_benchmark.py"
STAGED_IMAGE="${RUN_DIR}/benchmark-runtime.sif"
STAGED_WRAPPER="${RUN_DIR}/run_benchmark_in_container.sh"
STDOUT_LOG="${RUN_DIR}/compute.%J.out"
STDERR_LOG="${RUN_DIR}/compute.%J.err"

# Workstation /home aliases are not present on all compute nodes. Stage the
# harness and self-contained binary beside the shared PRFS fixtures.
cp "$BINARY" "$STAGED_BINARY"
cp "$REPO_DIR/tools/run_tensorstore_keypoint_read_benchmark.py" "$STAGED_RUNNER"
chmod +x "$STAGED_BINARY" "$STAGED_RUNNER"

cat >"$STAGED_WRAPPER" <<EOF
#!/usr/bin/env bash
set -euo pipefail
exec apptainer exec --bind /groups $(printf '%q' "$STAGED_IMAGE") \
  $(printf '%q' "$STAGED_BINARY") "\$@"
EOF
chmod +x "$STAGED_WRAPPER"

cat >"$JOB_SCRIPT" <<EOF
#!/usr/bin/env bash
set -euo pipefail
if [[ -z "\${LSB_JOBID:-}" ]]; then
  echo "This benchmark must run inside an LSF allocation." >&2
  exit 2
fi
command -v apptainer >/dev/null 2>&1 || {
  echo "apptainer is required on the compute node" >&2
  exit 2
}
if [[ ! -s $(printf '%q' "$STAGED_IMAGE") ]]; then
  apptainer pull $(printf '%q' "$STAGED_IMAGE") $(printf '%q' "$APPTAINER_IMAGE")
fi
cd $(printf '%q' "$OUTPUT_ROOT")
exec $(printf '%q' "$STAGED_RUNNER") run \
  --output-root $(printf '%q' "$OUTPUT_ROOT") \
  --binary $(printf '%q' "$STAGED_WRAPPER") \
  --host-label "compute-\${HOSTNAME:-unknown}" \
  --repetitions $(printf '%q' "$REPETITIONS")
EOF
chmod +x "$JOB_SCRIPT"

BSUB_ARGS=(
  -q "$QUEUE"
  -n "$NCORES"
  -W "$WALLTIME"
  -R "span[hosts=1] rusage[mem=4096]"
  -M 4096
  -J crimson_ts_keypoint_read
  -o "$STDOUT_LOG"
  -e "$STDERR_LOG"
)
if [[ "$WAIT" -eq 1 ]]; then
  BSUB_ARGS+=(-K)
fi

printf 'Command: bsub'
printf ' %q' "${BSUB_ARGS[@]}" bash "$JOB_SCRIPT"
printf '\n'

if command -v bsub >/dev/null 2>&1; then
  bsub "${BSUB_ARGS[@]}" bash "$JOB_SCRIPT"
else
  [[ -n "$SUBMIT_HOST" ]] || {
    echo "bsub unavailable and --submit-host is empty" >&2
    exit 2
  }
  printf -v REMOTE_COMMAND '%q ' bsub "${BSUB_ARGS[@]}" bash "$JOB_SCRIPT"
  ssh "$SUBMIT_HOST" "$REMOTE_COMMAND"
fi
