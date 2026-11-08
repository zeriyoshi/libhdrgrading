#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Picks how many compiles and tests run at once from the cores and the memory free right now, so a
# build and its tests leave room for whatever else the machine runs.
#
#   scripts/jobs.sh                                   print the count
#   scripts/jobs.sh cmake --workflow --preset dev     run a command with CMAKE_BUILD_PARALLEL_LEVEL
#                                                     and CTEST_PARALLEL_LEVEL set to it
#
# HDRG_JOB_MIB is the memory one job may take (default 1024: the largest compile, test, sanitized
# test and memcheck run measured stay under 700 MiB). An eighth of the memory, at least 2 GiB, is
# left free. HDRG_JOBS_CAP caps the count (scripts/docker_dev.sh passes the host's
# into the container), and a level already in the environment is kept.
set -euo pipefail

job_mib="${HDRG_JOB_MIB:-1024}"

case "$(uname -s)" in
    Darwin)
        cores="$(sysctl -n hw.ncpu)"
        total_mib=$(($(sysctl -n hw.memsize) / 1048576))
        # the kernel's estimate of the memory free for use (memory_pressure's free percentage)
        free_mib=$((total_mib * $(sysctl -n kern.memorystatus_level) / 100))
        ;;
    *)
        cores="$(nproc)"
        total_mib=$(($(awk '/^MemTotal:/ { print $2 }' /proc/meminfo) / 1024))
        free_mib=$(($(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo) / 1024))
        limit="$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo max)"
        if [[ "$limit" =~ ^[0-9]+$ ]]; then
            used="$(cat /sys/fs/cgroup/memory.current 2>/dev/null || echo 0)"
            total_mib=$((limit / 1048576 < total_mib ? limit / 1048576 : total_mib))
            room_mib=$(((limit - used) / 1048576))
            free_mib=$((room_mib < free_mib ? room_mib : free_mib))
        fi
        ;;
esac

reserve_mib=$((total_mib / 8 > 2048 ? total_mib / 8 : 2048))
jobs=$(((free_mib - reserve_mib) / job_mib))
if ((jobs > cores)); then
    jobs="$cores"
fi
if [[ -n "${HDRG_JOBS_CAP:-}" ]] && ((jobs > HDRG_JOBS_CAP)); then
    jobs="$HDRG_JOBS_CAP"
fi
if ((jobs < 1)); then
    printf 'warning: %s MiB free (%s MiB kept free, %s MiB a job): one job at a time\n' \
        "$free_mib" "$reserve_mib" "$job_mib" >&2
    jobs=1
fi

if [[ $# -eq 0 ]]; then
    echo "$jobs"
    exit 0
fi

export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$jobs}"
export CTEST_PARALLEL_LEVEL="${CTEST_PARALLEL_LEVEL:-$jobs}"
printf 'jobs: build %s, test %s (%s MiB free of %s, %s cores)\n' "$CMAKE_BUILD_PARALLEL_LEVEL" \
    "$CTEST_PARALLEL_LEVEL" "$free_mib" "$total_mib" "$cores" >&2
exec "$@"
