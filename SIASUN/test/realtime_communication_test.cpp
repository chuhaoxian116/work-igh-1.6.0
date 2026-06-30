#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/resource.h>

#include "app_config.h"
#include "axis_config.h"
#include "ethercat_app.h"

namespace {

/* 运行标志；收到退出信号或达到指定测试周期后置 0。 */
volatile std::sig_atomic_t keep_running = 1;

/*
 * 处理测试退出信号。
 *
 * signal_number：收到的 POSIX 信号编号，本测试不需要区分具体信号。
 */
void signal_handler(int signal_number) {
    /* ignored_signal_number：显式消费参数，避免编译器产生未使用告警。 */
    const int ignored_signal_number = signal_number;
    (void)ignored_signal_number;
    keep_running = 0;
}

/* 安装 Ctrl+C 和进程终止信号，保证主站资源可以正常释放。 */
void install_signal_handlers() {
    /* action：SIGINT/SIGTERM 共用的 sigaction 配置。 */
    struct sigaction action {};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

/*
 * 解析十进制有符号整数参数。
 *
 * text：命令行中的整数文本。
 * argument_name：参数名称，仅用于错误日志。
 * output：解析成功后保存整数值。
 */
bool parse_int(const char *text, const char *argument_name, int &output) {
    /* end：strtol() 返回后指向第一个未解析字符。 */
    char *end = nullptr;

    errno = 0;

    /* value：strtol() 得到的长整数，检查范围后再转换成 int。 */
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < INT_MIN || value > INT_MAX) {
        std::fprintf(stderr, "invalid %s: %s\n", argument_name, text);
        return false;
    }

    output = static_cast<int>(value);
    return true;
}

/*
 * 解析十进制无符号整数参数。
 *
 * text：命令行中的非负整数文本。
 * argument_name：参数名称，仅用于错误日志。
 * output：解析成功后保存 64 位无符号整数值。
 */
bool parse_uint64(const char *text,
                  const char *argument_name,
                  uint64_t &output) {
    /* end：strtoull() 返回后指向第一个未解析字符。 */
    char *end = nullptr;

    errno = 0;

    /* value：strtoull() 得到的无符号整数。 */
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-') {
        std::fprintf(stderr, "invalid %s: %s\n", argument_name, text);
        return false;
    }

    output = static_cast<uint64_t>(value);
    return true;
}

/*
 * 解析调度策略名称。
 *
 * text：支持 other、fifo 和 rr。
 * policy：解析成功后保存公共接口使用的调度策略枚举。
 */
bool parse_policy(const char *text, sinsun::SchedulingPolicy &policy) {
    if (std::strcmp(text, "other") == 0) {
        policy = sinsun::SchedulingPolicy::Other;
        return true;
    }
    if (std::strcmp(text, "fifo") == 0) {
        policy = sinsun::SchedulingPolicy::Fifo;
        return true;
    }
    if (std::strcmp(text, "rr") == 0) {
        policy = sinsun::SchedulingPolicy::RoundRobin;
        return true;
    }

    std::fprintf(stderr, "invalid policy: %s (expected other|fifo|rr)\n", text);
    return false;
}

/*
 * 将 timeval 转换成秒。
 *
 * value：getrusage() 返回的用户态或内核态 CPU 时间。
 */
double timeval_to_seconds(const timeval &value) {
    return static_cast<double>(value.tv_sec) +
           static_cast<double>(value.tv_usec) / 1000000.0;
}

/*
 * 打印本进程 CPU、内存、缺页和上下文切换统计。
 *
 * before：进入 PDO 通讯循环前的进程资源统计快照。
 * after：离开 PDO 通讯循环后的进程资源统计快照。
 * elapsed_seconds：PDO 通讯循环实际经过的墙钟时间。
 */
