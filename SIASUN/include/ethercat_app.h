#ifndef SINSUN_ETHERCAT_APP_H
#define SINSUN_ETHERCAT_APP_H

#include <array>
#include <csignal>
#include <cstdint>
#include <string>

#include "app_config.h"
#include "ecrt.h"
#include "pdo_config.h"

namespace sinsun {

/* SINSUN 主站运行期上下文，集中保存 IgH 句柄、PDO 偏移和状态快照。 */
struct App {
    /* IgH master 句柄，由 ecrt_request_master() 返回。 */
    ec_master_t *master = nullptr;
    /* IgH domain 句柄，用于管理所有从站的过程数据。 */
    ec_domain_t *domain = nullptr;
    /* 6 个伺服从站的配置句柄，数组下标 0-5 对应逻辑 id 1-6。 */
    std::array<ec_slave_config_t *, kServoCount> servo_configs{};
    /* 末端 IO 从站配置句柄，逻辑 id 为 7。 */
    ec_slave_config_t *endio_config = nullptr;
    /* 激活 master 后取得的 domain process data 起始地址。 */
    uint8_t *domain_pd = nullptr;
    /* 6 个伺服 RxPDO 输出 entry 偏移；第 6 轴用于使能和小范围运动。 */
    std::array<ServoOutputOffsets, kServoCount> servo_output_offsets{};
    /* 末端 IO RxPDO 输出 entry 偏移。 */
    EndIoOutputOffsets endio_output_offsets{};
    /* 6 个伺服 TxPDO 输入 entry 偏移，用于读取和打印。 */
    std::array<ServoOffsets, kServoCount> servo_offsets{};
    /* 末端 IO TxPDO 输入 entry 偏移，用于读取和打印。 */
    EndIoOffsets endio_offsets{};
    /* 上一次打印过的 master 状态，用于只在状态变化时打印。 */
    ec_master_state_t master_state{};
    /* 上一次打印过的 domain 状态，用于观察 working counter 变化。 */
    ec_domain_state_t domain_state{};
    /* 上一次打印过的伺服状态，数组下标 0-5 对应逻辑 id 1-6。 */
    std::array<ec_slave_config_state_t, kServoCount> servo_states{};
    /* 上一次打印过的末端 IO 状态。 */
    ec_slave_config_state_t endio_state{};
};

/* Linux 实时线程调度策略，供正式程序和测试 demo 共用。 */
enum class SchedulingPolicy {
    /* 普通分时调度策略 SCHED_OTHER。 */
    Other,
    /* 固定优先级实时调度策略 SCHED_FIFO。 */
    Fifo,
    /* 时间片轮转实时调度策略 SCHED_RR。 */
    RoundRobin,
};

/* 实时进程设置；测试 demo 可逐项改变这些变量。 */
struct RealtimeConfig {
    /* policy：目标调度策略，默认使用 SCHED_FIFO。 */
    SchedulingPolicy policy = SchedulingPolicy::Fifo;
    /* priority：目标优先级；-1 表示使用该策略允许的最高优先级。 */
    int priority = -1;
    /* cpu_id：进程绑定的逻辑 CPU；-1 表示不修改 CPU 亲和性。 */
    int cpu_id = -1;
    /* lock_memory：是否使用 mlockall() 锁定当前和后续内存页。 */
    bool lock_memory = true;
};

/* 实时设置结果，用于日志和不同系统环境之间的结果核对。 */
struct RealtimeSetupResult {
    /* scheduler_configured：调度策略与优先级是否设置成功。 */
    bool scheduler_configured = false;
    /* affinity_configured：未要求绑定 CPU，或 CPU 亲和性设置成功。 */
    bool affinity_configured = false;
    /* memory_locked：未要求锁内存，或 mlockall() 设置成功。 */
    bool memory_locked = false;
    /* actual_policy：设置完成后由操作系统返回的实际调度策略。 */
    int actual_policy = 0;
    /* actual_priority：设置完成后由操作系统返回的实际线程优先级。 */
    int actual_priority = 0;
    /* actual_cpu：读取结果时当前线程正在运行的逻辑 CPU。 */
    int actual_cpu = -1;
};

/* 周期通讯设置；默认值保持正式程序当前行为。 */
struct RunConfig {
    /* enable_motion：是否执行现有 Servo 6 使能和运动逻辑。 */
    bool enable_motion = true;
    /*
     * require_endio_for_quality：通信质量统计是否要求 EndIO 同时进入 OP。
     * true 保持7站完整判断；false 只把6个伺服作为必需通讯对象。
     */
    bool require_endio_for_quality = true;
    /* max_cycles：达到该周期数后退出；0 表示只由信号控制退出。 */
    uint64_t max_cycles = 0;
    /* dc_sync_period_cycles：DC 同步间隔；0 表示不主动发送同步请求。 */
    uint64_t dc_sync_period_cycles = kDcSyncPeriodCycles;
    /* report_period_cycles：通信质量报告间隔；0 表示只打印最终报告。 */
    uint64_t report_period_cycles = 10000;
};

/* 先写入 Axis*.xml 中的 SDO 参数，再配置 PDO/DC 并激活 master。 */
int configure(App &app, const std::string &axis_config_directory);

/* 按配置设置实时调度、CPU 亲和性和内存锁定，失败时打印 warning。 */
RealtimeSetupResult setup_realtime_process(
    const RealtimeConfig &config = RealtimeConfig{});

/* 执行 1 ms 周期通讯循环，直到收到退出信号或达到最大周期数。 */
void run(App &app,
         volatile std::sig_atomic_t &keep_running,
         const RunConfig &config = RunConfig{});

/* 释放 IgH master 资源。 */
void release(App &app);

}  // namespace sinsun

#endif
