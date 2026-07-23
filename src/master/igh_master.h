#ifndef IGH_MASTER_IGH_MASTER_H
#define IGH_MASTER_IGH_MASTER_H

#include <cstdint>
#include <memory>
#include <vector>

#include "device/igh_device.h"

namespace master {

/** @brief IgH 主站对象的生命周期状态。 */
enum class MasterState : uint8_t {
    Initial = 0,     // 尚未请求 IgH master。
    Configured = 1,  // PDO、DC 和 domain 已配置，尚未激活。
    Active = 2,      // master 已激活，可进入实时周期。
};

/** @brief 主站接口调用结果。 */
enum class MasterResult : uint8_t {
    Success = 0,
    InvalidState = 1,
    InvalidArgument = 2,
    Error = 3,
};

/** @brief 由 IghMaster 独占管理的 IgH master 句柄删除器。 */
struct NativeMasterDeleter {
    void operator()(ec_master_t *master) const noexcept;
};

/**
 * @brief 单 Domain 的通用 IgH EtherCAT 主站。
 *
 * 调用顺序：AddDevice() -> SetReferenceClockDevice() -> Configure() ->
 * Activate()。实时周期中由业务编排层调用 ReceiveAndProcess()，在完成
 * 本周期业务更新后调用 QueueAndSend()。
 *
 * 此类独占所有 IghDevice 对象，并负责它们的配置、激活前后生命周期和
 * PDO 周期调用。此类不创建实时线程、不 sleep、不调用业务或算法，也不
 * 包含具体设备 PDO。
 */
class IghMaster {
public:
    IghMaster(uint32_t master_index, uint32_t cycle_time_ns);
    ~IghMaster();

    IghMaster(const IghMaster &) = delete;
    IghMaster &operator=(const IghMaster &) = delete;

    /**
     * @brief 在 Configure() 前注册并接管一个设备适配器。
     *
     * 仅在返回 Success 时所有权转移给 IghMaster；失败时 device 保持由
     * 调用方持有，调用方可自行处理或复用它。
     */
    MasterResult AddDevice(std::unique_ptr<device::IghDevice> &device);

    /** @brief 指定已注册设备作为 EtherCAT DC 参考时钟。 */
    MasterResult SetReferenceClockDevice(const device::IghDevice &device);

    /** @brief 请求 IgH master，创建 domain，并调用所有设备 Configure()。 */
    MasterResult Configure();

    /** @brief 激活 master 并取得 domain process data 基地址。 */
    MasterResult Activate();

    /**
     * @brief 接收 EtherCAT 帧、处理 domain，并更新所有设备输入 PDO。
     *
     * application_time_ns 由实时调度层提供。若不使用 DC，可传 0。
     */
    MasterResult ReceiveAndProcess(uint64_t application_time_ns);

    /**
     * @brief 写入所有设备输出 PDO，并排队发送本周期 EtherCAT 帧。
     *
     * synchronize_dc 为 true 时，在 domain queue 前排队参考时钟和从站
     * 时钟同步报文；调用频率由上层周期逻辑决定。
     */
    MasterResult QueueAndSend(bool synchronize_dc);

    /** @brief 释放 IgH master 及其 domain 资源；可重复调用。 */
    void Release();

    MasterState state() const { return state_; }
    ec_master_t *native_master() const { return master_.get(); }
    ec_domain_t *native_domain() const { return domain_; }

private:
    bool ContainsDevice(const device::IghDevice &device) const;

    uint32_t master_index_ = 0;
    uint32_t cycle_time_ns_ = 0;
    MasterState state_ = MasterState::Initial;

    std::unique_ptr<ec_master_t, NativeMasterDeleter> master_;
    ec_domain_t *domain_ = nullptr;
    uint8_t *domain_pd_ = nullptr;

    const device::IghDevice *reference_clock_device_ = nullptr;
    std::vector<std::unique_ptr<device::IghDevice>> devices_;
};

}  // namespace master

#endif
