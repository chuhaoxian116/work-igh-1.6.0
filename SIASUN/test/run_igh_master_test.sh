#!/usr/bin/env bash

set -u
set -o pipefail

# TEST_TAG：本轮 IgH 内核线程测试编号。
TEST_TAG="${TEST_TAG:-O00}"
# TEST_BIN：被测 SIASUN 通讯与运动测试程序路径。
TEST_BIN="${TEST_BIN:-../../build/SIASUN/test/siasun_realtime_communication_test}"
# AXIS_DIR：Axis1.xml-Axis6.xml 所在目录。
AXIS_DIR="${AXIS_DIR:-../../doc/SIASUN/gcr10_1300}"
# DURATION_S：单次测试持续时间，单位秒。
DURATION_S="${DURATION_S:-600}"
# APP_CPU：用户态 1 ms 通讯线程绑定的逻辑 CPU。
APP_CPU="${APP_CPU:-2}"
# OP_CPU：EtherCAT-OP 内核线程绑定的逻辑 CPU；-1 表示不绑定。
OP_CPU="${OP_CPU:--1}"
# OP_POLICY：EtherCAT-OP 调度策略，可选 other、fifo 或 rr。
OP_POLICY="${OP_POLICY:-other}"
# OP_PRIORITY：EtherCAT-OP 实时优先级；other 策略必须为 0。
OP_PRIORITY="${OP_PRIORITY:-0}"
# HOUSEKEEPING_CPU：测试脚本和监控命令使用的逻辑 CPU。
HOUSEKEEPING_CPU="${HOUSEKEEPING_CPU:-0}"
# MONITOR_INTERVAL_S：采集 EtherCAT-OP 状态的时间间隔。
MONITOR_INTERVAL_S="${MONITOR_INTERVAL_S:-1}"
# OP_WAIT_SECONDS：等待 EtherCAT-OP 创建完成的最长时间。
OP_WAIT_SECONDS="${OP_WAIT_SECONDS:-120}"
# DC_SYNC_CYCLES：应用发送 DC 同步请求的周期倍数。
DC_SYNC_CYCLES="${DC_SYNC_CYCLES:-1}"
# ENABLE_MOTION：是否执行现有 Servo 6 单关节老化运动。
ENABLE_MOTION="${ENABLE_MOTION:-1}"
# LOAD_PROFILE：压力类型，可选 idle、cpu、memory 或 mixed。
LOAD_PROFILE="${LOAD_PROFILE:-idle}"
# LOAD_CPUSET：stress-ng 使用的逻辑 CPU 列表。
LOAD_CPUSET="${LOAD_CPUSET:-}"
# CPU_WORKERS：CPU 压力 worker 数量。
CPU_WORKERS="${CPU_WORKERS:-1}"
# CPU_LOAD：每个 CPU worker 的目标忙碌比例。
CPU_LOAD="${CPU_LOAD:-100}"
# VM_WORKERS：内存压力 worker 数量。
VM_WORKERS="${VM_WORKERS:-1}"
# VM_BYTES：每个内存压力 worker 使用的内存大小。
VM_BYTES="${VM_BYTES:-512M}"
# RESULT_DIR：IgH 内核线程状态和通讯质量日志目录。
RESULT_DIR="${RESULT_DIR:-results/$(date +%Y%m%d-%H%M%S)-${TEST_TAG}-igh-master}"
# RUNNER_PID：后台通讯测试脚本的进程号。
RUNNER_PID=""
# OP_PID：Master 激活后创建的 EtherCAT-OP 内核线程号。
OP_PID=""

# 中断或退出时停止整组后台测试进程。
cleanup() {
    if [[ -n "${RUNNER_PID}" ]] && kill -0 "${RUNNER_PID}" 2>/dev/null; then
        kill -INT -- "-${RUNNER_PID}" 2>/dev/null || true
        wait "${RUNNER_PID}" 2>/dev/null || true
    fi
}

# 等待 Master 激活后新创建的 EtherCAT-OP 内核线程。
wait_for_ethercat_op() {
    # attempt：按 100 ms 间隔等待线程出现的循环计数。
    local attempt
    # maximum_attempts：由等待秒数换算出的最大循环次数。
    local maximum_attempts=$((OP_WAIT_SECONDS * 10))

    for ((attempt = 0; attempt < maximum_attempts; ++attempt)); do
        OP_PID="$(pgrep -n -x EtherCAT-OP 2>/dev/null || true)"
        if [[ -n "${OP_PID}" ]] && [[ -r "/proc/${OP_PID}/status" ]]; then
            return 0
        fi
        sleep 0.1
    done

    return 1
}

# 设置 EtherCAT-OP 的 CPU 亲和性和调度策略。
configure_ethercat_op() {
    if [[ "${OP_CPU}" -ge 0 ]]; then
        taskset -pc "${OP_CPU}" "${OP_PID}"
    fi

    case "${OP_POLICY}" in
    other)
        chrt -o -p 0 "${OP_PID}"
        ;;
    fifo)
        chrt -f -p "${OP_PRIORITY}" "${OP_PID}"
        ;;
    rr)
        chrt -r -p "${OP_PRIORITY}" "${OP_PID}"
        ;;
    *)
        printf 'invalid OP_POLICY: %s\n' "${OP_POLICY}" >&2
        return 1
        ;;
    esac
}

