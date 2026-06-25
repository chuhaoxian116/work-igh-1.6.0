#include "drive_pdo.h"

#include <stddef.h>

#include "app_config.h"

/*
 * 主站写入的 RxPDO entries。
 *
 * 这里按 GSD620 当前 "ethercat cstruct" 输出配置：
 *   0x607A target position
 *   0x60FF target velocity
 *   0x6040 control word
 *   0x6071 target torque
 *   0x6060 operation mode
 *
 * 当前程序只写 target position、control word、operation mode，其余 entry
 * 保持映射但不主动控制，process data 初始值保持为 0。
 */
static const ec_pdo_entry_info_t drive_rxpdo_entries[] = {
    {0x607A, 0x00, 32}, {0x60FF, 0x00, 32}, {0x6040, 0x00, 16},
    {0x6071, 0x00, 16}, {0x6060, 0x00, 8},
};

/*
 * 主站读取的 TxPDO entries。
 *
 * 当前程序只读取 actual position、status word、operation mode display，
 * 其余 entry 保持映射，后面需要诊断速度/转矩/错误码时可以直接扩展。
 */
static const ec_pdo_entry_info_t drive_txpdo_entries[] = {
    {0x6064, 0x00, 32}, {0x603F, 0x00, 16}, {0x606C, 0x00, 32},
    {0x6041, 0x00, 16}, {0x6077, 0x00, 16}, {0x6061, 0x00, 8},
};

/* 将 RxPDO 分配到 SM2，将 TxPDO 分配到 SM3。 */
static const ec_pdo_info_t drive_pdos[] = {
    {DRIVE_RXPDO_INDEX, 5, drive_rxpdo_entries},
    {DRIVE_TXPDO_INDEX, 6, drive_txpdo_entries},
};

/*
 * GSD620 当前 SyncManager 布局，来自 "ethercat cstruct"。
 *
 * SM0/SM1 是邮箱，SM2 是输出 PDO，SM3 是输入 PDO。
 */
static const ec_sync_info_t drive_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
    {1, EC_DIR_INPUT, 0, NULL, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, &drive_pdos[0], EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, &drive_pdos[1], EC_WD_DISABLE},
    {0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT},
};

const ec_sync_info_t *drive_pdo_syncs(void) {
    return drive_syncs;
}

void drive_pdo_make_domain_regs(ec_pdo_entry_reg_t regs[DRIVE_PDO_REG_COUNT + 1],
                                drive_pdo_offsets_t *offsets) {
    /*
     * IgH 会在注册时填充 offset 指针。最后一个全 0 元素是列表结束标记，
     * 不能省略。
     */
    regs[0] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,           DRIVE_PRODUCT_CODE,
        0x607A,      0x00,           &offsets->target_position, NULL,
    };
    regs[1] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,        DRIVE_PRODUCT_CODE,
        0x6040,      0x00,           &offsets->control_word, NULL,
    };
    regs[2] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,          DRIVE_PRODUCT_CODE,
        0x6060,      0x00,           &offsets->operation_mode, NULL,
    };
    regs[3] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,           DRIVE_PRODUCT_CODE,
        0x6064,      0x00,           &offsets->actual_position, NULL,
    };
    regs[4] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,       DRIVE_PRODUCT_CODE,
        0x6041,      0x00,           &offsets->status_word, NULL,
    };
    regs[5] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS,
        DRIVE_POSITION,
        DRIVE_VENDOR_ID,
        DRIVE_PRODUCT_CODE,
        0x6061,
        0x00,
        &offsets->operation_mode_display,
        NULL,
    };
    regs[6] = (ec_pdo_entry_reg_t){0, 0, 0, 0, 0, 0, NULL, NULL};
}

drive_inputs_t drive_pdo_read_inputs(const uint8_t *domain_pd,
                                     const drive_pdo_offsets_t *offsets) {
    drive_inputs_t inputs;

    /* 使用 IgH 读写宏，避免字节序和有符号类型处理出错。 */
    inputs.actual_position = EC_READ_S32(domain_pd + offsets->actual_position);
    inputs.status_word = EC_READ_U16(domain_pd + offsets->status_word);
    inputs.operation_mode_display =
        EC_READ_S8(domain_pd + offsets->operation_mode_display);

    return inputs;
}

void drive_pdo_write_outputs(uint8_t *domain_pd, const drive_pdo_offsets_t *offsets,
                             const drive_outputs_t *outputs) {
    /* 当前还没有加入 CiA 402 控制流程，所以输出值暂时保持默认 0。 */
    EC_WRITE_S32(domain_pd + offsets->target_position, outputs->target_position);
    EC_WRITE_U16(domain_pd + offsets->control_word, outputs->control_word);
    EC_WRITE_S8(domain_pd + offsets->operation_mode, outputs->operation_mode);
}
