#!/bin/sh
set -eu

write_test=0
if [ "${1:-}" = "--write-readback" ]; then
    write_test=1
elif [ "$#" -ne 0 ]; then
    echo "usage: $0 [--write-readback]" >&2
    exit 2
fi

echo "arch=$(uname -m)"
policy_count=0
seen_efficiency=0
seen_performance=0
seen_prime=0
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$policy" ] || continue
    policy_count=$((policy_count + 1))
    cpus=$(tr '\n' ' ' < "$policy/related_cpus" | sed 's/[[:space:]]*$//')
    hw_min=$(cat "$policy/cpuinfo_min_freq")
    hw_max=$(cat "$policy/cpuinfo_max_freq")
    opps=$(cat "$policy/scaling_available_frequencies")
    sorted_opps=$(printf '%s\n' "$opps" | tr ' ' '\n' | sed '/^$/d' | sort -n)
    opp_count=$(printf '%s\n' "$sorted_opps" | wc -l)
    first=$(printf '%s\n' "$sorted_opps" | sed -n '1p')
    second=$(printf '%s\n' "$sorted_opps" | sed -n '2p')
    last=$(printf '%s\n' "$sorted_opps" | tail -n 1)
    if [ "$opp_count" -lt 2 ] || [ "$first" != "$hw_min" ] ||
       [ "$last" != "$hw_max" ]; then
        echo "cpu_opp_validation=FAIL policy=$(basename "$policy")" >&2
        exit 1
    fi
    case "$cpus" in
        "0 1 2") seen_efficiency=1 ;;
        "3 4 5 6") seen_performance=1 ;;
        "7") seen_prime=1 ;;
    esac
    # This window lies strictly between adjacent OPPs, so it contains no OPP.
    narrow_min=$((first + 1))
    narrow_max=$((second - 1))
    for frequency in $sorted_opps; do
        if [ "$frequency" -ge "$narrow_min" ] &&
           [ "$frequency" -le "$narrow_max" ]; then
            echo "no_opp_window=FAIL policy=$(basename "$policy")" >&2
            exit 1
        fi
    done
    # Capture the active governor: it decides how scaling_min/max_freq are
    # honored.  The daemon only writes the min/max window, so any governor works,
    # but recording it makes the reference environment reproducible and flags a
    # setup (e.g. a locked userspace governor) that could mask the daemon.
    governor="unknown"
    [ -r "$policy/scaling_governor" ] &&
        governor=$(cat "$policy/scaling_governor")
    available_governors="unknown"
    [ -r "$policy/scaling_available_governors" ] &&
        available_governors=$(tr '\n' ' ' < "$policy/scaling_available_governors" |
            sed 's/[[:space:]]*$//')
    echo "cpu_policy=$(basename "$policy") cpus=$cpus hw_khz=$hw_min-$hw_max opp_count=$opp_count"
    echo "no_opp_window_khz=$narrow_min-$narrow_max ceiling_choice_khz=$first"
    echo "cpu_governor=$(basename "$policy") active=$governor available=$available_governors"
done
echo "cpu_policy_count=$policy_count"
if [ "$policy_count" -ne 3 ] || [ "$seen_efficiency" -ne 1 ] ||
   [ "$seen_performance" -ne 1 ] || [ "$seen_prime" -ne 1 ]; then
    echo "cpu_topology=FAIL(expected masks: 0-2,3-6,7)" >&2
    exit 1
fi
echo "cpu_topology=PASS"
echo "cpu_opp_windows=PASS"

