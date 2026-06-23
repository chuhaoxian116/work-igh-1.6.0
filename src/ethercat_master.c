#include "igh_master/ethercat_master.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "igh_master/app_config.h"

#define NSEC_PER_SEC 1000000000LL
#define TWO_PI 6.28318530717958647692
#define PRINT_PERIOD_CYCLES (PRINT_PERIOD_NS / CYCLE_TIME_NS)
#define DC_MONITOR_PERIOD_CYCLES (DC_MONITOR_PERIOD_NS / CYCLE_TIME_NS)
#define SINE_PERIOD_CYCLES (MOTION_PERIOD_NS / CYCLE_TIME_NS)
#define COMMUNICATION_REPORT_PERIOD_CYCLES \
    (COMMUNICATION_REPORT_PERIOD_NS / CYCLE_TIME_NS)

enum {
    CONTROL_WAIT_STATUS = 0,
    CONTROL_ENABLE_06,
    CONTROL_ENABLE_07,
    CONTROL_ENABLE_15,
    CONTROL_SINE_MOTION,
};

/* 将 monotonic timespec 转为纳秒，供 IgH application time 使用。 */
static int64_t timespec_to_ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

/* 推进绝对唤醒时间，并保持 tv_nsec 在合法范围内。 */
static void add_ns(struct timespec *ts, int64_t ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= NSEC_PER_SEC) {
        ts->tv_nsec -= NSEC_PER_SEC;
        ts->tv_sec++;
    }
}

static void ns_to_timespec(uint64_t ns, struct timespec *ts) {
    ts->tv_sec = ns / NSEC_PER_SEC;
    ts->tv_nsec = ns % NSEC_PER_SEC;
}

/* CLOCK_MONOTONIC_RAW 不受 NTP 校时影响，适合统计真实调度周期。 */
static int64_t monotonic_raw_ns(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return timespec_to_ns(&now);
}

/* 累计一次周期调度质量；首周期只建立时间基准，不计算间隔。 */
static void quality_update_cycle(ethercat_master_app_t *app, int64_t now_ns) {
    ethercat_quality_stats_t *stats = &app->quality;

    stats->cycles++;
    if (stats->start_time_ns == 0) {
        stats->start_time_ns = now_ns;
        stats->last_cycle_time_ns = now_ns;
        return;
    }

    {
        int64_t interval_ns = now_ns - stats->last_cycle_time_ns;
        int64_t jitter_ns = llabs(interval_ns - (int64_t)CYCLE_TIME_NS);

        stats->last_cycle_time_ns = now_ns;
        stats->interval_samples++;
        stats->cycle_time_sum_ns += (uint64_t)interval_ns;
        stats->jitter_abs_sum_ns += (uint64_t)jitter_ns;

        if (interval_ns < stats->min_cycle_ns) stats->min_cycle_ns = interval_ns;
        if (interval_ns > stats->max_cycle_ns) stats->max_cycle_ns = interval_ns;
        if (jitter_ns > stats->max_abs_jitter_ns) {
            stats->max_abs_jitter_ns = jitter_ns;
        }
        if ((uint64_t)interval_ns > CYCLE_OVERRUN_LIMIT_NS) {
            stats->severe_overruns++;
        }
    }
}

/* 将 IgH domain working-counter 状态转换为可累计的通信成功率。 */
static void quality_update_domain(ethercat_master_app_t *app) {
    ec_domain_state_t state;
    ethercat_quality_stats_t *stats = &app->quality;

    ecrt_domain_state(app->domain, &state);
    if (state.working_counter < stats->min_working_counter) {
        stats->min_working_counter = state.working_counter;
    }
    if (state.working_counter > stats->max_working_counter) {
        stats->max_working_counter = state.working_counter;
    }

    switch (state.wc_state) {
    case EC_WC_COMPLETE:
        stats->domain_complete_cycles++;
        break;
    case EC_WC_INCOMPLETE:
        stats->domain_incomplete_cycles++;
        break;
    default:
        stats->domain_zero_cycles++;
        break;
    }
}

/*
 * IgH 通信质量报告。
 *
 * 分钟报告和退出报告使用同一统计口径，便于与 SOEM 报告直接比较。
 * 正弦轨迹的最大变化率为 pi * range / period。
 */
