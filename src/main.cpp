#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#include "device/gsd620/gsd620_device.h"
#include "orchestrator/robot_ethercat_orchestrator.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;  // 仅由信号处理函数写入的停止标志。

/**
 * @brief 设置当前 Linux 线程名称。
 *
 * 线程名称用于 top -H、ps -L 和调试器中区分主线程与 EtherCAT 周期线程。
 * 设置失败只打印告警，不影响程序继续运行。
 *
 * @param name 不超过 15 个可见字符的线程名称。
 */
void SetCurrentThreadName(const char* name) {
    const int result = pthread_setname_np(pthread_self(), name);
    if (result != 0) {
        std::fprintf(stderr, "warning: failed to set thread name '%s': %d\n", name, result);
    }
}

/**
 * @brief 接收进程退出信号并请求主线程停止周期线程。
 *
 * @param signal 收到的 POSIX 信号编号。
 */
void HandleStopSignal(int signal) {
    // 步骤 1：信号处理函数只修改 signal-safe 标志，实际停机由主线程执行。
    (void)signal;
    g_stop_requested = 1;
}

/**
 * @brief 为当前线程设置尽力而为的 SCHED_FIFO 调度策略。
 *
 * 缺少 CAP_SYS_NICE 或实时 limits 时只打印告警，周期线程仍会继续运行，
 * 以便开发阶段验证 EtherCAT 配置。
 */
void ConfigureCurrentThreadRealtime() {
    // 步骤 1：取得 SCHED_FIFO 允许的最高优先级。
    sched_param parameter{};
    parameter.sched_priority = sched_get_priority_max(SCHED_FIFO);

    // 步骤 2：尝试为当前周期线程应用实时调度策略。
    const int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameter);
    if (result != 0) {
        std::fprintf(stderr, "warning: pthread_setschedparam(SCHED_FIFO) failed: %d\n", result);
    }
}

/**
 * @brief 将纳秒周期累加到绝对唤醒时间。
 *
 * @param wakeup_time 需要原地更新的 CLOCK_MONOTONIC 绝对时间。
 * @param duration_ns 需要累加的时间长度，单位为纳秒。
 */
void AddNanoseconds(timespec& wakeup_time, uint32_t duration_ns) {
    constexpr long kNanosecondsPerSecond = 1'000'000'000L;

    // 步骤 1：先把一个周期累加到纳秒字段。
    wakeup_time.tv_nsec += static_cast<long>(duration_ns);

    // 步骤 2：将溢出的纳秒归一化到秒字段。
    while (wakeup_time.tv_nsec >= kNanosecondsPerSecond) {
        wakeup_time.tv_nsec -= kNanosecondsPerSecond;
        ++wakeup_time.tv_sec;
    }
}

/**
 * @brief 将 timespec 转换为纳秒时间戳。
 *
 * @param time 要转换的时间。
 * @return 自 CLOCK_MONOTONIC 起的纳秒数。
 */
uint64_t ToNanoseconds(const timespec& time) {
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

    // 步骤 1：统一转换为纳秒，作为 IgH application time。
    return static_cast<uint64_t>(time.tv_sec) * kNanosecondsPerSecond +
           static_cast<uint64_t>(time.tv_nsec);
}

/**
 * @brief 运行唯一的 EtherCAT 实时周期线程。
 *
 * 线程使用绝对时间睡眠，避免 sleep 相对误差在长期运行中累积。业务编排
 * 由 RobotEthercatOrchestrator::RunCycle() 完成。
 *
 * @param application 已成功 Initialize() 的 EtherCAT 编排层。
 * @param cycle_time_ns EtherCAT 标称周期，单位为纳秒。
 * @param keep_running 主线程和周期线程共享的运行开关。
 * @param cycle_failed 周期调用或等待失败时置为 true。
 */
