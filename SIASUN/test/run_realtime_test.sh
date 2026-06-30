#!/usr/bin/env bash

set -u
set -o pipefail

# TEST_BIN：被测 SIASUN 通讯与运动测试程序路径。
TEST_BIN="${TEST_BIN:-../../build/SIASUN/test/siasun_realtime_communication_test}"
# AXIS_DIR：Axis1.xml-Axis6.xml 所在目录。
AXIS_DIR="${AXIS_DIR:-../../doc/SIASUN/gcr10_1300}"
# DURATION_S：单次测试持续时间，单位秒。
DURATION_S="${DURATION_S:-60}"
# CPU_ID：主站进程绑定的逻辑 CPU；-1 表示不绑定。
CPU_ID="${CPU_ID:--1}"
# POLICY：线程调度策略，可选 fifo、rr 或 other。
POLICY="${POLICY:-fifo}"
# PRIORITY：实时优先级；-1 表示当前策略的最高优先级。
PRIORITY="${PRIORITY:--1}"
# MLOCK：是否锁定进程内存，1 表示开启，0 表示关闭。
MLOCK="${MLOCK:-1}"
# DC_SYNC_CYCLES：发送 DC 同步请求的周期倍数，1 表示每周期发送。
DC_SYNC_CYCLES="${DC_SYNC_CYCLES:-1}"
# ENABLE_MOTION：是否执行现有 Servo 6 单关节老化运动。
ENABLE_MOTION="${ENABLE_MOTION:-1}"
# LOAD_PROFILE：压力类型，可选 idle、cpu、memory 或 mixed。
LOAD_PROFILE="${LOAD_PROFILE:-idle}"
# TEST_TAG：本轮测试编号，用于区分同一压力类型下的多组结果。
TEST_TAG="${TEST_TAG:-manual}"
# LOAD_CPUSET：stress-ng 绑定的 CPU 列表；空值表示由系统调度。
LOAD_CPUSET="${LOAD_CPUSET:-}"
# CPU_WORKERS：CPU 压力 worker 数量。
CPU_WORKERS="${CPU_WORKERS:-1}"
# CPU_LOAD：每个 CPU worker 的目标忙碌比例，范围为 0-100。
CPU_LOAD="${CPU_LOAD:-100}"
# VM_WORKERS：内存压力 worker 数量。
VM_WORKERS="${VM_WORKERS:-1}"
# VM_BYTES：每个内存压力 worker 使用的内存大小。
VM_BYTES="${VM_BYTES:-512M}"
# RESULT_DIR：本轮日志和环境快照的输出目录。
RESULT_DIR="${RESULT_DIR:-results/$(date +%Y%m%d-%H%M%S)-${TEST_TAG}-${LOAD_PROFILE}}"
# STRESS_PID：后台 stress-ng 的进程号，空值表示尚未启动。
STRESS_PID=""

# 结束测试时终止并等待后台压力进程。
cleanup() {
    if [[ -n "${STRESS_PID}" ]] && kill -0 "${STRESS_PID}" 2>/dev/null; then
        kill "${STRESS_PID}" 2>/dev/null || true
        wait "${STRESS_PID}" 2>/dev/null || true
    fi
}

# 启动当前 LOAD_PROFILE 对应的 CPU 或内存压力。
start_stress() {
    # stress_command：按压力类型逐项组成 stress-ng 命令。
    local -a stress_command=(stress-ng --metrics-brief)

    case "${LOAD_PROFILE}" in
    idle)
        return
        ;;
    cpu)
        stress_command+=(--cpu "${CPU_WORKERS}" --cpu-load "${CPU_LOAD}")
        ;;
    memory)
        stress_command+=(--vm "${VM_WORKERS}" --vm-bytes "${VM_BYTES}"
            --vm-keep)
        ;;
    mixed)
        stress_command+=(--cpu "${CPU_WORKERS}" --cpu-load "${CPU_LOAD}"
            --vm "${VM_WORKERS}" --vm-bytes "${VM_BYTES}" --vm-keep)
        ;;
    *)
        printf 'invalid LOAD_PROFILE: %s\n' "${LOAD_PROFILE}" >&2
        exit 2
        ;;
    esac

    if ! command -v stress-ng >/dev/null 2>&1; then
        printf 'stress-ng is required for LOAD_PROFILE=%s\n' \
            "${LOAD_PROFILE}" >&2
        exit 2
    fi

    if [[ -n "${LOAD_CPUSET}" ]]; then
        stress_command=(taskset -c "${LOAD_CPUSET}" "${stress_command[@]}")
    fi

    "${stress_command[@]}" >"${RESULT_DIR}/stress.log" 2>&1 &
    STRESS_PID=$!
}