static void print_communication_report(const ethercat_master_app_t *app,
                                       const drive_inputs_t *inputs,
                                       const char *title) {
    const ethercat_quality_stats_t *stats = &app->quality;
    double elapsed_s = 0.0;
    double avg_cycle_us = 0.0;
    double avg_jitter_us = 0.0;
    double complete_percent = 0.0;
    double avg_dc_us = 0.0;
    double peak_rate = (double)MOTION_PEAK_RATE_COUNTS_PER_SEC;

    if (stats->start_time_ns && stats->last_cycle_time_ns >= stats->start_time_ns) {
        elapsed_s = (double)(stats->last_cycle_time_ns - stats->start_time_ns) /
                    (double)NSEC_PER_SEC;
    }
    if (stats->interval_samples) {
        avg_cycle_us = (double)stats->cycle_time_sum_ns /
                       (double)stats->interval_samples / 1000.0;
        avg_jitter_us = (double)stats->jitter_abs_sum_ns /
                        (double)stats->interval_samples / 1000.0;
    }
    if (stats->cycles) {
        complete_percent = 100.0 * (double)stats->domain_complete_cycles /
                           (double)stats->cycles;
    }
    if (stats->dc_valid_samples) {
        avg_dc_us = (double)stats->dc_abs_sum_ns /
                    (double)stats->dc_valid_samples / 1000.0;
    }

    printf("\n============================================================\n");
    printf("              IgH 通信质量报告（%s）\n", title);
    printf("============================================================\n");

    printf("[运行概况]\n");
    printf("  运行时长          : %12.3f s\n", elapsed_s);
    printf("  累计周期          : %12llu\n",
           (unsigned long long)stats->cycles);
    printf("  标称通信周期      : %12.3f us\n",
           (double)CYCLE_TIME_NS / 1000.0);

    printf("[周期质量]\n");
    printf("  平均实际周期      : %12.3f us\n", avg_cycle_us);
    printf("  最小 / 最大周期   : %12.3f / %.3f us\n",
           stats->interval_samples ? (double)stats->min_cycle_ns / 1000.0 : 0.0,
           stats->interval_samples ? (double)stats->max_cycle_ns / 1000.0 : 0.0);
    printf("  平均绝对抖动      : %12.3f us\n", avg_jitter_us);
    printf("  最大绝对抖动      : %12.3f us\n",
           (double)stats->max_abs_jitter_ns / 1000.0);
    printf("  严重超周期次数    : %12llu  (> 150%% 标称周期)\n",
           (unsigned long long)stats->severe_overruns);

    printf("[Domain 过程数据质量]\n");
    printf("  完整周期          : %12llu\n",
           (unsigned long long)stats->domain_complete_cycles);
    printf("  不完整周期        : %12llu\n",
           (unsigned long long)stats->domain_incomplete_cycles);
    printf("  无过程数据周期    : %12llu\n",
           (unsigned long long)stats->domain_zero_cycles);
    printf("  通信成功率        : %12.6f %%\n", complete_percent);
    printf("  WC 最小 / 最大    : %12u / %u\n",
           stats->min_working_counter == UINT_MAX ? 0 : stats->min_working_counter,
           stats->max_working_counter);

    printf("[DC 同步质量]\n");
    printf("  有效 / 无效采样   : %12llu / %llu\n",
           (unsigned long long)stats->dc_valid_samples,
           (unsigned long long)stats->dc_invalid_samples);
    printf("  平均绝对误差      : %12.3f us\n", avg_dc_us);
    printf("  最大绝对误差      : %12.3f us\n",
           (double)stats->max_dc_diff_ns / 1000.0);

    printf("[PDO 与运动状态]\n");
    printf("  目标位置          : %12d pulse\n", app->outputs.target_position);
    printf("  实际位置          : %12d pulse\n", inputs->actual_position);
    printf("  位置跟随误差      : %12d pulse\n",
           app->outputs.target_position - inputs->actual_position);
    printf("  状态字 / bit12    :       0x%04X / %d\n",
           inputs->status_word, (inputs->status_word & 0x1000) ? 1 : 0);
    printf("  模式 / 控制字     : %12d / 0x%04X\n",
           inputs->operation_mode_display, app->outputs.control_word);
    printf("  目标位置范围      : base ~ base + %d pulse\n", MOTION_RANGE_COUNTS);
    printf("  运动周期          : %12.3f s\n",
           (double)MOTION_PERIOD_NS / (double)NSEC_PER_SEC);
    printf("  最大目标变化率    : %12.3f pulse/s\n", peak_rate);

    printf("[主站与从站状态]\n");
    printf("  主站链路          : %12s\n",
           app->master_state.link_up ? "正常" : "断开");
    printf("  响应从站数        : %12u\n", app->master_state.slaves_responding);
    printf("  主站 AL 状态      :         0x%02X\n", app->master_state.al_states);
    printf("  从站在线 / OP     : %12u / %u\n",
           app->drive_state.online, app->drive_state.operational);
    printf("  从站 AL 状态      :         0x%02X\n", app->drive_state.al_state);
    printf("============================================================\n");
}

