#include "ethercat_app.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <climits>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <sys/mman.h>

#include "axis_config.h"
#include "sdo_parameter.h"

namespace sinsun {
namespace {

/*
 * 将 timespec 转成纳秒。
 *
 * ts：待转换的绝对时间，用于传给 IgH application time。
 */
int64_t timespec_to_ns(const timespec &ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

/*
 * 在 timespec 上累加纳秒，并修正 tv_nsec 溢出。
 *
 * ts：周期循环下一次唤醒的绝对时间。
 * ns：需要累加的纳秒数，当前每次累加 1 ms。
 */
void add_ns(timespec &ts, int64_t ns) {
    ts.tv_nsec += ns;
    while (ts.tv_nsec >= 1000000000LL) {
        ts.tv_nsec -= 1000000000LL;
        ++ts.tv_sec;
    }
}

/* 预触碰栈内存，降低后续实时循环中发生缺页的概率。 */
void prefault_stack() {
    /* dummy：用于主动访问一段栈空间，避免运行期首次触碰才分配物理页。 */
    volatile unsigned char dummy[kMaxSafeStack];

    /* i：栈预触碰循环下标。 */
    for (size_t i = 0; i < sizeof(dummy); ++i) {
        dummy[i] = 0;
    }
}

/*
 * 配置一个 SINSUN 伺服从站。
 *
 * app：主站运行期上下文，保存 master 句柄和伺服配置句柄。
 * index：0-based 伺服下标，0-5 对应用户侧 id 1-6。
 */
int configure_servo(App &app, size_t index) {
    /* position：IgH 使用的 0-based 从站位置。 */
    const uint16_t position = static_cast<uint16_t>(index);

    /* logical_id：用户侧看到的 1-based 伺服 id，只用于日志打印。 */
    const unsigned int logical_id = static_cast<unsigned int>(index + 1);

    /* config：当前伺服的 IgH slave config 句柄。 */
    ec_slave_config_t *config = ecrt_master_slave_config(
        app.master, kAlias, position, kVendorId, kServoProductCode);
    if (!config) {
        std::fprintf(stderr, "failed to get Servo %u slave config\n", logical_id);
        return -1;
    }

    if (ecrt_slave_config_pdos(config, EC_END, servo_syncs)) {
        std::fprintf(stderr, "failed to configure Servo %u PDOs\n", logical_id);
        return -1;
    }

    ecrt_slave_config_dc(config, kDcAssignActivate, kCycleTimeNs, 0, 0, 0);
    app.servo_configs[index] = config;
    return 0;
}

/*
 * 配置拓扑中的末端 IO 从站。
 *
 * app：主站运行期上下文，保存 master 句柄和末端 IO 配置句柄。
 */
int configure_endio(App &app) {
    /* app.endio_config：末端 IO 的 IgH slave config 句柄。 */
    app.endio_config = ecrt_master_slave_config(
        app.master, kAlias, kEndIoPosition, kVendorId, kEndIoProductCode);
    if (!app.endio_config) {
        std::fprintf(stderr, "failed to get EndIO %u slave config\n",
                     kEndIoLogicalId);
        return -1;
    }

    if (ecrt_slave_config_pdos(app.endio_config, EC_END, endio_syncs)) {
        std::fprintf(stderr, "failed to configure EndIO %u PDOs\n",
                     kEndIoLogicalId);
        return -1;
    }

    return 0;
}

/* PDO 注册项上限。
 *
 * 当前注册：
 * - 6 个伺服：9 个 RxPDO 输出 + 8 个 TxPDO 输入，共 102 项
 * - 末端 IO：36 个 RxPDO 输出 + 10 个 TxPDO 输入，共 46 项
 * 合计 148 项，256 留有余量。
 */
constexpr size_t kMaxPdoEntryRegs = 256;

constexpr uint64_t kDcMonitorPeriodCycles = 1000;
constexpr uint64_t kCycleOverrunLimitNs =
    (static_cast<uint64_t>(kCycleTimeNs) * 3ULL) / 2ULL;
constexpr double kPi = 3.14159265358979323846;

enum class ServoControlState {
    WaitStatus,
    Enable06,
    Enable07,
    Enable0F,
    SineMotion,
};

struct ServoMotionControl {
    ServoControlState state = ServoControlState::WaitStatus;
    bool waiting_op_logged = false;
    uint64_t state_cycles = 0;
    uint64_t motion_cycles = 0;
    int32_t base_position = 0;
};

struct QualityStats {
    bool active = false;
    bool dc_monitor_armed = false;

    uint64_t cycles = 0;
    uint64_t interval_samples = 0;
    uint64_t cycle_time_sum_ns = 0;
    uint64_t jitter_abs_sum_ns = 0;
    uint64_t severe_overruns = 0;

    uint64_t domain_complete_cycles = 0;
    uint64_t domain_incomplete_cycles = 0;
    uint64_t domain_zero_cycles = 0;
    uint64_t domain_state_read_errors = 0;

    uint32_t wc_last = 0;
    uint32_t min_working_counter = UINT_MAX;
    uint32_t max_working_counter = 0;

    uint64_t dc_valid_samples = 0;
    uint64_t dc_invalid_samples = 0;
    uint64_t dc_abs_sum_ns = 0;
    uint32_t max_dc_diff_ns = 0;

    int64_t start_time_ns = 0;
    int64_t last_cycle_time_ns = 0;
    int64_t min_cycle_ns = INT64_MAX;
    int64_t max_cycle_ns = 0;
    int64_t max_abs_jitter_ns = 0;
};

double ns_to_us(int64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

int64_t abs_int64(int64_t value) {
    return value < 0 ? -value : value;
}

/* CLOCK_MONOTONIC_RAW 不受 NTP 校时影响，用于统计真实调度周期。 */
int64_t monotonic_raw_ns() {
    timespec now {};
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
#else
    clock_gettime(CLOCK_MONOTONIC, &now);
#endif
    return timespec_to_ns(now);
}

/*
 * 向 domain PDO 注册表追加一个 entry。
 *
 * regs：待提交给 ecrt_domain_reg_pdo_entry_list() 的注册表数组。
 * reg_index：当前写入位置，函数内部写入后自增。
 * position：IgH 0-based 从站位置。
 * product_code：从站 ProductCode，用于区分伺服和末端 IO。
 * pdo_index：PDO entry 的对象字典 index。
 * subindex：PDO entry 的对象字典 subindex。
 * offset：IgH 写回过程数据字节偏移的位置。
 */
void add_reg(std::array<ec_pdo_entry_reg_t, kMaxPdoEntryRegs> &regs,
             size_t &reg_index,
             uint16_t position,
             uint32_t product_code,
             uint16_t pdo_index,
             uint8_t subindex,
             unsigned int *offset) {
    regs[reg_index++] = ec_pdo_entry_reg_t{
        kAlias, position, kVendorId, product_code, pdo_index, subindex, offset,
        nullptr,
    };
}

/*
 * 注册本程序需要读取和打印的 PDO entry。
 *
 * app：主站运行期上下文，函数会填充 app 中的各个 PDO offset 字段。
 */
int register_domain_entries(App &app) {
    /* regs：PDO entry 注册表，最后一个空元素作为 IgH 的结束标记。 */
    std::array<ec_pdo_entry_reg_t, kMaxPdoEntryRegs> regs {};

    /* reg_index：regs 当前写入下标。 */
    size_t reg_index = 0;

    /* i：伺服数组下标，0-5 对应用户侧 id 1-6。 */
    for (size_t i = 0; i < kServoCount; ++i) {
        /* pos：IgH 从站位置，和当前伺服数组下标一致。 */
        const uint16_t pos = static_cast<uint16_t>(i);

        /* out：当前伺服 RxPDO 输出 offset。 */
        ServoOutputOffsets &out = app.servo_output_offsets[i];

        add_reg(regs, reg_index, pos, kServoProductCode, 0x607A, 0x00,
                &out.target_position);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x60FE, 0x00,
                &out.digital_outputs);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x60FF, 0x00,
                &out.target_velocity);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6040, 0x00,
                &out.control_word);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6071, 0x00,
                &out.target_torque);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6060, 0x00,
                &out.operation_mode);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x7006, 0x00,
                &out.safe_control);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x7007, 0x00,
                &out.target_safe_position);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x7008, 0x00,
                &out.user_output);

        /* in：当前伺服 TxPDO 输入 offset。 */
        ServoOffsets &in = app.servo_offsets[i];

        add_reg(regs, reg_index, pos, kServoProductCode, 0x6064, 0x00,
                &in.actual_position);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x60FD, 0x00,
                &in.digital_input);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x603F, 0x00,
                &in.error_code);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x606C, 0x00,
                &in.actual_velocity);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6041, 0x00,
                &in.status_word);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6077, 0x00,
                &in.actual_torque);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6061, 0x00,
                &in.operation_mode_display);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x600B, 0x00,
                &in.sync0_time_difference);
    }

    /* 末端 IO RxPDO 输出 offset。 */
    EndIoOutputOffsets &endio_out = app.endio_output_offsets;

    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x7000, 0x01,
            &endio_out.led_work_control);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x7000, 0x02,
            &endio_out.digital_outputs_control);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x7000, 0x03,
            &endio_out.rs485_outputs_count);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x7000, 0x04,
            &endio_out.rs485_outputs_len);

    for (uint8_t subindex = 0x05; subindex <= 0x24; ++subindex) {
        const size_t data_index = static_cast<size_t>(subindex - 0x05);
        add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x7000,
                subindex, &endio_out.rs485_outputs_data[data_index]);
    }

    /* 末端 IO TxPDO 输入 offset。 */
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x01,
            &app.endio_offsets.error_code);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x02,
            &app.endio_offsets.digital_inputs);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x03,
            &app.endio_offsets.analog_voltage_1);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x04,
            &app.endio_offsets.analog_voltage_2);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x05,
            &app.endio_offsets.temperature);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x06,
            &app.endio_offsets.acceleration_1);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x07,
            &app.endio_offsets.acceleration_2);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x08,
            &app.endio_offsets.acceleration_3);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x09,
            &app.endio_offsets.rs485_inputs_count);
    add_reg(regs, reg_index, kEndIoPosition, kEndIoProductCode, 0x6000, 0x0A,
            &app.endio_offsets.rs485_inputs_len);

    if (reg_index + 1 >= regs.size()) {
        std::fprintf(stderr, "too many PDO entries registered: %zu/%zu\n",
                     reg_index, regs.size());
        return -1;
    }

    regs[reg_index] = ec_pdo_entry_reg_t{};

    std::printf("[PDO] registered %zu PDO entries "
                "(including RxPDO outputs and TxPDO inputs)\n",
                reg_index);

    if (ecrt_domain_reg_pdo_entry_list(app.domain, regs.data())) {
        std::fprintf(stderr, "failed to register PDO entries\n");
        return -1;
    }

    return 0;
}