trap cleanup EXIT INT TERM

mkdir -p "${RESULT_DIR}"

# environment_log：内核、CPU、启动参数和中断信息快照文件。
environment_log="${RESULT_DIR}/environment.log"
{
    printf 'date: '
    date --iso-8601=seconds
    uname -a
    printf '\ncmdline: '
    tr '\0' ' ' </proc/cmdline
    printf '\n\ncpu online: '
    cat /sys/devices/system/cpu/online
    printf '\nrealtime kernel flag: '
    cat /sys/kernel/realtime 2>/dev/null || printf 'not exposed'
    printf '\n\nEtherCAT related IRQs:\n'
    grep -Ei 'ethercat|ec_|eno|enp|eth' /proc/interrupts || true
    printf '\nsettings:\n'
    printf 'TEST_TAG=%s\n' "${TEST_TAG}"
    printf 'DURATION_S=%s CPU_ID=%s POLICY=%s PRIORITY=%s MLOCK=%s\n' \
        "${DURATION_S}" "${CPU_ID}" "${POLICY}" "${PRIORITY}" "${MLOCK}"
    printf 'DC_SYNC_CYCLES=%s ENABLE_MOTION=%s LOAD_PROFILE=%s LOAD_CPUSET=%s\n' \
        "${DC_SYNC_CYCLES}" "${ENABLE_MOTION}" "${LOAD_PROFILE}" \
        "${LOAD_CPUSET}"
    printf 'CPU_WORKERS=%s CPU_LOAD=%s VM_WORKERS=%s VM_BYTES=%s\n' \
        "${CPU_WORKERS}" "${CPU_LOAD}" "${VM_WORKERS}" "${VM_BYTES}"
    printf '\nloadavg before: '
    cat /proc/loadavg
    printf '\nmemory before:\n'
    grep -E '^(MemTotal|MemAvailable|SwapTotal|SwapFree):' /proc/meminfo
    printf '\npressure before:\n'
    grep -H . /proc/pressure/cpu /proc/pressure/memory 2>/dev/null || true
} >"${environment_log}"

start_stress

# test_command：传给测试程序的完整位置参数。
test_command=("${TEST_BIN}" "${AXIS_DIR}" "${DURATION_S}" "${CPU_ID}"
    "${POLICY}" "${PRIORITY}" "${MLOCK}" "${DC_SYNC_CYCLES}"
    "${ENABLE_MOTION}")

printf 'running:'
printf ' %q' "${test_command[@]}"
printf '\nresults: %s\n' "${RESULT_DIR}"

# time_command：优先使用 GNU time 保存峰值 RSS 和进程 CPU 百分比。
time_command=()
if [[ -x /usr/bin/time ]]; then
    time_command=(/usr/bin/time -v)
fi

"${time_command[@]}" "${test_command[@]}" \
    > >(tee "${RESULT_DIR}/communication.log") \
    2> >(tee "${RESULT_DIR}/communication.stderr.log" >&2)

# test_status：被测程序退出状态，用于自动化矩阵判定。
test_status=$?
cleanup
trap - EXIT INT TERM

{
    printf '\nloadavg after: '
    cat /proc/loadavg
    printf '\nmemory after:\n'
    grep -E '^(MemTotal|MemAvailable|SwapTotal|SwapFree):' /proc/meminfo
    printf '\npressure after:\n'
    grep -H . /proc/pressure/cpu /proc/pressure/memory 2>/dev/null || true
} >>"${environment_log}"

printf 'test exit status: %d\n' "${test_status}" \
    | tee "${RESULT_DIR}/status.log"
exit "${test_status}"