static const char *control_state_name(int state) {
    switch (state) {
    case CONTROL_WAIT_STATUS:
        return "wait_status";
    case CONTROL_ENABLE_06:
        return "enable_06";
    case CONTROL_ENABLE_07:
        return "enable_07";
    case CONTROL_ENABLE_15:
        return "enable_15";
    case CONTROL_SINE_MOTION:
        return "sine_motion";
    default:
        return "unknown";
    }
}

/* 只在 master 状态变化时打印，避免每个周期都刷日志。 */
static void check_master_state(ethercat_master_app_t *app) {
    ec_master_state_t state;

    ecrt_master_state(app->master, &state);
    if (state.slaves_responding != app->master_state.slaves_responding) {
        printf("master: %u slave(s) responding\n", state.slaves_responding);
    }
    if (state.al_states != app->master_state.al_states) {
        printf("master: AL states 0x%02X\n", state.al_states);
    }
    if (state.link_up != app->master_state.link_up) {
        printf("master: link %s\n", state.link_up ? "up" : "down");
    }

    app->master_state = state;
}

/* 打印 domain working counter 和状态变化。 */
static void check_domain_state(ethercat_master_app_t *app) {
    ec_domain_state_t state;

    ecrt_domain_state(app->domain, &state);
    if (state.working_counter != app->domain_state.working_counter) {
        printf("domain: WC %u\n", state.working_counter);
    }
    if (state.wc_state != app->domain_state.wc_state) {
        printf("domain: state %u\n", state.wc_state);
    }

    app->domain_state = state;
}

/* 打印从站 online、operational、AL state 的变化。 */
static void check_drive_state(ethercat_master_app_t *app) {
    ec_slave_config_state_t state;

    ecrt_slave_config_state(app->drive_config, &state);
    if (state.al_state != app->drive_state.al_state) {
        printf("drive: AL state 0x%02X\n", state.al_state);
    }
    if (state.online != app->drive_state.online) {
        printf("drive: %s\n", state.online ? "online" : "offline");
    }
    if (state.operational != app->drive_state.operational) {
        printf("drive: %soperational\n", state.operational ? "" : "not ");
    }

    app->drive_state = state;
}

static void hold_actual_position(ethercat_master_app_t *app,
                                 const drive_inputs_t *inputs, uint16_t control_word) {
    app->outputs.target_position = inputs->actual_position;
    app->outputs.control_word = control_word;
    app->outputs.operation_mode = DRIVE_OPERATION_MODE;
}

static void set_control_state(ethercat_master_app_t *app, int state) {
    app->control_state = state;
    app->control_state_cycles = 0;
}

/*
 * 简单 CiA 402 控制状态机。
 *
 * 这里按现场要求执行 0x06 -> 0x07 -> 0x15。每个步骤保持
 * ENABLE_STEP_CYCLES 个周期。使能完成后，先锁存当前实际位置作为 base，
 * 然后执行 base -> base + 30000 -> base 的平滑正弦位置给定。
 */
