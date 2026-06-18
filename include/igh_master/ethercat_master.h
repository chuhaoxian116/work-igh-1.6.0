#ifndef IGH_MASTER_ETHERCAT_MASTER_H
#define IGH_MASTER_ETHERCAT_MASTER_H

#include <signal.h>
#include <stdint.h>

#include "ecrt.h"
#include "igh_master/drive_pdo.h"

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
} ethercat_master_app_t;

void ethercat_master_app_init(ethercat_master_app_t *app);
int ethercat_master_app_configure(ethercat_master_app_t *app);
void ethercat_master_app_run(
    ethercat_master_app_t *app,
    volatile sig_atomic_t *keep_running);
void ethercat_master_app_release(ethercat_master_app_t *app);

#endif
