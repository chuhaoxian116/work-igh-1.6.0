#ifndef SINSUN_ETHERCAT_APP_H
#define SINSUN_ETHERCAT_APP_H

#include <array>
#include <csignal>
#include <cstdint>
#include <string>

#include "app_config.h"
#include "ecrt.h"
#include "pdo_config.h"

namespace sinsun {

/* SINSUN 主站运行期上下文，集中保存 IgH 句柄、PDO 偏移和状态快照。 */
struct App {
    /* IgH master 句柄，由 ecrt_request_master() 返回。 */
    ec_master_t *master = nullptr;
    /* IgH domain 句柄，用于管理所有从站的过程数据。 */
    ec_domain_t *domain = nullptr;
    /* 6 个伺服从站的配置句柄，数组下标 0-5 对应逻辑 id 1-6。 */
    std::array<ec_slave_config_t *, kServoCount> servo_configs{};
    /* 末端 IO 从站配置句柄，逻辑 id 为 7。 */
    ec_slave_config_t *endio_config = nullptr;
    /* 激活 master 后取得的 domain process data 起始地址。 */
    uint8_t *domain_pd = nullptr;
    /* 6 个伺服需要读取和打印的 PDO entry 偏移。 */
    std::array<ServoOffsets, kServoCount> servo_offsets{};
    /* 末端 IO 需要读取和打印的 PDO entry 偏移。 */
    EndIoOffsets endio_offsets{};
    /* 上一次打印过的 master 状态，用于只在状态变化时打印。 */
    ec_master_state_t master_state{};
    /* 上一次打印过的 domain 状态，用于观察 working counter 变化。 */
    ec_domain_state_t domain_state{};
    /* 上一次打印过的伺服状态，数组下标 0-5 对应逻辑 id 1-6。 */
    std::array<ec_slave_config_state_t, kServoCount> servo_states{};
    /* 上一次打印过的末端 IO 状态。 */
    ec_slave_config_state_t endio_state{};
};

/* 先写入 Axis*.xml 中的 SDO 参数，再配置 PDO/DC 并激活 master。 */
int configure(App &app, const std::string &axis_config_directory);

/* 尽力进入实时调度并锁定内存，失败时只打印 warning。 */
void setup_realtime_process();

/* 执行 1 ms 周期通讯循环，直到 keep_running 被信号处理函数置 0。 */
void run(App &app, volatile std::sig_atomic_t &keep_running);

/* 释放 IgH master 资源。 */
void release(App &app);

}  // namespace sinsun

#endif