static void update_drive_control(ethercat_master_app_t *app,
                                 const drive_inputs_t *inputs) {
#if ENABLE_DRIVE_CONTROL
    if (inputs->status_word == 0) {
        hold_actual_position(app, inputs, 0x0000);
        return;
    }

    switch (app->control_state) {
    case CONTROL_WAIT_STATUS:
        hold_actual_position(app, inputs, 0x0006);
        set_control_state(app, CONTROL_ENABLE_06);
        break;

    case CONTROL_ENABLE_06:
        hold_actual_position(app, inputs, 0x0006);
        if (++app->control_state_cycles >= ENABLE_STEP_CYCLES) {
            set_control_state(app, CONTROL_ENABLE_07);
        }
        break;

    case CONTROL_ENABLE_07:
        hold_actual_position(app, inputs, 0x0007);
        if (++app->control_state_cycles >= ENABLE_STEP_CYCLES) {
            set_control_state(app, CONTROL_ENABLE_15);
        }
        break;

    case CONTROL_ENABLE_15:
        hold_actual_position(app, inputs, 0x000f);
        if (++app->control_state_cycles >= ENABLE_STEP_CYCLES) {
            app->sine_base_position = inputs->actual_position;
            app->motion_cycles = 0;
            set_control_state(app, CONTROL_SINE_MOTION);
        }
        break;

    case CONTROL_SINE_MOTION: {
        double phase = TWO_PI * (double)app->motion_cycles / (double)SINE_PERIOD_CYCLES;
        double offset = (1.0 - cos(phase)) * 0.5 * MOTION_RANGE_COUNTS;

        app->outputs.target_position =
            app->sine_base_position + (int32_t)(offset + 0.5);
        app->outputs.control_word = 0x000f;
        app->outputs.operation_mode = DRIVE_OPERATION_MODE;
        app->motion_cycles = (app->motion_cycles + 1) % SINE_PERIOD_CYCLES;
        break;
    }

    default:
        set_control_state(app, CONTROL_WAIT_STATUS);
        hold_actual_position(app, inputs, 0x0000);
        break;
    }
#else
    hold_actual_position(app, inputs, 0x0000);
#endif
}

static void update_dc_monitor(ethercat_master_app_t *app, uint64_t cycle) {
    uint32_t diff_ns;

    if (cycle > 0 && cycle % DC_MONITOR_PERIOD_CYCLES == 0) {
        diff_ns = ecrt_master_sync_monitor_process(app->master);
        if (diff_ns != (uint32_t)-1) {
            app->quality.dc_valid_samples++;
            app->quality.dc_abs_sum_ns += diff_ns;
            if (diff_ns > app->quality.max_dc_diff_ns) {
                app->quality.max_dc_diff_ns = diff_ns;
            }
        } else {
            app->quality.dc_invalid_samples++;
        }

        if (pthread_mutex_trylock(&app->debug_lock) == 0) {
            if (diff_ns != (uint32_t)-1) {
                app->debug_snapshot.dc_sync_diff_ns = diff_ns;
                app->debug_snapshot.dc_sync_diff_valid = 1;
            } else {
                app->debug_snapshot.dc_sync_diff_valid = 0;
            }
            pthread_mutex_unlock(&app->debug_lock);
        }
    }
}

static void queue_dc_monitor(ethercat_master_app_t *app, uint64_t cycle) {
    if (cycle % DC_MONITOR_PERIOD_CYCLES == 0) {
        ecrt_master_sync_monitor_queue(app->master);
    }
}

static void update_debug_snapshot(ethercat_master_app_t *app, uint64_t cycle,
                                  const drive_inputs_t *inputs) {
    if (pthread_mutex_trylock(&app->debug_lock) != 0) {
        return;
    }

    app->debug_snapshot.cycle = cycle;
    app->debug_snapshot.inputs = *inputs;
    app->debug_snapshot.outputs = app->outputs;
    app->debug_snapshot.control_state = app->control_state;
    app->debug_snapshot.sine_base_position = app->sine_base_position;

    pthread_mutex_unlock(&app->debug_lock);
}

#if DEBUG_READ_SYNC_LOST_SDO
static int read_sync_lost_count(ethercat_master_app_t *app, uint32_t *value) {
    uint8_t data[4] = {0};
    size_t result_size = 0;
    uint32_t abort_code = 0;
    int ret;

    ret = ecrt_master_sdo_upload(app->master, DRIVE_POSITION, SYNC_LOST_COUNT_INDEX,
                                 SYNC_LOST_COUNT_SUBINDEX, data, sizeof(data),
                                 &result_size, &abort_code);
    if (ret) {
        return ret;
    }

    if (result_size == 1) {
        *value = data[0];
    } else if (result_size == 2) {
        *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
    } else {
        *value = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                 ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    }

    return 0;
}
#endif

