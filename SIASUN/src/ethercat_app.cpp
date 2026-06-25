#include "ethercat_app.h"

#include <cerrno>
#include <cstdio>
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

    ecrt_slave_config_dc(app.endio_config, kDcAssignActivate, kCycleTimeNs, 0, 0, 0);
    return 0;
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
void add_reg(std::array<ec_pdo_entry_reg_t, kServoCount * 8 + 11> &regs,
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
    std::array<ec_pdo_entry_reg_t, kServoCount * 8 + 11> regs {};

    /* reg_index：regs 当前写入下标。 */
    size_t reg_index = 0;

    /* i：伺服数组下标，0-5 对应用户侧 id 1-6。 */
    for (size_t i = 0; i < kServoCount; ++i) {
        /* pos：IgH 从站位置，和当前伺服数组下标一致。 */
        const uint16_t pos = static_cast<uint16_t>(i);

        /* o：当前伺服的 PDO offset 集合，注册成功后由 IgH 填充。 */
        ServoOffsets &o = app.servo_offsets[i];

        add_reg(regs, reg_index, pos, kServoProductCode, 0x6064, 0x00,
                &o.actual_position);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x60FD, 0x00,
                &o.digital_input);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x603F, 0x00,
                &o.error_code);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x606C, 0x00,
                &o.actual_velocity);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6041, 0x00,
                &o.status_word);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6077, 0x00,
                &o.actual_torque);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x6061, 0x00,
                &o.operation_mode_display);
        add_reg(regs, reg_index, pos, kServoProductCode, 0x600B, 0x00,
                &o.sync0_time_difference);
    }

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

    regs[reg_index] = ec_pdo_entry_reg_t{};

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
        std::memcmp(&master_state, &app.master_state, sizeof(master_state)) != 0) {
        std::printf("master: slaves=%u al_states=0x%02X link=%s\n",
                    master_state.slaves_responding,
                    master_state.al_states,
                    master_state.link_up ? "up" : "down");
        app.master_state = master_state;
    }

    /* domain_state：本周期读取到的 domain working-counter 状态。 */
    ec_domain_state_t domain_state {};
    if (ecrt_domain_state(app.domain, &domain_state) == 0 &&
        std::memcmp(&domain_state, &app.domain_state, sizeof(domain_state)) != 0) {
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
            std::memcmp(&state, &app.servo_states[i], sizeof(state)) != 0) {
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
        std::memcmp(&endio_state, &app.endio_state, sizeof(endio_state)) != 0) {
        std::printf("endio %u: al_state=0x%02X online=%u operational=%u\n",
                    kEndIoLogicalId,
                    endio_state.al_state,
                    endio_state.online,
                    endio_state.operational);
        app.endio_state = endio_state;
    }
}

/*
 * 打印当前过程数据快照。
 *
 * app：主站运行期上下文，提供 process data 地址和 PDO offset。
 * cycle：当前 1 ms 通讯周期计数。
 */
void print_process_data(const App &app, uint64_t cycle) {
    std::printf("\ncycle=%llu\n", static_cast<unsigned long long>(cycle));

    /* i：伺服数组下标，0-5 对应用户侧 id 1-6。 */
    for (size_t i = 0; i < kServoCount; ++i) {
        /* o：当前伺服的 PDO offset 集合。 */
        const ServoOffsets &o = app.servo_offsets[i];

        /* pd：domain process data 起始地址，用 offset 定位每个 PDO entry。 */
        const uint8_t *pd = app.domain_pd;

        std::printf(
            "servo %zu: pos=%d vel=%d torque=%d status=0x%04X mode=%d "
            "err=0x%08X din=0x%08X sync0_diff=%u\n",
            i + 1,
            EC_READ_S32(pd + o.actual_position),
            EC_READ_S32(pd + o.actual_velocity),
            EC_READ_S16(pd + o.actual_torque),
            EC_READ_U16(pd + o.status_word),
            EC_READ_S8(pd + o.operation_mode_display),
            EC_READ_U32(pd + o.error_code),
            EC_READ_U32(pd + o.digital_input),
            EC_READ_U16(pd + o.sync0_time_difference));
    }

    /* e：末端 IO 的 PDO offset 集合。 */
    const EndIoOffsets &e = app.endio_offsets;

    /* pd：domain process data 起始地址，用 offset 定位末端 IO PDO entry。 */
    const uint8_t *pd = app.domain_pd;

    std::printf(
        "endio %u: err=0x%02X din=0x%02X ai1=%u ai2=%u temp=%d "
        "acc=(%d,%d,%d) rs485_count=%u rs485_len=%u\n",
        kEndIoLogicalId,
        EC_READ_U8(pd + e.error_code),
        EC_READ_U8(pd + e.digital_inputs),
        EC_READ_U16(pd + e.analog_voltage_1),
        EC_READ_U16(pd + e.analog_voltage_2),
        EC_READ_S16(pd + e.temperature),
        EC_READ_S16(pd + e.acceleration_1),
        EC_READ_S16(pd + e.acceleration_2),
        EC_READ_S16(pd + e.acceleration_3),
        EC_READ_U16(pd + e.rs485_inputs_count),
        EC_READ_U16(pd + e.rs485_inputs_len));
}

}  // namespace

/* 尽力设置实时调度和内存锁定；失败不退出，只打印 warning。 */
void setup_realtime_process() {
    /* param：SCHED_FIFO 调度参数，使用系统允许的最高优先级。 */
    sched_param param {};

    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        std::fprintf(stderr, "warning: sched_setscheduler failed: %s\n",
                     std::strerror(errno));
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        std::fprintf(stderr, "warning: mlockall failed: %s\n",
                     std::strerror(errno));
    }

    prefault_stack();
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

    if (ecrt_master_activate(app.master)) {
        std::fprintf(stderr, "failed to activate EtherCAT master\n");
        return -1;
    }

    app.domain_pd = ecrt_domain_data(app.domain);
    if (!app.domain_pd) {
        std::fprintf(stderr, "failed to get domain process data\n");
        return -1;
    }

    return 0;
}

/*
 * 运行 1 ms EtherCAT 周期通讯。
 *
 * app：主站运行期上下文，提供 master/domain/process data。
 * keep_running：信号处理函数会置 0，循环据此退出。
 */
void run(App &app, volatile std::sig_atomic_t &keep_running) {
    /* wakeup：下一次周期唤醒的绝对时间。 */
    timespec wakeup {};

    clock_gettime(CLOCK_MONOTONIC, &wakeup);

    std::printf("starting 1 ms SINSUN EtherCAT communication loop\n");
    std::printf("topology: servo id 1-6, endio id 7, DC assign=0x%04X\n",
                kDcAssignActivate);

    /* cycle：1 ms 通讯周期计数，用于控制打印和 DC 同步节拍。 */
    uint64_t cycle = 0;

    while (keep_running) {
        add_ns(wakeup, kCycleTimeNs);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup, nullptr);

        /* app_time：传给 IgH 的 application time，单位 ns。 */
        const uint64_t app_time = static_cast<uint64_t>(timespec_to_ns(wakeup));
        ecrt_master_application_time(app.master, app_time);

        ecrt_master_receive(app.master);
        ecrt_domain_process(app.domain);

        print_state_changes(app);

        if (cycle % kPrintPeriodCycles == 0) {
            print_process_data(app, cycle);
        }

        if (cycle % kDcSyncPeriodCycles == 0) {
            ecrt_master_sync_reference_clock(app.master);
            ecrt_master_sync_slave_clocks(app.master);
        }

        ecrt_domain_queue(app.domain);
        ecrt_master_send(app.master);
        ++cycle;
    }
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
