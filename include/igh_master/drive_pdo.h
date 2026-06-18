#ifndef IGH_MASTER_DRIVE_PDO_H
#define IGH_MASTER_DRIVE_PDO_H

/*
 * CiA 402 伺服 PDO 接口。
 *
 * 本模块负责 PDO 布局、domain 注册表，以及 1 ms EtherCAT 循环中使用的
 * 类型化 PDO 读写函数。
 */

#include <stdint.h>

#include "ecrt.h"

/* 共 6 个 PDO entry：3 个输出，3 个输入。 */
#define DRIVE_PDO_REG_COUNT 6

/* 主站写入伺服 RxPDO 的数据。 */
typedef struct {
    int32_t target_position; /* 0x607A:00 目标位置 */
    uint16_t control_word;   /* 0x6040:00 控制字 */
    int8_t operation_mode;   /* 0x6060:00 操作模式 */
} drive_outputs_t;

/* 主站从伺服 TxPDO 读取的数据。 */
typedef struct {
    int32_t actual_position;       /* 0x6064:00 实际位置 */
    uint16_t status_word;          /* 0x6041:00 状态字 */
    int8_t operation_mode_display; /* 0x6061:00 操作模式显示 */
} drive_inputs_t;

/*
 * IgH 在 domain 注册时分配的字节偏移。
 *
 * 这些偏移只有在 ecrt_domain_reg_pdo_entry_list() 成功后才有效，用于访问
 * process data buffer 中的具体 PDO entry。
 */
typedef struct {
    unsigned int target_position;
    unsigned int control_word;
    unsigned int operation_mode;
    unsigned int actual_position;
    unsigned int status_word;
    unsigned int operation_mode_display;
} drive_pdo_offsets_t;

/* 传给 IgH 的 SyncManager 和 PDO 映射描述。 */
const ec_sync_info_t *drive_pdo_syncs(void);

/* 生成 domain 注册列表，并把每个 PDO entry 绑定到对应的 offset 字段。 */
void drive_pdo_make_domain_regs(
    ec_pdo_entry_reg_t regs[DRIVE_PDO_REG_COUNT + 1],
    drive_pdo_offsets_t *offsets);

/* 从 process data buffer 读取全部 TxPDO entry。 */
drive_inputs_t drive_pdo_read_inputs(
    const uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets);

/* 向 process data buffer 写入全部 RxPDO entry。 */
void drive_pdo_write_outputs(
    uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets,
    const drive_outputs_t *outputs);

#endif
