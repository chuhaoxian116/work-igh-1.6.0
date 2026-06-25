#include <csignal>
#include <cstdio>

#include "ethercat_app.h"

namespace {

/* 运行标志；信号处理函数置 0 后，1 ms 通讯循环会自然退出。 */
volatile std::sig_atomic_t keep_running = 1;

/*
 * 进程信号处理函数。
 *
 * 收到 SIGINT/SIGTERM 时只修改 sig_atomic_t 标志，避免在信号上下文做复杂操作。
 */
void signal_handler(int) {
    keep_running = 0;
}

/* 安装 Ctrl+C 和进程终止信号处理，保证主站循环可以干净退出。 */
void install_signal_handlers() {
    /* sa：POSIX sigaction 配置结构体，指定 signal_handler 作为回调。 */
    struct sigaction sa {};

    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace

/* SINSUN 主站程序入口。 */
int main() {
    /* app：SINSUN EtherCAT 主站运行期上下文。 */
    sinsun::App app;

    install_signal_handlers();

    if (sinsun::configure(app)) {
        sinsun::release(app);
        return 1;
    }

    sinsun::setup_realtime_process();
    sinsun::run(app, keep_running);
    sinsun::release(app);

    std::printf("stopped\n");
    return 0;
}
