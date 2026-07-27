#ifndef IGH_MASTER_IGH_MASTER_H
#define IGH_MASTER_IGH_MASTER_H

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "device/igh_device.h"

namespace master {

/** @brief IgH 主站对象的生命周期状态。 */
enum class MasterState : uint8_t {
    kInitial = 0,     // 尚未请求 IgH master。
    kConfigured = 1,  // PDO、DC 和 domain 已配置，尚未激活。
    kActive = 2,      // master 已激活，可进入实时周期。
};

/** @brief 主站接口调用结果。 */
enum class MasterResult : uint8_t {
    kSuccess = 0,          // 本次调用成功。
    kInvalidState = 1,     // 当前主站生命周期状态不允许调用。
    kInvalidArgument = 2,  // 参数为空、重复或不属于当前主站。
    kError = 3,            // IgH 底层调用或设备适配器配置失败。
};

/**
 * @brief 模板化设备注册接口的返回结果。
 *
 * device 只是观察指针；注册成功后对象由 IghMaster 独占管理，调用方不得
 * delete。注册失败时 device 为 nullptr。
 */
template <typename DeviceType>
struct DeviceAddResult {
    MasterResult result = MasterResult::kError;  // 设备注册结果。
    DeviceType* device = nullptr;  // 注册成功后的非拥有型设备观察指针。

    /**
   * @brief 判断设备是否注册成功。
   *
   * @return true result 为 kSuccess 且 device 有效。
   * @return false 设备注册失败。
   */
    explicit operator bool() const noexcept {
        return result == MasterResult::kSuccess && device != nullptr;
    }
};

/** @brief 由 IghMaster 独占管理的 IgH master 句柄删除器。 */
struct NativeMasterDeleter {
    /**
     * @brief 调用 IgH API 释放已请求的 master 句柄。
     *
     * @param master 需要释放的 IgH master 句柄；允许为 nullptr。
     */
    void operator()(ec_master_t* master) const noexcept;
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
    /**
   * @brief 创建尚未请求 IgH master 的主站管理对象。
   *
   * 构造阶段不访问 EtherCAT 总线；实际 master 请求发生在 Configure()。
   *
   * @param master_index 需要请求的 IgH master 编号。
   * @param cycle_time_ns 主站标称周期，单位为纳秒。
   */
    IghMaster(uint32_t master_index, uint32_t cycle_time_ns);

    /**
   * @brief 自动释放 IgH master，并重置所有已注册设备。
   */
    ~IghMaster();

    IghMaster(const IghMaster&) = delete;
    IghMaster& operator=(const IghMaster&) = delete;

    /**
   * @brief 在 Configure() 前注册并接管一个设备适配器。
   *
   * 仅在返回 kSuccess 时所有权转移给 IghMaster；失败时 device 保持由
   * 调用方持有，调用方可自行处理或复用它。
   *
   * @param device 待注册的从站适配器智能指针。
   * @return kSuccess 主站已接管从站对象。
   * @return kInvalidState 主站已经配置或激活，不能新增从站。
   * @return kInvalidArgument device 为空或该对象已被注册。
   */
    MasterResult AddDevice(std::unique_ptr<device::IghDevice>& device);

    /**
   * @brief 构造、注册并接管一个具体类型的从站适配器。
   *
   * 本接口用于初始化阶段减少 make_unique、基类指针转换和观察指针保存的
   * 样板代码。DeviceType 必须派生自 IghDevice，且不能是抽象类。
   *
   * @tparam DeviceType 需要创建的具体从站适配器类型。
   * @tparam Args DeviceType 构造函数参数类型。
   * @param args 转发给 DeviceType 构造函数的参数。
   * @return 注册结果以及成功后的非拥有型具体设备观察指针。
   */
    template <typename DeviceType, typename... Args>
    DeviceAddResult<DeviceType> AddDevice(Args&&... args) {
        static_assert(std::is_base_of<device::IghDevice, DeviceType>::value,
                      "DeviceType must derive from device::IghDevice");
        static_assert(!std::is_abstract<DeviceType>::value,
                      "DeviceType must be a concrete device adapter");

        if (state_ != MasterState::kInitial) {
            return {MasterResult::kInvalidState, nullptr};
        }

        auto concrete_device = std::make_unique<DeviceType>(std::forward<Args>(args)...);
        DeviceType* const observer = concrete_device.get();
        std::unique_ptr<device::IghDevice> base_device = std::move(concrete_device);

        const MasterResult result = AddDevice(base_device);
        if (result != MasterResult::kSuccess) {
            return {result, nullptr};
        }
        return {MasterResult::kSuccess, observer};
    }

    /**
   * @brief 指定一个已注册从站作为 EtherCAT DC 参考时钟。
   *
   * 必须在 Configure() 前调用；未调用时仍可配置主站，但不会显式选择
   * DC 参考时钟。
   *
   * @param device 已由当前主站注册的从站对象。
   * @return kSuccess 已设置 DC 参考时钟。
   * @return kInvalidState 主站已经配置或激活。
   * @return kInvalidArgument device 不属于当前主站。
   */
    MasterResult SetReferenceClockDevice(const device::IghDevice& device);