static void *debug_thread_main(void *arg) {
    ethercat_master_app_t *app = (ethercat_master_app_t *)arg;
    struct timespec sleep_time;

    ns_to_timespec(DEBUG_PRINT_PERIOD_NS, &sleep_time);

    while (*app->debug_keep_running) {
        ethercat_debug_snapshot_t snapshot;

        nanosleep(&sleep_time, NULL);

        pthread_mutex_lock(&app->debug_lock);
        snapshot = app->debug_snapshot;
        pthread_mutex_unlock(&app->debug_lock);

        if (snapshot.dc_sync_diff_valid) {
            printf("debug: cycle=%llu target_position=%d control_word=0x%04X "
                   "operation_mode=%d actual_position=%d status_word=0x%04X "
                   "operation_mode_display=%d control_state=%s "
                   "sine_base_position=%d dc_sync_diff_ns=%u\n",
                   (unsigned long long)snapshot.cycle, snapshot.outputs.target_position,
                   snapshot.outputs.control_word, snapshot.outputs.operation_mode,
                   snapshot.inputs.actual_position, snapshot.inputs.status_word,
                   snapshot.inputs.operation_mode_display,
                   control_state_name(snapshot.control_state),
                   snapshot.sine_base_position, snapshot.dc_sync_diff_ns);
        } else {
            printf("debug: cycle=%llu target_position=%d control_word=0x%04X "
                   "operation_mode=%d actual_position=%d status_word=0x%04X "
                   "operation_mode_display=%d control_state=%s "
                   "sine_base_position=%d dc_sync_diff_ns=N/A\n",
                   (unsigned long long)snapshot.cycle, snapshot.outputs.target_position,
                   snapshot.outputs.control_word, snapshot.outputs.operation_mode,
                   snapshot.inputs.actual_position, snapshot.inputs.status_word,
                   snapshot.inputs.operation_mode_display,
                   control_state_name(snapshot.control_state),
                   snapshot.sine_base_position);
        }

#if DEBUG_READ_SYNC_LOST_SDO
        {
            uint32_t sync_lost_count = 0;
            if (read_sync_lost_count(app, &sync_lost_count) == 0) {
                printf("debug: sync_lost_count=%u\n", sync_lost_count);
            }
        }
#endif
    }

    return NULL;
}

void ethercat_master_app_init(ethercat_master_app_t *app) {
    memset(app, 0, sizeof(*app));
    app->quality.min_cycle_ns = INT64_MAX;
    app->quality.min_working_counter = UINT_MAX;
    pthread_mutex_init(&app->debug_lock, NULL);
    app->debug_lock_ready = 1;
}

int ethercat_master_app_configure(ethercat_master_app_t *app) {
    ec_pdo_entry_reg_t domain_regs[DRIVE_PDO_REG_COUNT + 1];

    /*
     * 所有 ecrt_* 配置调用都放在 activate 之前。ecrt_master_activate()
     * 之后，周期任务中只做实时友好的 process data 和 DC 同步调用。
     */
    app->master = ecrt_request_master(MASTER_INDEX);
    if (!app->master) {
        fprintf(stderr, "failed to request EtherCAT master %u\n", MASTER_INDEX);
        return -1;
    }

    app->domain = ecrt_master_create_domain(app->master);
    if (!app->domain) {
        fprintf(stderr, "failed to create process data domain\n");
        return -1;
    }

    app->drive_config = ecrt_master_slave_config(
        app->master, DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE);
    if (!app->drive_config) {
        fprintf(stderr,
                "failed to create drive slave config at %u:%u "
                "(vendor=0x%08X product=0x%08X)\n",
                DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE);
        return -1;
    }

    /* PDO assignment/mapping 必须在 domain 注册之前完成。 */
    printf("configuring PDOs...\n");
    if (ecrt_slave_config_pdos(app->drive_config, EC_END, drive_pdo_syncs())) {
        fprintf(stderr, "failed to configure drive PDOs\n");
        return -1;
    }

    drive_pdo_make_domain_regs(domain_regs, &app->pdo_offsets);
    if (ecrt_domain_reg_pdo_entry_list(app->domain, domain_regs)) {
        fprintf(stderr, "failed to register PDO entries\n");
        return -1;
    }

    /*
     * 选择伺服作为 DC 参考时钟，并将 SYNC0 配置为和用户态循环一致的周期。
     */
    if (ecrt_master_select_reference_clock(app->master, app->drive_config)) {
        fprintf(stderr, "failed to select DC reference clock\n");
        return -1;
    }

    if (ecrt_slave_config_dc(app->drive_config, DC_ASSIGN_ACTIVATE, CYCLE_TIME_NS,
                             DC_SYNC0_SHIFT_NS, 0, 0)) {
        fprintf(stderr, "failed to configure distributed clocks\n");
        return -1;
    }

    printf("activating master...\n");
    if (ecrt_master_activate(app->master)) {
        fprintf(stderr, "failed to activate master\n");
        return -1;
    }

    /* process data 指针在 master 成功激活后才有效。 */
    app->domain_pd = ecrt_domain_data(app->domain);
    if (!app->domain_pd) {
        fprintf(stderr, "failed to get domain data pointer\n");
        return -1;
    }

    printf("motion profile: range=[base, base+%d], period=%.3fs, "
           "peak_target_rate=%.3f pulse/s\n",
           MOTION_RANGE_COUNTS,
           (double)MOTION_PERIOD_NS / (double)NSEC_PER_SEC,
           (double)MOTION_PEAK_RATE_COUNTS_PER_SEC);

    return 0;
}

