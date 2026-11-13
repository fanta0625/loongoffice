#!/usr/bin/env bash
# Capture a live Draw hang without changing its document or terminating it.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 || ! $1 =~ ^[0-9]+$ ]]; then
    echo "Usage: sudo bash $0 SOFFICE_BIN_PID [NEW_OUTPUT_DIRECTORY]" >&2
    exit 2
fi
pdf_pid=$1
pdf_exe=$(readlink "/proc/$pdf_pid/exe")
if [[ ${pdf_exe##*/} != soffice.bin ]]; then
    echo "PID $pdf_pid is not soffice.bin: $pdf_exe" >&2
    exit 2
fi
command -v gdb >/dev/null || { echo "Install gdb first." >&2; exit 2; }
if [[ $# == 2 ]]; then
    pdf_out=$2
    mkdir -- "$pdf_out"
else
    pdf_out=$(mktemp -d /tmp/pdf-hang.XXXXXX)
fi
pdf_out=$(realpath -- "$pdf_out")
printf 'Capturing PID %s into %s\n' "$pdf_pid" "$pdf_out"

{
    date --iso-8601=ns
    uname -a
    printf 'executable: %s\n' "$pdf_exe"
    lscpu
    free -h
    cat "/proc/$pdf_pid/status"
    cat "/proc/$pdf_pid/smaps_rollup" 2>/dev/null || true
    # Backend libraries help distinguish GTK/Qt/X11 and graphics driver paths.
    awk '/libvclplug|libQt[56]|libcairo|libGL|libEGL|libpdfium/ {print}' \
        "/proc/$pdf_pid/maps"
} > "$pdf_out/environment.txt" 2>&1

# Each attach briefly suspends the process and detaches before the next sample.
# No core dump, document data, environment variables, or variable values.
for pdf_sample in 1 2 3; do
    if [[ ! -d /proc/$pdf_pid ]]; then
        echo "Process exited before sample $pdf_sample." >&2
        break
    fi
    {
        date --iso-8601=ns
        ps -L -p "$pdf_pid" -o pid,tid,stat,pcpu,wchan:32,comm
    } > "$pdf_out/threads-$pdf_sample.txt" 2>&1
    if ! gdb -nx -nh -batch \
        -iex 'set auto-load off' \
        -iex 'set debuginfod enabled off' \
        -ex 'set pagination off' \
        -ex 'set print thread-events off' \
        -ex 'set print frame-arguments none' \
        -ex 'thread apply all bt 40' \
        -ex detach -ex quit -p "$pdf_pid" \
        > "$pdf_out/stacks-$pdf_sample.txt" 2>&1; then
        echo "gdb could not capture sample $pdf_sample; see stacks-$pdf_sample.txt." >&2
        exit 1
    fi
    if [[ $pdf_sample != 3 ]]; then
        sleep 1
    fi
done
printf 'Report: %s\n' "$pdf_out"