/*
 * 打印 master/domain/从站状态变化。
 *
 * app：主站运行期上下文，保存上一次状态快照以避免每周期刷屏。
 */
void print_state_changes(App &app) {
    /* master_state：本周期读取到的 master 状态。 */
    ec_master_state_t master_state {};
    if (ecrt_master_state(app.master, &master_state) == 0 &&
        (master_state.slaves_responding != app.master_state.slaves_responding ||
         master_state.al_states != app.master_state.al_states ||
         master_state.link_up != app.master_state.link_up)) {
        std::printf("master: slaves=%u al_states=0x%02X link=%s\n",
                    master_state.slaves_responding,
                    master_state.al_states,
                    master_state.link_up ? "up" : "down");
        app.master_state = master_state;
    }

    /* domain_state：本周期读取到的 domain working-counter 状态。 */
    ec_domain_state_t domain_state {};
    if (ecrt_domain_state(app.domain, &domain_state) == 0 &&
        (domain_state.working_counter != app.domain_state.working_counter ||
         domain_state.wc_state != app.domain_state.wc_state)) {
        std::printf("domain: wc=%u state=%u\n",
                    domain_state.working_counter,
                    domain_state.wc_state);
        app.domain_state = domain_state;
    }

    /* i：伺服数组下标，0-5 对应用户侧 id 1-6。 */
    for (size_t i = 0; i < kServoCount; ++i) {
        /* state：本周期读取到的单个伺服应用层状态。 */
        ec_slave_config_state_t state {};
        if (ecrt_slave_config_state(app.servo_configs[i], &state) == 0 &&
            (state.al_state != app.servo_states[i].al_state ||
             state.online != app.servo_states[i].online ||
             state.operational != app.servo_states[i].operational)) {
            std::printf("servo %zu: al_state=0x%02X online=%u operational=%u\n",
                        i + 1,
                        state.al_state,
                        state.online,
                        state.operational);
            app.servo_states[i] = state;
        }
    }

    /* endio_state：本周期读取到的末端 IO 应用层状态。 */
    ec_slave_config_state_t endio_state {};
    if (ecrt_slave_config_state(app.endio_config, &endio_state) == 0 &&
        (endio_state.al_state != app.endio_state.al_state ||
         endio_state.online != app.endio_state.online ||
         endio_state.operational != app.endio_state.operational)) {
        std::printf("endio %u: al_state=0x%02X online=%u operational=%u\n",
                    kEndIoLogicalId,
                    endio_state.al_state,
                    endio_state.online,
                    endio_state.operational);
        app.endio_state = endio_state;
    }
}