# 保存 EtherCAT-OP 当前调度、亲和性和内核调度统计。
capture_ethercat_op_snapshot() {
    # snapshot_name：快照名称，用于区分设置完成和测试结束状态。
    local snapshot_name="$1"
    # snapshot_path：当前快照的输出文件。
    local snapshot_path="${RESULT_DIR}/ethercat-op-${snapshot_name}.log"

    {
        date --iso-8601=seconds
        ps -p "${OP_PID}" \
            -o pid,tid,cls,rtprio,pri,ni,psr,pcpu,stat,comm
        chrt -p "${OP_PID}"
        taskset -pc "${OP_PID}"
        printf '\n/proc/%s/sched:\n' "${OP_PID}"
        cat "/proc/${OP_PID}/sched"
        printf '\n/proc/%s/schedstat:\n' "${OP_PID}"
        cat "/proc/${OP_PID}/schedstat"
    } >"${snapshot_path}" 2>&1
}

trap cleanup EXIT INT TERM

mkdir -p "${RESULT_DIR}"

if [[ "${HOUSEKEEPING_CPU}" -ge 0 ]]; then
    taskset -pc "${HOUSEKEEPING_CPU}" "$$" >/dev/null
fi

printf 'IgH master test %s\n' "${TEST_TAG}"
printf 'APP: CPU=%s FIFO=99; EtherCAT-OP: CPU=%s policy=%s priority=%s\n' \
    "${APP_CPU}" "${OP_CPU}" "${OP_POLICY}" "${OP_PRIORITY}"
printf 'results: %s\n' "${RESULT_DIR}"

# runner_command：复用应用测试脚本启动通讯、运动和可选系统压力。
runner_command=(
    env
    TEST_TAG="${TEST_TAG}"
    TEST_BIN="${TEST_BIN}"
    AXIS_DIR="${AXIS_DIR}"
    DURATION_S="${DURATION_S}"
    CPU_ID="${APP_CPU}"
    POLICY=fifo
    PRIORITY=-1
    MLOCK=1
    DC_SYNC_CYCLES="${DC_SYNC_CYCLES}"
    ENABLE_MOTION="${ENABLE_MOTION}"
    LOAD_PROFILE="${LOAD_PROFILE}"
    LOAD_CPUSET="${LOAD_CPUSET}"
    CPU_WORKERS="${CPU_WORKERS}"
    CPU_LOAD="${CPU_LOAD}"
    VM_WORKERS="${VM_WORKERS}"
    VM_BYTES="${VM_BYTES}"
    RESULT_DIR="${RESULT_DIR}"
    ./run_realtime_test.sh
)

setsid "${runner_command[@]}" &
RUNNER_PID=$!

if ! wait_for_ethercat_op; then
    printf 'failed to find EtherCAT-OP within %s seconds\n' \
        "${OP_WAIT_SECONDS}" >&2
    exit 2
fi

printf 'found EtherCAT-OP pid=%s\n' "${OP_PID}"
if ! configure_ethercat_op; then
    printf 'failed to configure EtherCAT-OP pid=%s\n' "${OP_PID}" >&2
    exit 2
fi

capture_ethercat_op_snapshot "configured"

# monitor_log：每秒记录 EtherCAT-OP 所在 CPU、调度类别和 CPU 占用。
monitor_log="${RESULT_DIR}/ethercat-op-monitor.log"
printf 'timestamp etimes pid tid cls rtprio pri psr pcpu stat comm schedstat\n' \
    >"${monitor_log}"

while kill -0 "${RUNNER_PID}" 2>/dev/null &&
      kill -0 "${OP_PID}" 2>/dev/null; do
    # timestamp：当前监控样本的 ISO 8601 时间。
    timestamp="$(date --iso-8601=seconds)"
    # process_sample：ps 返回的 EtherCAT-OP 当前调度状态。
    process_sample="$(ps -p "${OP_PID}" \
        -o etimes=,pid=,tid=,cls=,rtprio=,pri=,psr=,pcpu=,stat=,comm=)"
    # schedstat_sample：运行时间、等待时间和调度次数的内核累计值。
    schedstat_sample="$(cat "/proc/${OP_PID}/schedstat" 2>/dev/null || true)"
    printf '%s %s %s\n' \
        "${timestamp}" "${process_sample}" "${schedstat_sample}" \
        >>"${monitor_log}"
    sleep "${MONITOR_INTERVAL_S}"
done

if kill -0 "${OP_PID}" 2>/dev/null; then
    capture_ethercat_op_snapshot "final"
fi

# runner_status：通讯测试脚本的最终退出状态。
wait "${RUNNER_PID}"
runner_status=$?
RUNNER_PID=""
trap - EXIT INT TERM

printf 'IgH master test exit status: %d\n' "${runner_status}" \
    | tee "${RESULT_DIR}/igh-master-status.log"
exit "${runner_status}"