void print_resource_usage(const rusage &before,
                          const rusage &after,
                          double elapsed_seconds) {
    /* user_cpu_seconds：PDO 通讯窗口消耗的用户态 CPU 时间。 */
    const double user_cpu_seconds =
        timeval_to_seconds(after.ru_utime) -
        timeval_to_seconds(before.ru_utime);

    /* system_cpu_seconds：PDO 通讯窗口消耗的内核态 CPU 时间。 */
    const double system_cpu_seconds =
        timeval_to_seconds(after.ru_stime) -
        timeval_to_seconds(before.ru_stime);

    /* cpu_percent：单核口径下的通讯窗口平均 CPU 占用率。 */
    const double cpu_percent = elapsed_seconds > 0.0 ?
        (user_cpu_seconds + system_cpu_seconds) * 100.0 / elapsed_seconds :
        0.0;

    std::printf("\n[RESOURCE] elapsed_s=%.6f cpu_percent=%.3f\n",
                elapsed_seconds,
                cpu_percent);
    std::printf("[RESOURCE] user_cpu_s=%.6f system_cpu_s=%.6f\n",
                user_cpu_seconds,
                system_cpu_seconds);
    std::printf("[RESOURCE] max_rss_kb=%ld minor_faults=%ld major_faults=%ld\n",
                after.ru_maxrss,
                after.ru_minflt - before.ru_minflt,
                after.ru_majflt - before.ru_majflt);
    std::printf("[RESOURCE] voluntary_cs=%ld involuntary_cs=%ld\n",
                after.ru_nvcsw - before.ru_nvcsw,
                after.ru_nivcsw - before.ru_nivcsw);
}

/* 打印测试 demo 的位置参数说明。 */
void print_usage(const char *program_name) {
    std::fprintf(
        stderr,
        "usage: %s AxisXmlDirectory [duration_s] [cpu_id] [policy] "
        "[priority] [mlock] [dc_sync_cycles] [enable_motion] "
        "[require_endio_op]\n"
        "defaults: duration_s=60 cpu_id=-1 policy=fifo priority=-1(max) "
        "mlock=1 dc_sync_cycles=1 enable_motion=1 require_endio_op=1\n",
        program_name);
}

}  // namespace