void write_servo_zero_output(uint8_t *pd, const ServoOutputOffsets &o) {
    EC_WRITE_S32(pd + o.target_position, 0);
    EC_WRITE_U32(pd + o.digital_outputs, 0);
    EC_WRITE_S32(pd + o.target_velocity, 0);
    EC_WRITE_U16(pd + o.control_word, 0x0000);
    EC_WRITE_S16(pd + o.target_torque, 0);
    EC_WRITE_U8(pd + o.operation_mode, 0);
    EC_WRITE_U8(pd + o.safe_control, 0);
    EC_WRITE_S32(pd + o.target_safe_position, 0);
    EC_WRITE_U32(pd + o.user_output, 0);
}

void write_endio_zero_output(uint8_t *pd, const EndIoOutputOffsets &e) {
    EC_WRITE_U8(pd + e.led_work_control, 0);
    EC_WRITE_U8(pd + e.digital_outputs_control, 0);
    EC_WRITE_U16(pd + e.rs485_outputs_count, 0);
    EC_WRITE_U16(pd + e.rs485_outputs_len, 0);

    for (const auto offset : e.rs485_outputs_data) {
        EC_WRITE_U8(pd + offset, 0);
    }
}

void hold_motion_servo_position(App &app, uint16_t control_word) {
    uint8_t *pd = app.domain_pd;
    const ServoOutputOffsets &out = app.servo_output_offsets[kMotionServoIndex];
    const ServoOffsets &in = app.servo_offsets[kMotionServoIndex];
    const int32_t actual_position = EC_READ_S32(pd + in.actual_position);

    EC_WRITE_S32(pd + out.target_position, actual_position);
    EC_WRITE_U32(pd + out.digital_outputs, 0);
    EC_WRITE_S32(pd + out.target_velocity, 0);
    EC_WRITE_U16(pd + out.control_word, control_word);
    EC_WRITE_S16(pd + out.target_torque, 0);
    EC_WRITE_U8(pd + out.operation_mode, kDriveOperationMode);
    EC_WRITE_U8(pd + out.safe_control, 0);
    EC_WRITE_S32(pd + out.target_safe_position, 0);
    EC_WRITE_U32(pd + out.user_output, 0);
}

void set_servo_control_state(ServoMotionControl &control,
                             ServoControlState state) {
    control.state = state;
    control.state_cycles = 0;
}

void log_servo6_control_step(const char *step,
                             uint16_t status_word,
                             int32_t actual_position,
                             uint16_t control_word) {
    std::printf("Servo 6 control: %-18s status=0x%04X actual=%d "
                "control=0x%04X\n",
                step,
                status_word,
                actual_position,
                control_word);
}

/*
 * 第 6 轴 CiA402 使能和小范围往返运动。
 *
 * 只对 kMotionServoIndex 写 0x6040/0x607A/0x6060；其它 5 个伺服继续写 0，
 * 避免误使能。使能完成后锁存当前实际位置为 base，再执行以 base 为中心
 * 的正弦位置轨迹；启动阶段用渐变包络逐步拉起振幅，避免速度突变。
 */
