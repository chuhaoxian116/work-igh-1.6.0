#ifndef IGH_DEVICE_DEVICE_SETUP_H
#define IGH_DEVICE_DEVICE_SETUP_H

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "device/igh_device.h"

namespace device {

/**
 * @brief 一个可重复创建设备实例的通用设备定义。
 *
 * DeviceDefinition 只保存设备工厂和 DC 参考时钟标记，不依赖任何具体
 * 从站类型。编排层每次 Initialize() 时通过 CreateDevice() 创建新实例，
 * 因此 Shutdown() 后仍可重新初始化。
 */
class DeviceDefinition {
public:
    using DeviceFactory = std::function<std::unique_ptr<IghDevice>()>;

    /**
     * @brief 为任意具体 IghDevice 类型创建设备定义。
     *
     * 构造参数会保存到定义中，并在每次 CreateDevice() 时用于创建新的
     * DeviceType。为支持重复初始化，保存的参数必须能够以 const 引用重复
     * 构造 DeviceType。
     *
     * @tparam DeviceType 需要创建的具体从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param use_as_dc_reference_clock 是否将该设备选为 DC 参考时钟。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 可交给编排层保存和使用的设备定义。
     */
    template <typename DeviceType, typename... Args>
    static DeviceDefinition Create(bool use_as_dc_reference_clock, Args&&... args) {
        static_assert(std::is_base_of<IghDevice, DeviceType>::value,
                      "DeviceType must derive from device::IghDevice");
        static_assert(!std::is_abstract<DeviceType>::value,
                      "DeviceType must be a concrete device adapter");
        static_assert(
            std::is_constructible<DeviceType, const typename std::decay<Args>::type&...>::value,
            "DeviceType must be repeatedly constructible from the stored arguments");

        using StoredArguments = std::tuple<typename std::decay<Args>::type...>;
        const auto stored_arguments =
            std::make_shared<const StoredArguments>(std::forward<Args>(args)...);

        DeviceFactory factory = [stored_arguments]() -> std::unique_ptr<IghDevice> {
            return std::apply(
                [](const auto&... stored_args) -> std::unique_ptr<IghDevice> {
                    return std::make_unique<DeviceType>(stored_args...);
                },
                *stored_arguments);
        };

        return DeviceDefinition(std::move(factory), use_as_dc_reference_clock);
    }

    /**
     * @brief 使用保存的工厂创建一个新的设备适配器。
     *
     * @return 新创建且尚未注册的设备；工厂无效或创建失败时为 nullptr。
     */
    std::unique_ptr<IghDevice> CreateDevice() const { return factory_ ? factory_() : nullptr; }

    /**
     * @brief 查询该设备是否应作为 DC 参考时钟。
     *
     * @return true 初始化时应选择该设备为 DC 参考时钟。
     * @return false 该设备不是 DC 参考时钟。
     */
    bool use_as_dc_reference_clock() const noexcept { return use_as_dc_reference_clock_; }

private:
    /**
     * @brief 保存设备工厂和 DC 参考时钟属性。
     *
     * @param factory 用于重复创建设备实例的工厂。
     * @param use_as_dc_reference_clock 是否作为 DC 参考时钟。
     */
    DeviceDefinition(DeviceFactory factory, bool use_as_dc_reference_clock)
        : factory_(std::move(factory)), use_as_dc_reference_clock_(use_as_dc_reference_clock) {}

    DeviceFactory factory_{};                 // 用于每次初始化时创建设备实例。
    bool use_as_dc_reference_clock_ = false;  // 是否将该设备选为 DC 参考时钟。
};

using DeviceDefinitions = std::vector<DeviceDefinition>;

/**
 * @brief 用模板接口组装一组与具体设备类型无关的设备定义。
 *
 * 普通设备使用 AddDevice()，唯一的 DC 参考设备使用
 * AddReferenceClockDevice()。Build() 后将定义交给业务编排层。
 */
class DeviceSetup {
public:
    /**
     * @brief 添加一个普通 EtherCAT 从站定义。
     *
     * @tparam DeviceType 具体从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 当前 DeviceSetup，可继续链式添加设备。
     */
    template <typename DeviceType, typename... Args>
    DeviceSetup& AddDevice(Args&&... args) {
        definitions_.push_back(
            DeviceDefinition::Create<DeviceType>(false, std::forward<Args>(args)...));
        return *this;
    }

    /**
     * @brief 添加一个需要选为 DC 参考时钟的 EtherCAT 从站定义。
     *
     * 一组定义最多应调用一次本函数；编排层会拒绝多个 DC 参考设备。
     *
     * @tparam DeviceType 具体从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 当前 DeviceSetup，可继续链式添加设备。
     */
    template <typename DeviceType, typename... Args>
    DeviceSetup& AddReferenceClockDevice(Args&&... args) {
        definitions_.push_back(
            DeviceDefinition::Create<DeviceType>(true, std::forward<Args>(args)...));
        return *this;
    }

    /**
     * @brief 结束设备装配并转移全部设备定义。
     *
     * @return 可传给 RobotEthercatOrchestrator 的设备定义列表。
     */
    DeviceDefinitions Build() && { return std::move(definitions_); }

private:
    DeviceDefinitions definitions_{};  // 按 EtherCAT 从站配置顺序保存的定义。
};

}  // namespace device

#endif
