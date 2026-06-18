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
#define DRIVE_VENDOR_ID 0x00000000u
#define DRIVE_PRODUCT_CODE 0x00000000u

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

#endif