void update_motion_servo_control(App &app, ServoMotionControl &control) {
    uint8_t *pd = app.domain_pd;
    const ServoOffsets &in = app.servo_offsets[kMotionServoIndex];
    const ServoOutputOffsets &out =
        app.servo_output_offsets[kMotionServoIndex];
    const uint16_t status_word = EC_READ_U16(pd + in.status_word);
    const int32_t actual_position = EC_READ_S32(pd + in.actual_position);
    const ec_slave_config_state_t &servo6_state =
        app.servo_states[kMotionServoIndex];

    if (!servo6_state.online || !servo6_state.operational ||
        servo6_state.al_state != 0x08) {
        if (!control.waiting_op_logged) {
            std::printf("Servo 6 control: waiting OP, AL=0x%02X online=%u "
                        "OP=%u\n",
                        servo6_state.al_state,
                        servo6_state.online,
                        servo6_state.operational);
            control.waiting_op_logged = true;
        }
        write_servo_zero_output(pd, out);
        return;
    }

    if (control.waiting_op_logged) {
        std::printf("Servo 6 control: OP ready, status=0x%04X actual=%d\n",
                    status_word,
                    actual_position);
        control.waiting_op_logged = false;
    }

    if (status_word == 0) {
        hold_motion_servo_position(app, 0x0000);
        return;
    }

    switch (control.state) {
    case ServoControlState::WaitStatus:
        hold_motion_servo_position(app, 0x0006);
        log_servo6_control_step("start 0x06",
                                status_word,
                                actual_position,
                                0x0006);
        set_servo_control_state(control, ServoControlState::Enable06);
        break;

    case ServoControlState::Enable06:
        hold_motion_servo_position(app, 0x0006);
        if (++control.state_cycles >= kEnableStepCycles) {
            log_servo6_control_step("0x06 -> 0x07",
                                    status_word,
                                    actual_position,
                                    0x0007);
            set_servo_control_state(control, ServoControlState::Enable07);
        }
        break;

    case ServoControlState::Enable07:
        hold_motion_servo_position(app, 0x0007);
        if (++control.state_cycles >= kEnableStepCycles) {
            log_servo6_control_step("0x07 -> 0x0F",
                                    status_word,
                                    actual_position,
                                    0x000F);
            set_servo_control_state(control, ServoControlState::Enable0F);
        }
        break;

    case ServoControlState::Enable0F:
        hold_motion_servo_position(app, 0x000F);
        if (++control.state_cycles >= kEnableStepCycles) {
            control.base_position = actual_position;
            control.motion_cycles = 0;
            log_servo6_control_step("motion ready",
                                    status_word,
                                    actual_position,
                                    0x000F);
            set_servo_control_state(control, ServoControlState::SineMotion);
            std::printf("Servo 6 motion: base=%d amplitude=%d "
                        "peak_to_peak=%d period=%llu cycles ramp=%llu cycles\n",
                        control.base_position,
                        kMotionAmplitudeCounts,
                        kMotionRangeCounts,
                        static_cast<unsigned long long>(kMotionPeriodCycles),
                        static_cast<unsigned long long>(kMotionRampCycles));
        }
        break;

    case ServoControlState::SineMotion: {
        const double phase = 2.0 * kPi *
            static_cast<double>(control.motion_cycles) /
            static_cast<double>(kMotionPeriodCycles);
        const double ramp_progress = control.motion_cycles >= kMotionRampCycles ?
            1.0 : static_cast<double>(control.motion_cycles) /
                static_cast<double>(kMotionRampCycles);
        const double envelope = 0.5 - 0.5 * std::cos(kPi * ramp_progress);
        const double amplitude = static_cast<double>(kMotionAmplitudeCounts);
        const double offset = envelope * amplitude * std::sin(phase);
        const int32_t target_position =
            control.base_position + static_cast<int32_t>(std::lround(offset));

        EC_WRITE_S32(pd + out.target_position, target_position);
        EC_WRITE_U32(pd + out.digital_outputs, 0);
        EC_WRITE_S32(pd + out.target_velocity, 0);
        EC_WRITE_U16(pd + out.control_word, 0x000F);
        EC_WRITE_S16(pd + out.target_torque, 0);
        EC_WRITE_U8(pd + out.operation_mode, kDriveOperationMode);
        EC_WRITE_U8(pd + out.safe_control, 0);
        EC_WRITE_S32(pd + out.target_safe_position, 0);
        EC_WRITE_U32(pd + out.user_output, 0);

        control.motion_cycles =
            (control.motion_cycles + 1) % kMotionPeriodCycles;
        break;
    }
    }
}

/*
 * 周期性写入输出 PDO。
 *
 * 前 5 个伺服和末端 IO 只刷新默认 0 输出；最后一个伺服关节单独上使能并
 * 做小范围慢速往返运动。
 */
void write_cycle_outputs(App &app, ServoMotionControl &control) {
    uint8_t *pd = app.domain_pd;

    for (size_t i = 0; i < kServoCount; ++i) {
        if (i == kMotionServoIndex) {
            continue;
        }
        write_servo_zero_output(pd, app.servo_output_offsets[i]);
    }

    update_motion_servo_control(app, control);
    write_endio_zero_output(pd, app.endio_output_offsets);
}

/* 激活 master 后先写一次全零输出，等待首个有效 TxPDO 后再执行第 6 轴控制。 */
void write_default_outputs(App &app) {
    uint8_t *pd = app.domain_pd;

    for (size_t i = 0; i < kServoCount; ++i) {
        write_servo_zero_output(pd, app.servo_output_offsets[i]);
    }

    write_endio_zero_output(pd, app.endio_output_offsets);
}


bool all_slaves_operational(App &app) {
    for (size_t i = 0; i < kServoCount; ++i) {
        ec_slave_config_state_t state {};
        if (ecrt_slave_config_state(app.servo_configs[i], &state) != 0) {
            return false;
        }
        if (!state.online || !state.operational || state.al_state != 0x08) {
            return false;
        }
    }

    ec_slave_config_state_t endio_state {};
    if (ecrt_slave_config_state(app.endio_config, &endio_state) != 0) {
        return false;
    }

    return endio_state.online && endio_state.operational &&
           endio_state.al_state == 0x08;
}