int ethercat_master_app_start_debug_thread(ethercat_master_app_t *app,
                                           volatile sig_atomic_t *keep_running) {
    app->debug_keep_running = keep_running;
    if (pthread_create(&app->debug_thread, NULL, debug_thread_main, app)) {
        fprintf(stderr, "failed to create debug thread\n");
        return -1;
    }

    app->debug_thread_running = 1;
    return 0;
}

void ethercat_master_app_stop_debug_thread(ethercat_master_app_t *app) {
    if (app->debug_thread_running) {
        pthread_join(app->debug_thread, NULL);
        app->debug_thread_running = 0;
    }
}

void ethercat_master_app_run(ethercat_master_app_t *app,
                             volatile sig_atomic_t *keep_running) {
    struct timespec wakeup_time;
    drive_inputs_t last_inputs = {0};
    uint64_t cycle = 0;

    clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
    add_ns(&wakeup_time, CYCLE_TIME_NS);

    /*
     * 使用绝对时间睡眠，避免周期误差累积。目标唤醒时间也会传给 IgH 作为
     * application time，让 DC 同步更稳定。
     */
    while (*keep_running) {
        drive_inputs_t inputs;
        int ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);
        if (ret) {
            if (ret == EINTR) {
                continue;
            }
            fprintf(stderr, "clock_nanosleep failed: %s\n", strerror(ret));
            break;
        }

        ecrt_master_application_time(app->master,
                                     (uint64_t)timespec_to_ns(&wakeup_time));

        ecrt_master_receive(app->master);
        ecrt_domain_process(app->domain);

        inputs = drive_pdo_read_inputs(app->domain_pd, &app->pdo_offsets);
        last_inputs = inputs;

        /* 周期与 domain 质量在 process 完成本周期数据后统一采样。 */
        quality_update_cycle(app, monotonic_raw_ns());
        quality_update_domain(app);

        if (cycle % PRINT_PERIOD_CYCLES == 0) {
            check_master_state(app);
            check_domain_state(app);
            check_drive_state(app);
        }

        update_dc_monitor(app, cycle);
        update_drive_control(app, &inputs);
        drive_pdo_write_outputs(app->domain_pd, &app->pdo_offsets, &app->outputs);
        update_debug_snapshot(app, cycle, &inputs);

        /*
         * 发送 process data 帧之前先排队 DC 同步报文。参考时钟会跟随上面设置
         * 的 application time。
         */
        ecrt_master_sync_reference_clock(app->master);
        ecrt_master_sync_slave_clocks(app->master);
        queue_dc_monitor(app, cycle);

        ecrt_domain_queue(app->domain);
        ecrt_master_send(app->master);

        cycle++;
        if (cycle % COMMUNICATION_REPORT_PERIOD_CYCLES == 0) {
            print_communication_report(app, &last_inputs, "每分钟累计");
        }
        add_ns(&wakeup_time, CYCLE_TIME_NS);
    }

    /* Ctrl+C 使循环退出后、释放 master 前输出最终完整报告。 */
    print_communication_report(app, &last_inputs, "Ctrl+C 最终汇总");
}

void ethercat_master_app_release(ethercat_master_app_t *app) {
    ethercat_master_app_stop_debug_thread(app);

    if (app->master) {
        ecrt_release_master(app->master);
    }

    if (app->debug_lock_ready) {
        pthread_mutex_destroy(&app->debug_lock);
    }

    memset(app, 0, sizeof(*app));
}