gpu=/sys/class/devfreq/3d00000.gpu
if [ -r "$gpu/available_frequencies" ]; then
    gpu_opps=$(cat "$gpu/available_frequencies")
    gpu_sorted=$(printf '%s\n' "$gpu_opps" | tr ' ' '\n' | sed '/^$/d' | sort -n)
    gpu_opp_min=$(printf '%s\n' "$gpu_sorted" | sed -n '1p')
    gpu_opp_max=$(printf '%s\n' "$gpu_sorted" | tail -n 1)
    gpu_count=$(printf '%s\n' "$gpu_sorted" | wc -l)
    gpu_policy_min=$(cat "$gpu/min_freq")
    gpu_policy_max=$(cat "$gpu/max_freq")
    echo "gpu=$(cat "$gpu/name") policy_hz=$gpu_policy_min-$gpu_policy_max opp_hz=$gpu_opp_min-$gpu_opp_max opp_count=$gpu_count"
    if [ "$gpu_count" -lt 2 ] ||
       [ "$gpu_policy_min" -lt "$gpu_opp_min" ] ||
       [ "$gpu_policy_max" -gt "$gpu_opp_max" ] ||
       [ "$gpu_policy_min" -gt "$gpu_policy_max" ]; then
        echo "gpu_opp=FAIL" >&2
        exit 1
    fi
    echo "gpu_opp=PASS"
else
    echo "gpu=FAIL(unavailable)" >&2
    exit 1
fi

# Thermal zones drive throttling, which competes with the daemon's frequency
# window.  Record each zone's type and current reading so a captured trace can
# be correlated with any thermal ceiling the daemon observes.  This is
# read-only telemetry: missing zones are reported, not treated as a failure.
thermal_zone_count=0
for zone in /sys/class/thermal/thermal_zone*; do
    [ -d "$zone" ] || continue
    thermal_zone_count=$((thermal_zone_count + 1))
    zone_type="unknown"
    [ -r "$zone/type" ] && zone_type=$(cat "$zone/type")
    zone_temp="unknown"
    [ -r "$zone/temp" ] && zone_temp=$(cat "$zone/temp")
    echo "thermal_zone=$(basename "$zone") type=$zone_type temp_mC=$zone_temp"
done
echo "thermal_zone_count=$thermal_zone_count"
if [ "$thermal_zone_count" -eq 0 ]; then
    echo "thermal=WARN(no thermal zones exposed)" >&2
else
    echo "thermal=PASS"
fi

if [ "$write_test" -eq 0 ]; then
    echo "write_readback=SKIP(use --write-readback as root)"
    exit 0
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "write_readback=FAIL(root required)" >&2
    exit 1
fi
if pgrep -x uperf-linux >/dev/null 2>&1; then
    echo "write_readback=FAIL(stop uperf-linux before validation)" >&2
    exit 1
fi

restore_file=$(mktemp /tmp/uperf-frequency-restore-XXXXXX)
restore() {
    while IFS='|' read -r path value; do
        [ -n "$path" ] || continue
        printf '%s' "$value" > "$path" || true
    done < "$restore_file"
    rm -f "$restore_file"
}
trap restore EXIT HUP INT TERM

verify_pair() {
    min_path=$1
    max_path=$2
    label=$3
    desired_min=$4
    desired_max=$5
    original_min=$(cat "$min_path")
    original_max=$(cat "$max_path")
    printf '%s|%s\n%s|%s\n' "$max_path" "$original_max" \
        "$min_path" "$original_min" >> "$restore_file"
    # Exercise real driver store handlers with actual OPP endpoints, then let
    # the EXIT trap restore the policy that was active before validation.
    printf '%s' "$desired_max" > "$max_path"
    printf '%s' "$desired_min" > "$min_path"
    actual_min=$(cat "$min_path")
    actual_max=$(cat "$max_path")
    if [ "$actual_min" != "$desired_min" ] ||
       [ "$actual_max" != "$desired_max" ]; then
        echo "write_readback=$label FAIL expected=$desired_min,$desired_max actual=$actual_min,$actual_max"
        exit 1
    fi
    echo "write_readback=$label PASS value=$actual_min,$actual_max"
}

for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$policy" ] || continue
    verify_pair "$policy/scaling_min_freq" "$policy/scaling_max_freq" \
        "$(basename "$policy")" "$(cat "$policy/cpuinfo_min_freq")" \
        "$(cat "$policy/cpuinfo_max_freq")"
done
if [ -r "$gpu/available_frequencies" ]; then
    verify_pair "$gpu/min_freq" "$gpu/max_freq" gpu \
        "$gpu_opp_min" "$gpu_opp_max"
fi