void quality_update_cycle(QualityStats &stats, int64_t now_ns) {
    ++stats.cycles;

    if (stats.start_time_ns == 0) {
        stats.start_time_ns = now_ns;
        stats.last_cycle_time_ns = now_ns;
        return;
    }

    const int64_t interval_ns = now_ns - stats.last_cycle_time_ns;
    const int64_t jitter_ns = abs_int64(interval_ns -
                                        static_cast<int64_t>(kCycleTimeNs));

    stats.last_cycle_time_ns = now_ns;
    ++stats.interval_samples;
    stats.cycle_time_sum_ns += static_cast<uint64_t>(interval_ns);
    stats.jitter_abs_sum_ns += static_cast<uint64_t>(jitter_ns);

    if (interval_ns < stats.min_cycle_ns) {
        stats.min_cycle_ns = interval_ns;
    }
    if (interval_ns > stats.max_cycle_ns) {
        stats.max_cycle_ns = interval_ns;
    }
    if (jitter_ns > stats.max_abs_jitter_ns) {
        stats.max_abs_jitter_ns = jitter_ns;
    }
    if (static_cast<uint64_t>(interval_ns) > kCycleOverrunLimitNs) {
        ++stats.severe_overruns;
    }
}

void quality_update_domain(QualityStats &stats,
                           const ec_domain_state_t *domain_state) {
    if (!domain_state) {
        ++stats.domain_state_read_errors;
        return;
    }

    stats.wc_last = domain_state->working_counter;
    if (domain_state->working_counter < stats.min_working_counter) {
        stats.min_working_counter = domain_state->working_counter;
    }
    if (domain_state->working_counter > stats.max_working_counter) {
        stats.max_working_counter = domain_state->working_counter;
    }

    switch (domain_state->wc_state) {
    case EC_WC_COMPLETE:
        ++stats.domain_complete_cycles;
        break;
    case EC_WC_INCOMPLETE:
        ++stats.domain_incomplete_cycles;
        break;
    default:
        ++stats.domain_zero_cycles;
        break;
    }
}

bool quality_update_after_op(App &app,
                             QualityStats &stats,
                             int64_t now_ns,
                             const ec_domain_state_t *domain_state) {
    if (!stats.active) {
        if (!all_slaves_operational(app)) {
            return false;
        }

        stats.active = true;
        std::printf("通信质量统计开始：7 个从站已全部进入 OP，启动阶段数据不计入统计。\n");
    }

    quality_update_cycle(stats, now_ns);
    quality_update_domain(stats, domain_state);
    return true;
}

void update_dc_monitor(App &app, QualityStats &stats, uint64_t cycle) {
    if (cycle > 0 && cycle % kDcMonitorPeriodCycles == 0) {
        const uint32_t diff_ns = ecrt_master_sync_monitor_process(app.master);

        if (stats.active && stats.dc_monitor_armed) {
            if (diff_ns != static_cast<uint32_t>(-1)) {
                ++stats.dc_valid_samples;
                stats.dc_abs_sum_ns += diff_ns;
                if (diff_ns > stats.max_dc_diff_ns) {
                    stats.max_dc_diff_ns = diff_ns;
                }
            } else {
                ++stats.dc_invalid_samples;
            }
        }
    }
}

void queue_dc_monitor(App &app, QualityStats &stats, uint64_t cycle) {
    if (stats.active && cycle % kDcMonitorPeriodCycles == 0) {
        ecrt_master_sync_monitor_queue(app.master);
        stats.dc_monitor_armed = true;
    }
}

