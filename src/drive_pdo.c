#include "igh_master/drive_pdo.h"

#include <stddef.h>

#include "igh_master/app_config.h"

static const ec_pdo_entry_info_t drive_rxpdo_entries[] = {
    {0x607A, 0x00, 32},
    {0x6040, 0x00, 16},
    {0x6060, 0x00, 8},
};

static const ec_pdo_entry_info_t drive_txpdo_entries[] = {
    {0x6064, 0x00, 32},
    {0x6041, 0x00, 16},
    {0x6061, 0x00, 8},
};

static const ec_pdo_info_t drive_pdos[] = {
    {DRIVE_RXPDO_INDEX, 3, drive_rxpdo_entries},
    {DRIVE_TXPDO_INDEX, 3, drive_txpdo_entries},
};

static const ec_sync_info_t drive_syncs[] = {
    {2, EC_DIR_OUTPUT, 1, &drive_pdos[0], EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, &drive_pdos[1], EC_WD_DISABLE},
    {0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT}
};

const ec_sync_info_t *drive_pdo_syncs(void)
{
    return drive_syncs;
}

void drive_pdo_make_domain_regs(
    ec_pdo_entry_reg_t regs[DRIVE_PDO_REG_COUNT + 1],
    drive_pdo_offsets_t *offsets)
{
    regs[0] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x607A, 0x00, &offsets->target_position, NULL
    };
    regs[1] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x6040, 0x00, &offsets->control_word, NULL
    };
    regs[2] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x6060, 0x00, &offsets->operation_mode, NULL
    };
    regs[3] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x6064, 0x00, &offsets->actual_position, NULL
    };
    regs[4] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x6041, 0x00, &offsets->status_word, NULL
    };
    regs[5] = (ec_pdo_entry_reg_t){
        DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE,
        0x6061, 0x00, &offsets->operation_mode_display, NULL
    };
    regs[6] = (ec_pdo_entry_reg_t){0, 0, 0, 0, 0, 0, NULL, NULL};
}

drive_inputs_t drive_pdo_read_inputs(
    const uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets)
{
    drive_inputs_t inputs;

    inputs.actual_position =
        EC_READ_S32(domain_pd + offsets->actual_position);
    inputs.status_word =
        EC_READ_U16(domain_pd + offsets->status_word);
    inputs.operation_mode_display =
        EC_READ_S8(domain_pd + offsets->operation_mode_display);

    return inputs;
}

void drive_pdo_write_outputs(
    uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets,
    const drive_outputs_t *outputs)
{
    EC_WRITE_S32(domain_pd + offsets->target_position,
        outputs->target_position);
    EC_WRITE_U16(domain_pd + offsets->control_word,
        outputs->control_word);
    EC_WRITE_S8(domain_pd + offsets->operation_mode,
        outputs->operation_mode);
}