void RunCycleThread(orchestrator::RobotEthercatOrchestrator& application,
                    uint32_t cycle_time_ns,
                    std::atomic_bool& keep_running,
                    std::atomic_bool& cycle_failed) {
    // 步骤 1：命名周期线程，便于与只负责生命周期管理的主线程区分。
    SetCurrentThreadName("ethercat-cycle");

    // 步骤 2：为当前线程配置尽力而为的实时调度属性。
    ConfigureCurrentThreadRealtime();

    // 步骤 3：读取单调时钟，并计算第一个绝对周期唤醒点。
    timespec wakeup_time{};
    if (clock_gettime(CLOCK_MONOTONIC, &wakeup_time) != 0) {
        std::perror("clock_gettime(CLOCK_MONOTONIC) failed");
        cycle_failed.store(true);
        keep_running.store(false);
        return;
    }

    AddNanoseconds(wakeup_time, cycle_time_ns);
    while (keep_running.load()) {
        // 步骤 4：按绝对时间等待，避免相对 sleep 误差逐周期累积。
        const int sleep_result =
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, nullptr);
        if (sleep_result != 0 && sleep_result != EINTR) {
            std::fprintf(stderr, "clock_nanosleep failed: %d\n", sleep_result);
            cycle_failed.store(true);
            keep_running.store(false);
            return;
        }
        if (sleep_result == EINTR) {
            continue;
        }

        // 步骤 5：以本周期计划唤醒时间执行一次完整 EtherCAT 收发。
        if (application.RunCycle(ToNanoseconds(wakeup_time)) !=
            orchestrator::OrchestratorResult::kSuccess) {
            cycle_failed.store(true);
            keep_running.store(false);
            return;
        }

        // 步骤 6：推进到下一周期的绝对唤醒点。
        AddNanoseconds(wakeup_time, cycle_time_ns);
    }
}

/**
 * @brief 尽力锁定当前进程内存，降低周期线程缺页风险。
 */
void LockProcessMemory() {
    // 步骤 1：锁定当前和未来映射的内存，降低实时周期中的缺页概率。
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::perror("warning: mlockall failed");
    }
}

}  // namespace

/**
 * @brief 启动单 GSD620 EtherCAT 主站周期程序。
 *
 * @return 0 用户正常停止。
 * @return 1 主站初始化或实时周期失败。
 */
int main() {
    // 步骤 1：命名主线程，便于与 EtherCAT 实时周期线程区分。
    SetCurrentThreadName("robot-main");

    // 步骤 2：注册安全停机信号，信号处理函数只负责提出停止请求。
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);

    // 步骤 3：组装主站参数与具体设备定义；GSD620 同时作为 DC 参考设备。
    orchestrator::RobotEthercatConfiguration configuration{};
    device::Gsd620Configuration gsd620_configuration{};
    device::DeviceSetup device_setup;
    device_setup.AddRobotAxisReferenceClockDevice<device::Gsd620Device>(0, gsd620_configuration);

    orchestrator::RobotEthercatOrchestrator application(configuration,
                                                        std::move(device_setup).Build());

    // 步骤 4：统一注册从站、配置 PDO/DC，并激活 IgH master。
    if (application.Initialize() != orchestrator::OrchestratorResult::kSuccess) {
        std::fprintf(stderr, "failed to initialize EtherCAT application\n");
        return 1;
    }

    // 步骤 5：在启动周期线程前锁定进程内存。
    LockProcessMemory();

    // 步骤 6：创建唯一 EtherCAT 周期线程。
    std::atomic_bool keep_running{true};   // 主线程和周期线程共享的运行状态。
    std::atomic_bool cycle_failed{false};  // 周期线程检测到不可恢复错误时置位。
    std::thread cycle_thread(RunCycleThread,
                             std::ref(application),
                             configuration.cycle_time_ns,
                             std::ref(keep_running),
                             std::ref(cycle_failed));

    // 步骤 7：主线程只监控停止请求，不参与 EtherCAT 周期收发。
    while (keep_running.load()) {
        if (g_stop_requested != 0) {
            keep_running.store(false);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // 步骤 8：等待周期线程退出后释放主站和从站资源。
    cycle_thread.join();
    application.Shutdown();

    // 步骤 9：区分周期故障退出与用户正常停止。
    if (cycle_failed.load()) {
        std::fprintf(stderr, "EtherCAT cycle stopped because of an error\n");
        return 1;
    }

    std::puts("EtherCAT application stopped");
    return 0;
}