void print_quality_report(App &app,
                          const QualityStats &stats,
                          bool final_report) {
    /* 退出报告和分钟报告共用同一套统计口径，保持与 GSD620 示例一致。 */
    const char *title = final_report ? "Ctrl+C 最终汇总" : "每分钟累计";
    ec_master_state_t master_state {};
    ec_domain_state_t domain_state {};
    std::array<ec_slave_config_state_t, kServoCount> servo_states {};
    ec_slave_config_state_t endio_state {};
    size_t online_count = 0;
    size_t op_count = 0;

    ecrt_master_state(app.master, &master_state);
    ecrt_domain_state(app.domain, &domain_state);

    for (size_t i = 0; i < kServoCount; ++i) {
        ecrt_slave_config_state(app.servo_configs[i], &servo_states[i]);
        if (servo_states[i].online) {
            ++online_count;
        }
        if (servo_states[i].operational) {
            ++op_count;
        }
    }

    ecrt_slave_config_state(app.endio_config, &endio_state);
    if (endio_state.online) {
        ++online_count;
    }
    if (endio_state.operational) {
        ++op_count;
    }

    double runtime_s = 0.0;
    if (stats.start_time_ns != 0 && stats.last_cycle_time_ns >= stats.start_time_ns) {
        runtime_s = static_cast<double>(stats.last_cycle_time_ns - stats.start_time_ns) /
                    1000000000.0;
    }

    const double nominal_period_us = ns_to_us(kCycleTimeNs);
    const double avg_period_us = stats.interval_samples > 0 ?
        ns_to_us(static_cast<int64_t>(stats.cycle_time_sum_ns /
                                      stats.interval_samples)) : 0.0;
    const double avg_jitter_us = stats.interval_samples > 0 ?
        ns_to_us(static_cast<int64_t>(stats.jitter_abs_sum_ns /
                                      stats.interval_samples)) : 0.0;
    const double success_rate = stats.cycles > 0 ?
        static_cast<double>(stats.domain_complete_cycles) * 100.0 /
            static_cast<double>(stats.cycles) : 0.0;
    const double dc_avg_abs_us = stats.dc_valid_samples > 0 ?
        ns_to_us(static_cast<int64_t>(stats.dc_abs_sum_ns /
                                      stats.dc_valid_samples)) : 0.0;
    const uint32_t wc_min = stats.min_working_counter == UINT_MAX ?
        0 : stats.min_working_counter;

    std::printf("\n============================================================\n");
    std::printf("              IgH 通信质量报告（%s）\n", title);
    std::printf("============================================================\n");

    if (!stats.active) {
        std::printf("[统计状态]\n");
        std::printf("  7 个从站尚未全部进入 OP，本次没有有效运行期通信样本。\n");
        std::printf("  INIT / PRE-OP / SAFE-OP 启动阶段数据均未计入。\n");
        std::printf("============================================================\n");
        return;
    }

    /* 周期质量只统计全部从站进入 OP 之后的运行期样本。 */
    std::printf("[运行概况]\n");
    std::printf("  运行时长          : %12.3f s\n", runtime_s);
    std::printf("  累计周期          : %12llu\n",
                static_cast<unsigned long long>(stats.cycles));
    std::printf("  标称通信周期      : %12.3f us\n", nominal_period_us);

    std::printf("[周期质量]\n");
    std::printf("  平均实际周期      : %12.3f us\n", avg_period_us);
    std::printf("  最小 / 最大周期   : %12.3f / %.3f us\n",
                stats.interval_samples > 0 ? ns_to_us(stats.min_cycle_ns) : 0.0,
                stats.interval_samples > 0 ? ns_to_us(stats.max_cycle_ns) : 0.0);
    std::printf("  平均绝对抖动      : %12.3f us\n", avg_jitter_us);
    std::printf("  最大绝对抖动      : %12.3f us\n",
                ns_to_us(stats.max_abs_jitter_ns));
    std::printf("  严重超周期次数    : %12llu  (> 150%% 标称周期)\n",
                static_cast<unsigned long long>(stats.severe_overruns));

    /* Domain 的 WC 状态用于衡量本周期过程数据是否完整交换。 */
    std::printf("[Domain 过程数据质量]\n");
    std::printf("  完整周期          : %12llu\n",
                static_cast<unsigned long long>(stats.domain_complete_cycles));
    std::printf("  不完整周期        : %12llu\n",
                static_cast<unsigned long long>(stats.domain_incomplete_cycles));
    std::printf("  无过程数据周期    : %12llu\n",
                static_cast<unsigned long long>(stats.domain_zero_cycles));
    std::printf("  Domain 读取失败   : %12llu\n",
                static_cast<unsigned long long>(stats.domain_state_read_errors));
    std::printf("  通信成功率        : %12.6f %%\n", success_rate);
    std::printf("  WC 最小 / 最大    : %12u / %u\n",
                wc_min, stats.max_working_counter);

    /* DC 误差来自 IgH sync monitor，单位统一换算成 us 方便对比。 */
    std::printf("[DC 同步质量]\n");
    std::printf("  有效 / 无效采样   : %12llu / %llu\n",
                static_cast<unsigned long long>(stats.dc_valid_samples),
                static_cast<unsigned long long>(stats.dc_invalid_samples));
    std::printf("  平均绝对误差      : %12.3f us\n", dc_avg_abs_us);
    std::printf("  最大绝对误差      : %12.3f us\n",
                ns_to_us(stats.max_dc_diff_ns));

    /* 6 个伺服逐行打印反馈值和主站下发值，便于对照目标与实际。 */
    std::printf("[PDO 与从站状态]\n");
    const uint8_t *pd = app.domain_pd;
    for (size_t i = 0; i < kServoCount; ++i) {
        const ServoOffsets &in = app.servo_offsets[i];
        const ServoOutputOffsets &out = app.servo_output_offsets[i];
        std::printf("  Servo %zu actual status/mode/pos/vel/err: "
                    "0x%04X / %d / %d / %d / 0x%08X\n",
                    i + 1,
                    EC_READ_U16(pd + in.status_word),
                    EC_READ_S8(pd + in.operation_mode_display),
                    EC_READ_S32(pd + in.actual_position),
                    EC_READ_S32(pd + in.actual_velocity),
                    EC_READ_U32(pd + in.error_code));
        std::printf("          target ctrl/mode/pos          : "
                    "0x%04X / %d / %d\n",
                    EC_READ_U16(pd + out.control_word),
                    EC_READ_S8(pd + out.operation_mode),
                    EC_READ_S32(pd + out.target_position));
    }

    /* EndIO 单独打印数字量、模拟量和 RS485 输入长度，便于现场快速核对。 */
    const EndIoOffsets &e = app.endio_offsets;
    std::printf("  EndIO %u err/din     :       0x%02X / 0x%02X\n",
                kEndIoLogicalId,
                EC_READ_U8(pd + e.error_code),
                EC_READ_U8(pd + e.digital_inputs));
    std::printf("          ai1/ai2/temp : %12u / %u / %d\n",
                EC_READ_U16(pd + e.analog_voltage_1),
                EC_READ_U16(pd + e.analog_voltage_2),
                EC_READ_S16(pd + e.temperature));
    std::printf("          acc xyz      : %12d / %d / %d\n",
                EC_READ_S16(pd + e.acceleration_1),
                EC_READ_S16(pd + e.acceleration_2),
                EC_READ_S16(pd + e.acceleration_3));
    std::printf("          rs485 cnt/len: %12u / %u\n",
                EC_READ_U16(pd + e.rs485_inputs_count),
                EC_READ_U16(pd + e.rs485_inputs_len));

    std::printf("[主站与从站状态]\n");
    std::printf("  主站链路          : %12s\n", master_state.link_up ? "正常" : "断开");
    std::printf("  响应从站数        : %12u\n", master_state.slaves_responding);
    std::printf("  主站 AL 状态      :         0x%02X\n", master_state.al_states);
    std::printf("  从站在线 / OP     : %12zu / %zu\n", online_count, op_count);
    std::printf("  Domain 当前状态   :     wc=%u state=%u\n",
                domain_state.working_counter, domain_state.wc_state);
    for (size_t i = 0; i < kServoCount; ++i) {
        std::printf("  Servo %zu AL/online/OP:   0x%02X / %u / %u\n",
                    i + 1,
                    servo_states[i].al_state,
                    servo_states[i].online,
                    servo_states[i].operational);
    }
    std::printf("  EndIO %u AL/online/OP:   0x%02X / %u / %u\n",
                kEndIoLogicalId,
                endio_state.al_state,
                endio_state.online,
                endio_state.operational);
    std::printf("============================================================\n");
}

}  // namespace

