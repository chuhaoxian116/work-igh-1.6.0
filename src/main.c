#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "igh_master/app_config.h"
#include "igh_master/ethercat_master.h"

#define MAX_SAFE_STACK (8 * 1024)

static volatile sig_atomic_t keep_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void prefault_stack(void)
{
    volatile unsigned char dummy[MAX_SAFE_STACK];
    size_t i;

    for (i = 0; i < sizeof(dummy); ++i) {
        dummy[i] = 0;
    }
}

static void setup_realtime_process(void)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        fprintf(stderr, "warning: sched_setscheduler failed: %s\n",
            strerror(errno));
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        fprintf(stderr, "warning: mlockall failed: %s\n", strerror(errno));
    }

    prefault_stack();
}

int main(void)
{
    ethercat_master_app_t app;

    install_signal_handlers();
    ethercat_master_app_init(&app);

    if (ethercat_master_app_configure(&app)) {
        ethercat_master_app_release(&app);
        return 1;
    }

    setup_realtime_process();

    printf("starting 1 ms EtherCAT loop, printing every 5 s\n");
    ethercat_master_app_run(&app, &keep_running);

    ethercat_master_app_release(&app);
    printf("stopped\n");

    return 0;
}