/* SIASUN 实时通讯、单关节老化运动与资源占用测试入口。 */
int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 10) {
        print_usage(argv[0]);
        return 1;
    }

    /* requested_directory：命令行传入的 Axis*.xml 目录。 */
    const std::string requested_directory = argv[1];

    /* duration_seconds：测试持续时间，默认 60 秒。 */
    uint64_t duration_seconds = 60;

    /* realtime_config：实时策略、优先级、亲和性和内存锁定配置。 */
    sinsun::RealtimeConfig realtime_config {};

    /* run_config：运动开关、结束周期和 DC 同步间隔配置。 */
    sinsun::RunConfig run_config {};
    run_config.report_period_cycles = 0;

    /* lock_memory：命令行中的 0/1 内存锁定开关。 */
    int lock_memory = 1;

    /* enable_motion：命令行中的 0/1 单关节老化运动开关。 */
    int enable_motion = 1;

    /* require_endio_op：命令行中的 0/1 EndIO 通信质量门槛开关。 */
    int require_endio_op = 1;

    if ((argc > 2 &&
         !parse_uint64(argv[2], "duration_s", duration_seconds)) ||
        (argc > 3 &&
         !parse_int(argv[3], "cpu_id", realtime_config.cpu_id)) ||
        (argc > 4 &&
         !parse_policy(argv[4], realtime_config.policy)) ||
        (argc > 5 &&
         !parse_int(argv[5], "priority", realtime_config.priority)) ||
        (argc > 6 && !parse_int(argv[6], "mlock", lock_memory)) ||
        (argc > 7 &&
         !parse_uint64(argv[7],
                       "dc_sync_cycles",
                       run_config.dc_sync_period_cycles)) ||
        (argc > 8 &&
         !parse_int(argv[8], "enable_motion", enable_motion)) ||
        (argc > 9 &&
         !parse_int(argv[9], "require_endio_op", require_endio_op))) {
        print_usage(argv[0]);
        return 1;
    }

    if (duration_seconds == 0 ||
        duration_seconds > UINT64_MAX / 1000000000ULL) {
        std::fprintf(stderr, "duration_s must be greater than zero\n");
        return 1;
    }
    if (lock_memory != 0 && lock_memory != 1) {
        std::fprintf(stderr, "mlock must be 0 or 1\n");
        return 1;
    }
    if (enable_motion != 0 && enable_motion != 1) {
        std::fprintf(stderr, "enable_motion must be 0 or 1\n");
        return 1;
    }
    if (require_endio_op != 0 && require_endio_op != 1) {
        std::fprintf(stderr, "require_endio_op must be 0 or 1\n");
        return 1;
    }

    realtime_config.lock_memory = lock_memory == 1;
    run_config.enable_motion = enable_motion == 1;
    run_config.require_endio_for_quality = require_endio_op == 1;

    /* duration_nanoseconds：测试时长换算成纳秒。 */
    const uint64_t duration_nanoseconds =
        duration_seconds * 1000000000ULL;
    run_config.max_cycles = duration_nanoseconds / sinsun::kCycleTimeNs;

    /* axis_config_directory：校验后实际使用的 Axis*.xml 目录。 */
    std::string axis_config_directory;
    if (sinsun::resolve_axis_config_directory(requested_directory,
                                              axis_config_directory)) {
        std::fprintf(stderr, "invalid Axis*.xml directory: %s\n", argv[1]);
        return 1;
    }

    std::printf("[TEST] duration=%llu s cycles=%llu axis_dir=%s\n",
                static_cast<unsigned long long>(duration_seconds),
                static_cast<unsigned long long>(run_config.max_cycles),
                axis_config_directory.c_str());
    std::printf("[TEST] Servo 6 aging motion: %s\n",
                run_config.enable_motion ? "enabled" : "disabled");
    std::printf("[TEST] communication gate: %s\n",
                run_config.require_endio_for_quality
                    ? "Servo 1-6 + EndIO 7"
                    : "Servo 1-6 only; EndIO 7 is optional");

    install_signal_handlers();

    /* app：SIASUN EtherCAT 主站运行期上下文。 */
    sinsun::App app;
    if (sinsun::configure(app, axis_config_directory)) {
        sinsun::release(app);
        return 1;
    }

    /* realtime_result：用于确认请求的实时设置是否真正生效。 */
    const sinsun::RealtimeSetupResult realtime_result =
        sinsun::setup_realtime_process(realtime_config);

    /* realtime_ready：三个请求项是否都按预期设置成功。 */
    const bool realtime_ready =
        realtime_result.scheduler_configured &&
        realtime_result.affinity_configured &&
        realtime_result.memory_locked;
    if (!realtime_ready) {
        std::fprintf(stderr,
                     "[TEST] warning: one or more realtime settings failed; "
                     "keep this run as a non-realtime comparison sample\n");
    }

    /* usage_before：进入 PDO 通讯循环前的进程资源统计快照。 */
    rusage usage_before {};
    getrusage(RUSAGE_SELF, &usage_before);

    /* time_before：进入 PDO 通讯循环前的单调时钟时间。 */
    timespec time_before {};
    clock_gettime(CLOCK_MONOTONIC, &time_before);

    sinsun::run(app, keep_running, run_config);

    /* time_after：离开 PDO 通讯循环后的单调时钟时间。 */
    timespec time_after {};
    clock_gettime(CLOCK_MONOTONIC, &time_after);

    /* usage_after：离开 PDO 通讯循环后的进程资源统计快照。 */
    rusage usage_after {};
    const int usage_status = getrusage(RUSAGE_SELF, &usage_after);

    sinsun::release(app);

    /* elapsed_seconds：PDO 通讯循环经过的单调时钟时间，单位秒。 */
    const double elapsed_seconds =
        static_cast<double>(time_after.tv_sec - time_before.tv_sec) +
        static_cast<double>(time_after.tv_nsec - time_before.tv_nsec) /
            1000000000.0;

    if (usage_status == 0) {
        print_resource_usage(usage_before, usage_after, elapsed_seconds);
    } else {
        std::fprintf(stderr, "warning: getrusage failed: %s\n",
                     std::strerror(errno));
    }

    return 0;
}
