#include "igh_master/ethercat_master.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "igh_master/app_config.h"

#define NSEC_PER_SEC 1000000000LL
#define PRINT_PERIOD_CYCLES (PRINT_PERIOD_NS / CYCLE_TIME_NS)

static int64_t timespec_to_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static void add_ns(struct timespec *ts, int64_t ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= NSEC_PER_SEC) {
        ts->tv_nsec -= NSEC_PER_SEC;
        ts->tv_sec++;
    }
}

static void check_master_state(ethercat_master_app_t *app)
{
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

static void check_domain_state(ethercat_master_app_t *app)
{
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

static void check_drive_state(ethercat_master_app_t *app)
{
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

static void print_drive_data(const ethercat_master_app_t *app, uint64_t cycle)
{
    drive_inputs_t inputs =
        drive_pdo_read_inputs(app->domain_pd, &app->pdo_offsets);

    printf("cycle=%llu target_position=%d control_word=0x%04X "
           "operation_mode=%d actual_position=%d status_word=0x%04X "
           "operation_mode_display=%d\n",
           (unsigned long long)cycle,
           app->outputs.target_position,
           app->outputs.control_word,
           app->outputs.operation_mode,
           inputs.actual_position,
           inputs.status_word,
           inputs.operation_mode_display);
}

void ethercat_master_app_init(ethercat_master_app_t *app)
{
    memset(app, 0, sizeof(*app));
}

int ethercat_master_app_configure(ethercat_master_app_t *app)
{
    ec_pdo_entry_reg_t domain_regs[DRIVE_PDO_REG_COUNT + 1];

    app->master = ecrt_request_master(MASTER_INDEX);
    if (!app->master) {
        fprintf(stderr, "failed to request EtherCAT master %u\n",
            MASTER_INDEX);
        return -1;
    }

    app->domain = ecrt_master_create_domain(app->master);
    if (!app->domain) {
        fprintf(stderr, "failed to create process data domain\n");
        return -1;
    }

    app->drive_config = ecrt_master_slave_config(app->master, DRIVE_ALIAS,
        DRIVE_POSITION, DRIVE_VENDOR_ID, DRIVE_PRODUCT_CODE);
    if (!app->drive_config) {
        fprintf(stderr, "failed to create drive slave config at %u:%u "
                "(vendor=0x%08X product=0x%08X)\n",
                DRIVE_ALIAS, DRIVE_POSITION, DRIVE_VENDOR_ID,
                DRIVE_PRODUCT_CODE);
        return -1;
    }

    printf("configuring PDOs...\n");
    if (ecrt_slave_config_pdos(app->drive_config, EC_END,
            drive_pdo_syncs())) {
        fprintf(stderr, "failed to configure drive PDOs\n");
        return -1;
    }

    drive_pdo_make_domain_regs(domain_regs, &app->pdo_offsets);
    if (ecrt_domain_reg_pdo_entry_list(app->domain, domain_regs)) {
        fprintf(stderr, "failed to register PDO entries\n");
        return -1;
    }

    if (ecrt_master_select_reference_clock(app->master, app->drive_config)) {
        fprintf(stderr, "failed to select DC reference clock\n");
        return -1;
    }

    if (ecrt_slave_config_dc(app->drive_config, DC_ASSIGN_ACTIVATE,
            CYCLE_TIME_NS, DC_SYNC0_SHIFT_NS, 0, 0)) {
        fprintf(stderr, "failed to configure distributed clocks\n");
        return -1;
    }

    printf("activating master...\n");
    if (ecrt_master_activate(app->master)) {
        fprintf(stderr, "failed to activate master\n");
        return -1;
    }

    app->domain_pd = ecrt_domain_data(app->domain);
    if (!app->domain_pd) {
        fprintf(stderr, "failed to get domain data pointer\n");
        return -1;
    }

    return 0;
}

void ethercat_master_app_run(
    ethercat_master_app_t *app,
    volatile sig_atomic_t *keep_running)
{
    struct timespec wakeup_time;
    uint64_t cycle = 0;

    clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
    add_ns(&wakeup_time, CYCLE_TIME_NS);

    while (*keep_running) {
        int ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
            &wakeup_time, NULL);
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

        if (cycle % PRINT_PERIOD_CYCLES == 0) {
            check_master_state(app);
            check_domain_state(app);
            check_drive_state(app);
            print_drive_data(app, cycle);
        }

        drive_pdo_write_outputs(app->domain_pd, &app->pdo_offsets,
            &app->outputs);

        ecrt_master_sync_reference_clock(app->master);
        ecrt_master_sync_slave_clocks(app->master);

        ecrt_domain_queue(app->domain);
        ecrt_master_send(app->master);

        cycle++;
        add_ns(&wakeup_time, CYCLE_TIME_NS);
    }
}

void ethercat_master_app_release(ethercat_master_app_t *app)
{
    if (app->master) {
        ecrt_release_master(app->master);
    }

    ethercat_master_app_init(app);
}
