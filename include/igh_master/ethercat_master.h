#ifndef IGH_MASTER_ETHERCAT_MASTER_H
#define IGH_MASTER_ETHERCAT_MASTER_H

/*
 * IgH 主站应用层封装。
 *
 * 其它模块通过这里的 API 使用主站能力，尽量不直接调用 ecrt_*。
 * 这样可以把主站初始化、DC 同步、周期性 process data 处理集中管理。
 */

#include <signal.h>
#include <stdint.h>
#include <pthread.h>

#include "ecrt.h"
#include "igh_master/drive_pdo.h"

/*
 * 单主站、单 domain、单 CiA 402 伺服的运行状态。
 *
 * master/domain/drive 的状态快照会被缓存下来，只在状态变化时打印，
 * 避免 1 ms 循环持续刷屏。
 */
typedef struct {
    uint64_t cycle;
    drive_inputs_t inputs;
    drive_outputs_t outputs;
    uint32_t dc_sync_diff_ns;
    int dc_sync_diff_valid;
    int32_t sine_base_position;
    int control_state;
} ethercat_debug_snapshot_t;

/*
 * 通信质量累计统计。
 *
 * 周期线程是唯一写入者，分钟报告和退出报告也由周期线程打印，因此不需要
 * 在 1 ms 热路径中增加互斥锁。domain_* 计数使用 IgH working-counter 状态，
 * 用于区分完整、部分和完全无 process data 的周期。
 */
typedef struct {
    uint64_t cycles;
    uint64_t interval_samples;
    uint64_t cycle_time_sum_ns;
    uint64_t jitter_abs_sum_ns;
    uint64_t severe_overruns;

    uint64_t domain_complete_cycles;
    uint64_t domain_incomplete_cycles;
    uint64_t domain_zero_cycles;
    uint32_t min_working_counter;
    uint32_t max_working_counter;

    uint64_t dc_valid_samples;
    uint64_t dc_invalid_samples;
    uint64_t dc_abs_sum_ns;
    uint32_t max_dc_diff_ns;

    int64_t start_time_ns;
    int64_t last_cycle_time_ns;
    int64_t min_cycle_ns;
    int64_t max_cycle_ns;
    int64_t max_abs_jitter_ns;
} ethercat_quality_stats_t;

typedef struct {
    ec_master_t *master;
    ec_domain_t *domain;
    ec_slave_config_t *drive_config;
    uint8_t *domain_pd;

    ec_master_state_t master_state;
    ec_domain_state_t domain_state;
    ec_slave_config_state_t drive_state;

    drive_pdo_offsets_t pdo_offsets;
    drive_outputs_t outputs;

    pthread_mutex_t debug_lock;
    pthread_t debug_thread;
    volatile sig_atomic_t *debug_keep_running;
    int debug_lock_ready;
    int debug_thread_running;

    ethercat_debug_snapshot_t debug_snapshot;
    ethercat_quality_stats_t quality;

    int control_state;
    uint64_t control_state_cycles;
    uint64_t motion_cycles;
    int32_t sine_base_position;
} ethercat_master_app_t;

/* 清零运行期指针、状态、offset 和输出值。 */
void ethercat_master_app_init(ethercat_master_app_t *app);

/*
 * 请求 IgH master，配置伺服 PDO/DC，激活 master，并获取 domain
 * process data 指针。
 */
int ethercat_master_app_configure(ethercat_master_app_t *app);

/* 启动调试线程，慢速打印当前 PDO、控制状态和 DC 同步监视信息。 */
int ethercat_master_app_start_debug_thread(
    ethercat_master_app_t *app,
    volatile sig_atomic_t *keep_running);

/* 停止调试线程。 */
void ethercat_master_app_stop_debug_thread(ethercat_master_app_t *app);

/* 执行 1 ms 周期任务，直到 keep_running 变为 0。 */
void ethercat_master_app_run(ethercat_master_app_t *app,
                             volatile sig_atomic_t *keep_running);

/* 释放 IgH master，并清空应用状态。 */
void ethercat_master_app_release(ethercat_master_app_t *app);

#endif