    /**
   * @brief 请求 IgH master，创建 PDO domain，并配置所有已注册从站。
   *
   * 本函数依次调用每个 IghDevice::Configure()，并在设置了参考设备时
   * 选择其为 DC 参考时钟。任一步失败都会释放已请求的 IgH 资源。
   *
   * @return kSuccess 主站和所有从站配置完成，状态进入 kConfigured。
   * @return kInvalidState 主站不是 kInitial 状态，或尚未注册任何从站。
   * @return kError 请求 IgH master、创建 domain 或从站配置失败。
   */
    MasterResult Configure();

    /**
   * @brief 激活 IgH master 并取得 domain process data 基地址。
   *
   * 成功后才允许进入实时周期；激活失败时会释放 IgH 资源并重置设备。
   *
   * @return kSuccess 主站状态进入 kActive。
   * @return kInvalidState 主站尚未完成 Configure()。
   * @return kError 激活 master 或取得 domain 数据基地址失败。
   */
    MasterResult Activate();

    /**
   * @brief 接收 EtherCAT 帧、处理 domain，并更新所有设备输入 PDO。
   *
   * application_time_ns 由实时调度层提供。若不使用 DC，可传 0。
   *
   * @param application_time_ns 当前周期的单调时间，单位为纳秒。
   * @return kSuccess 已完成帧接收、domain 处理和全部设备输入 PDO 映射。
   * @return kInvalidState 主站未处于 kActive 状态。
   */
    MasterResult ReceiveAndProcess(uint64_t application_time_ns);

    /**
   * @brief 写入所有设备输出 PDO，并排队发送本周期 EtherCAT 帧。
   *
   * synchronize_dc 为 true 时，在 domain queue 前排队参考时钟和从站
   * 时钟同步报文；调用频率由上层周期逻辑决定。
   *
   * @param synchronize_dc 是否在本周期同步参考时钟与从站时钟。
   * @return kSuccess 已完成全部设备输出 PDO 映射并发送帧。
   * @return kInvalidState 主站未处于 kActive 状态。
   */
    MasterResult QueueAndSend(bool synchronize_dc);

    /**
   * @brief 重置设备并释放 IgH master 及其关联 domain 资源。
   *
   * 此函数可重复调用；已注册的设备对象仍由 IghMaster 保留，因此可在
   * 后续再次调用 Configure() 重新建立通信。
   */
    void Release();

    /**
   * @brief 获取当前主站生命周期状态。
   *
   * @return 当前主站状态。
   */
    MasterState state() const { return state_; }

    /**
     * @brief 判断最近一次 Domain 过程数据交换是否完整。
     *
     * 该值在 ReceiveAndProcess() 调用 ecrt_domain_process() 后更新，仅供
     * 主站内部 PDO 桥接生成 communication_valid，不对外暴露
     * 完整健康快照。
     *
     * @return true 最近一次 Domain working counter 完整。
     * @return false 尚未收到完整过程数据或主站未激活。
     */
    bool domain_data_valid() const noexcept { return domain_data_valid_; }

    /**
     * @brief 获取 IgH 原生 master 观察指针。
     *
     * 调用方不得释放该指针，也不应绕过 IghMaster 直接执行周期调用。
     *
     * @return 当前 IgH master 句柄；未配置时为 nullptr。
     */
    ec_master_t* native_master() const { return master_.get(); }

    /**
     * @brief 获取 IgH 原生 PDO domain 观察指针。
     *
     * @return 当前 domain 句柄；Configure() 前或 Release() 后为 nullptr。
     */
    ec_domain_t* native_domain() const { return domain_; }

private:
    /**
   * @brief 判断指定从站对象是否已由当前主站注册。
   *
   * @param device 待查询的从站对象。
   * @return true device 已存在于设备列表。
   * @return false device 不属于当前主站。
   */
    bool ContainsDevice(const device::IghDevice& device) const;

    uint32_t master_index_ = 0;   // 需要请求的 IgH master 编号。
    uint32_t cycle_time_ns_ = 0;  // 设备配置使用的标称周期，单位为纳秒。
    MasterState state_ = MasterState::kInitial;                 // 当前主站生命周期状态。
    std::unique_ptr<ec_master_t, NativeMasterDeleter> master_;  // 独占的 IgH master 句柄。
    ec_domain_t* domain_ = nullptr;   // 由 master_ 管理的唯一 PDO domain。
    uint8_t* domain_pd_ = nullptr;    // 激活后取得的 domain process data 基地址。
    bool domain_data_valid_ = false;  // 最近一次 Domain working counter 是否完整。
    const device::IghDevice* reference_clock_device_ =
        nullptr;  // devices_ 中被选为 DC 参考时钟的观察指针。
    std::vector<std::unique_ptr<device::IghDevice>> devices_;  // 主站独占管理的从站适配器列表。
};

}  // namespace master

#endif