namespace {

/*
 * 将公共接口中的调度策略转换成 Linux sched_*() 使用的常量。
 *
 * policy：调用方选定的普通、FIFO 或 RR 调度策略。
 */
int native_scheduling_policy(SchedulingPolicy policy) {
    switch (policy) {
    case SchedulingPolicy::Other:
        return SCHED_OTHER;
    case SchedulingPolicy::Fifo:
        return SCHED_FIFO;
    case SchedulingPolicy::RoundRobin:
        return SCHED_RR;
    }

    return SCHED_OTHER;
}

}  // namespace

/* 按配置设置调度策略、CPU 亲和性和内存锁定；失败不退出。 */
RealtimeSetupResult setup_realtime_process(const RealtimeConfig &config) {
    /* result：记录每项实时设置是否生效以及最终实际值。 */
    RealtimeSetupResult result {};

    /* native_policy：Linux sched_setscheduler() 接受的调度策略。 */
    const int native_policy = native_scheduling_policy(config.policy);

    /* minimum_priority：当前调度策略允许的最低优先级。 */
    const int minimum_priority = sched_get_priority_min(native_policy);

    /* maximum_priority：当前调度策略允许的最高优先级。 */
    const int maximum_priority = sched_get_priority_max(native_policy);

    /* requested_priority：-1 时选择当前策略的最高优先级。 */
    const int requested_priority =
        config.priority < 0 ? maximum_priority : config.priority;

    /* param：传给 sched_setscheduler() 的线程优先级参数。 */
    sched_param param {};
    param.sched_priority = requested_priority;

    if (minimum_priority == -1 || maximum_priority == -1 ||
        requested_priority < minimum_priority ||
        requested_priority > maximum_priority) {
        std::fprintf(stderr,
                     "warning: invalid scheduler priority %d, range=%d..%d\n",
                     requested_priority,
                     minimum_priority,
                     maximum_priority);
    } else if (sched_setscheduler(0, native_policy, &param) == -1) {
        std::fprintf(stderr, "warning: sched_setscheduler failed: %s\n",
                     std::strerror(errno));
    } else {
        result.scheduler_configured = true;
    }

    result.affinity_configured = config.cpu_id < 0;
    if (config.cpu_id >= 0) {
        /* cpu_set：只保留目标逻辑 CPU 的进程亲和性掩码。 */
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);

        if (config.cpu_id >= CPU_SETSIZE) {
            std::fprintf(stderr,
                         "warning: cpu id %d exceeds CPU_SETSIZE=%d\n",
                         config.cpu_id,
                         CPU_SETSIZE);
        } else {
            CPU_SET(config.cpu_id, &cpu_set);
            if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == -1) {
                std::fprintf(stderr,
                             "warning: sched_setaffinity CPU %d failed: %s\n",
                             config.cpu_id,
                             std::strerror(errno));
            } else {
                result.affinity_configured = true;
            }
        }
    }

    result.memory_locked = !config.lock_memory;
    if (config.lock_memory) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
            std::fprintf(stderr, "warning: mlockall failed: %s\n",
                         std::strerror(errno));
        } else {
            result.memory_locked = true;
        }
    }

    prefault_stack();

    /* actual_param：由操作系统返回的当前实际线程优先级。 */
    sched_param actual_param {};
    result.actual_policy = sched_getscheduler(0);
    if (sched_getparam(0, &actual_param) == 0) {
        result.actual_priority = actual_param.sched_priority;
    }
    result.actual_cpu = sched_getcpu();

    std::printf("[RT] requested policy=%d priority=%d cpu=%d mlock=%s\n",
                native_policy,
                requested_priority,
                config.cpu_id,
                config.lock_memory ? "on" : "off");
    std::printf("[RT] actual policy=%d priority=%d cpu=%d "
                "scheduler=%s affinity=%s mlock=%s\n",
                result.actual_policy,
                result.actual_priority,
                result.actual_cpu,
                result.scheduler_configured ? "ok" : "failed",
                result.affinity_configured ? "ok" : "failed",
                result.memory_locked ? "ok" : "failed");

    return result;
}

/*
 * 配置并激活 SINSUN EtherCAT 主站应用。
 *
 * app：主站运行期上下文，函数会填充 master/domain/slave config/pd 指针。
 * axis_config_directory：Axis1.xml-Axis6.xml 所在目录。
 */
