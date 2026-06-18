#ifndef IGH_MASTER_DRIVE_PDO_H
#define IGH_MASTER_DRIVE_PDO_H

#include <stdint.h>

#include "ecrt.h"

#define DRIVE_PDO_REG_COUNT 6

typedef struct {
    int32_t target_position; /* 0x607A:00 target position */
    uint16_t control_word;   /* 0x6040:00 control word */
    int8_t operation_mode;   /* 0x6060:00 modes of operation */
} drive_outputs_t;

typedef struct {
    int32_t actual_position;       /* 0x6064:00 position actual value */
    uint16_t status_word;          /* 0x6041:00 status word */
    int8_t operation_mode_display; /* 0x6061:00 modes of operation display */
} drive_inputs_t;

typedef struct {
    unsigned int target_position;
    unsigned int control_word;
    unsigned int operation_mode;
    unsigned int actual_position;
    unsigned int status_word;
    unsigned int operation_mode_display;
} drive_pdo_offsets_t;

const ec_sync_info_t *drive_pdo_syncs(void);

void drive_pdo_make_domain_regs(
    ec_pdo_entry_reg_t regs[DRIVE_PDO_REG_COUNT + 1],
    drive_pdo_offsets_t *offsets);

drive_inputs_t drive_pdo_read_inputs(
    const uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets);

void drive_pdo_write_outputs(
    uint8_t *domain_pd,
    const drive_pdo_offsets_t *offsets,
    const drive_outputs_t *outputs);

#endif
