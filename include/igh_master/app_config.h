#ifndef IGH_MASTER_APP_CONFIG_H
#define IGH_MASTER_APP_CONFIG_H

/*
 * 工程集中配置。
 *
 * 和硬件相关的参数统一放在这里，避免 EtherCAT 主站逻辑和 PDO 映射代码
 * 到处散落设备参数。更换伺服或调整站号时，优先检查这个文件。
 */

/*
 * 从站身份信息。
 *
 * 上机前需要按实际设备修改。通常可以用下面命令读取：
 *   ethercat slaves -v
 */
#define MASTER_INDEX 0
#define DRIVE_ALIAS 0
#define DRIVE_POSITION 0
#define DRIVE_VENDOR_ID 0x00000911u
#define DRIVE_PRODUCT_CODE 0x00000620u

/*
 * 通用 CiA 402 PDO 布局：
 *   SM2 output：RxPDO 0x1600
 *   SM3 input ：TxPDO 0x1A00
 *
 * 如果伺服 ESI 或 "ethercat pdos" 显示的 PDO assignment object 不同，
 * 需要同步修改这里的 PDO 编号。
 */
#define DRIVE_RXPDO_INDEX 0x1600
#define DRIVE_TXPDO_INDEX 0x1A00

/*
 * 1 ms EtherCAT 通讯周期，使用 DC SYNC0。
 *
 * DC_ASSIGN_ACTIVATE 和厂家有关。0x0300 是 SYNC0-only CiA 402 伺服里
 * 较常见的值，但实际值应以 ESI XML 中的 Device -> Dc -> AssignActivate
 * 为准。
 */
#define CYCLE_TIME_NS 1000000u
#define DC_ASSIGN_ACTIVATE 0x0300u
#define DC_SYNC0_SHIFT_NS 0

/* 循环任务中的诊断打印周期。 */
#define PRINT_PERIOD_NS 5000000000ULL

/*
 * 控制功能开关。
 *
 * 置 1 时，程序会执行 0x06 -> 0x07 -> 0x15 使能序列，并在使能完成后
 * 进入正弦位置给定。置 0 时只做 PDO 收发和调试打印。
 */
#define ENABLE_DRIVE_CONTROL 1

/* 当前使用 8：Cyclic Synchronous Position，和现场反馈 mode display 一致。 */
#define DRIVE_OPERATION_MODE 8

/* 每个使能控制字保持的周期数。1000 个 1 ms 周期即 1 秒。 */
#define ENABLE_STEP_CYCLES 1000ULL

/*
 * CSP 运动参数（SOEM 工程中有同名配置，修改时请保持两边一致）。
 *
 * MOTION_RANGE_COUNTS：相对起始位置的最大行程，单位 pulse。
 * MOTION_PEAK_RATE_COUNTS_PER_SEC：目标位置的峰值变化率，单位 pulse/s。
 *
 * 当前轨迹是平滑半余弦往复：base -> base + range -> base。轨迹周期由
 * “行程”和“峰值速度”自动推导，无需手工换算；1 ms 周期下，50000
 * pulse/s 对应峰值约 50 pulse/cycle。
 */
#define MOTION_RANGE_COUNTS 100000
#define MOTION_PEAK_RATE_COUNTS_PER_SEC 50000

/* pi * 1e9，用整数运算推导半余弦轨迹周期，避免配置文件依赖 libm。 */
#define MOTION_PI_SCALED 3141592654ULL
#define MOTION_PERIOD_NS                                                    \
    ((MOTION_PI_SCALED * MOTION_RANGE_COUNTS) /                           \
     MOTION_PEAK_RATE_COUNTS_PER_SEC)

/*
 * 通信质量报告周期。
 *
 * 运行期间每 60 秒打印一次累计通信质量；Ctrl+C 退出时再打印完整总报告。
 */
#define COMMUNICATION_REPORT_PERIOD_NS 60000000000ULL

/* 实际周期超过标称周期的 150% 时，记为一次严重超周期。 */
#define CYCLE_OVERRUN_LIMIT_NS ((CYCLE_TIME_NS * 3ULL) / 2ULL)

/* DC 同步监视周期，使用 IgH sync monitor，单位 ns。 */
#define DC_MONITOR_PERIOD_NS 1000000000ULL

/* 调试线程打印周期，单位 ns。 */
#define DEBUG_PRINT_PERIOD_NS 1000000000ULL

/*
 * 是否在调试线程中通过 SDO 慢速读取伺服同步丢失计数 0x2005:35。
 *
 * SDO upload 是阻塞调用，不会放在 1 ms 周期线程里。默认关闭；需要和伺服
 * 软件里的“同步丢失计数”对比时再打开。
 */
#define DEBUG_READ_SYNC_LOST_SDO 0
#define SYNC_LOST_COUNT_INDEX 0x2005
#define SYNC_LOST_COUNT_SUBINDEX 0x35

#endif