int configure(App &app, const std::string &axis_config_directory) {
    /* app.master：请求到的 IgH master 句柄。 */
    app.master = ecrt_request_master(kMasterIndex);
    if (!app.master) {
        std::fprintf(stderr, "failed to request EtherCAT master %u\n",
                     kMasterIndex);
        return -1;
    }

    /* axis_parameters：从 Axis*.xml 读取出来的 6 轴伺服参数集合。 */
    AxisParameterSet axis_parameters;

    if (load_axis_parameter_set(axis_config_directory, axis_parameters)) {
        return -1;
    }

    if (write_axis_parameters(app.master, axis_parameters)) {
        return -1;
    }

    /* app.domain：包含 6 个伺服和 1 个末端 IO 的过程数据 domain。 */
    app.domain = ecrt_master_create_domain(app.master);
    if (!app.domain) {
        std::fprintf(stderr, "failed to create process data domain\n");
        return -1;
    }

    /* i：伺服数组下标，逐个配置 6 个伺服。 */
    for (size_t i = 0; i < kServoCount; ++i) {
        if (configure_servo(app, i)) {
            return -1;
        }
    }

    if (configure_endio(app) || register_domain_entries(app)) {
        return -1;
    }

    if (ecrt_master_select_reference_clock(app.master, app.servo_configs[0])) {
        std::fprintf(stderr, "failed to select Servo 1 as DC reference clock\n");
        return -1;
    }

    if (ecrt_master_activate(app.master)) {
        std::fprintf(stderr, "failed to activate EtherCAT master\n");
        return -1;
    }

    app.domain_pd = ecrt_domain_data(app.domain);
    if (!app.domain_pd) {
        std::fprintf(stderr, "failed to get domain process data\n");
        return -1;
    }

    /* 激活后先初始化一次输出 PDO 默认值。 */
    write_default_outputs(app);

    return 0;
}

/*
 * 运行 1 ms EtherCAT 周期通讯。
 *
 * app：主站运行期上下文，提供 master/domain/process data。
 * keep_running：信号处理函数会置 0，循环据此退出。
 */
void run(App &app,
         volatile std::sig_atomic_t &keep_running,
         const RunConfig &config) {
    /* wakeup：下一次周期唤醒的绝对时间。 */
    timespec wakeup {};

    clock_gettime(CLOCK_MONOTONIC, &wakeup);

    QualityStats quality_stats {};

    std::printf("starting 1 ms SINSUN EtherCAT communication loop\n");
    std::printf("topology: servo id 1-6, endio id 7, DC assign=0x%04X\n",
                kDcAssignActivate);
    if (config.enable_motion) {
        std::printf("motion: only Servo 6 enabled, amplitude=%d pulse, "
                    "peak_to_peak=%d pulse, period=%llu ms ramp=%llu ms\n",
                    kMotionAmplitudeCounts,
                    kMotionRangeCounts,
                    static_cast<unsigned long long>(kMotionPeriodCycles),
                    static_cast<unsigned long long>(kMotionRampCycles));
    } else {
        std::printf("motion: disabled; all RxPDO outputs remain zero\n");
    }
    std::printf("test options: max_cycles=%llu dc_sync_period=%llu "
                "report_period=%llu\n",
                static_cast<unsigned long long>(config.max_cycles),
                static_cast<unsigned long long>(
                    config.dc_sync_period_cycles),
                static_cast<unsigned long long>(config.report_period_cycles));

    /* cycle：1 ms 通讯周期计数，用于控制打印和 DC 同步节拍。 */
    uint64_t cycle = 0;
    ServoMotionControl motion_control {};

    while (keep_running) {
        add_ns(wakeup, kCycleTimeNs);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, nullptr);

        /* app_time：传给 IgH 的 application time，单位 ns。 */
        const uint64_t app_time = static_cast<uint64_t>(timespec_to_ns(wakeup));
        ecrt_master_application_time(app.master, app_time);

        ecrt_master_receive(app.master);
        ecrt_domain_process(app.domain);

        ec_domain_state_t domain_state {};
        const ec_domain_state_t *domain_state_ptr = nullptr;
        if (ecrt_domain_state(app.domain, &domain_state) == 0) {
            domain_state_ptr = &domain_state;
        }

        print_state_changes(app);

        quality_update_after_op(app, quality_stats, monotonic_raw_ns(),
                                domain_state_ptr);

        update_dc_monitor(app, quality_stats, cycle);

        if (config.enable_motion) {
            /* 正式控制模式按原逻辑刷新 Servo 6 的控制输出。 */
            write_cycle_outputs(app, motion_control);
        } else {
            /* 被动测试模式每周期保持全部输出为 0，防止意外使能或运动。 */
            write_default_outputs(app);
        }

        if (config.report_period_cycles > 0 &&
            cycle > 0 &&
            quality_stats.active &&
            quality_stats.cycles > 0 &&
            quality_stats.cycles % config.report_period_cycles == 0) {
            print_quality_report(app, quality_stats, false);
        }

        if (config.dc_sync_period_cycles > 0 &&
            cycle % config.dc_sync_period_cycles == 0) {
            ecrt_master_sync_reference_clock(app.master);
            ecrt_master_sync_slave_clocks(app.master);
        }

        queue_dc_monitor(app, quality_stats, cycle);

        ecrt_domain_queue(app.domain);
        ecrt_master_send(app.master);
        ++cycle;

        if (config.max_cycles > 0 && cycle >= config.max_cycles) {
            keep_running = 0;
        }
    }

    print_quality_report(app, quality_stats, true);
}

/*
 * 释放 IgH master。
 *
 * app：主站运行期上下文，函数只释放非空 master 句柄。
 */
void release(App &app) {
    if (app.master) {
        ecrt_release_master(app.master);
    }
}

}  // namespace sinsun
