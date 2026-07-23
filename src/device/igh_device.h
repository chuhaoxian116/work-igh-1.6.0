#ifndef IGH_DEVICE_H
#define IGH_DEVICE_H

#include <cstdint>

#include "ecrt.h"

namespace device {

/**
 * @brief 主站传给设备适配器的配置上下文。
 *
 * 设备在 Configure() 中完成从站身份匹配、PDO 映射、PDO entry 注册、
 * DC 配置以及必要的激活前 SDO 参数写入。
 */
struct DeviceConfiguration {
    ec_master_t *master = nullptr;       // 当前 IgH master。
    ec_domain_t *domain = nullptr;       // 当前主站管理的唯一 PDO domain。
    uint32_t cycle_time_ns = 0;          // 标称 EtherCAT 周期，单位为纳秒。
};

/**
 * @brief IgH 从站设备适配器的公共基类。
 *
 * 主站只管理 EtherCAT 周期与 Domain；具体设备类负责自身的 PDO、DC、
 * SDO 和过程数据映射。业务层只需要派生此类实现具体设备，然后将
 * std::unique_ptr<IghDevice> 交给 IghMaster 管理。
 *
 * 所有设备必须在 IghMaster::Configure() 前注册；实时循环中不允许新增、
 * 删除或替换设备。
 */
class IghDevice {
public:
    virtual ~IghDevice() = default;

    IghDevice(const IghDevice &) = delete;
    IghDevice &operator=(const IghDevice &) = delete;

    /**
     * @brief 配置设备的从站、PDO、DC 和 PDO entry 注册。
     *
     * 该函数仅在 master 激活前调用一次；返回 false 时主站配置失败。
     */
    virtual bool Configure(const DeviceConfiguration &configuration) = 0;

    /**
     * @brief 返回 Configure() 中获得的从站配置，用于选择 DC 参考时钟。
     */
    virtual ec_slave_config_t *slave_config() const = 0;

    /**
     * @brief 将已处理的 TxPDO 从 domain process data 映射到设备状态。
     *
     * 禁止在实时周期内分配内存、加锁、打印或进行 mailbox 通讯。
     */
    virtual void ReadProcessData(const uint8_t *domain_pd) noexcept = 0;

    /**
     * @brief 将设备命令映射到 domain process data 的 RxPDO 区域。
     *
     * 禁止在实时周期内分配内存、加锁、打印或进行 mailbox 通讯。
     */
    virtual void WriteProcessData(uint8_t *domain_pd) noexcept = 0;

    /**
     * @brief 在 IghMaster 释放底层 master 前清除设备保存的配置状态。
     *
     * 派生类可在此重置 slave_config、PDO offset、通信状态等由 Configure()
     * 建立的数据。默认实现为空，适用于未保存此类状态的设备。
     */
    virtual void Reset() noexcept {}

protected:
    IghDevice() = default;
};

}  // namespace device

#endif
