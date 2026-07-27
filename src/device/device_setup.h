#ifndef IGH_DEVICE_DEVICE_SETUP_H
#define IGH_DEVICE_DEVICE_SETUP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "device/igh_device.h"

namespace device {

inline constexpr uint8_t kUnassignedLogicalAxisIndex = 0xFFU;

/**
 * @brief 从站在机器人应用中的业务角色。
 *
 * 该角色不描述具体设备型号，只用于决定设备是否参与 RobotCycleData 的
 * 逻辑轴映射。
 */
enum class DeviceRole : uint8_t {
    kGeneric = 0,    // MCU、IO、传感器等不占用机器人逻辑轴的普通设备。
    kRobotAxis = 1,  // 映射到 RobotCycleData::robot_feedback/setpoints 的机器人轴。
};

/**
 * @brief 从站的业务角色和逻辑轴绑定。
 */
struct DeviceBinding {
    DeviceRole role = DeviceRole::kGeneric;                    // 从站业务角色。
    uint8_t logical_axis_index = kUnassignedLogicalAxisIndex;  // RobotCycleData 逻辑轴下标。
};

/**
 * @brief 一个可重复创建设备实例的通用设备定义。
 *
 * DeviceDefinition 保存设备工厂、DC 参考时钟标记和业务绑定，不依赖任何
 * 具体从站类型。编排层每次 Initialize() 时通过 CreateDevice() 创建新
 * 实例，因此 Shutdown() 后仍可重新初始化。
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
        return CreateWithBinding<DeviceType>(
            {}, use_as_dc_reference_clock, std::forward<Args>(args)...);
    }

    /**
     * @brief 为任意具体 IghDevice 类型创建带业务绑定的设备定义。
     *
     * 本接口主要由 DeviceSetup 的机器人轴模板方法调用。普通调用方优先
     * 使用 AddDevice()、AddRobotAxisDevice() 等装配接口。
     *
     * @tparam DeviceType 需要创建的具体从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param binding 设备业务角色和逻辑轴编号。
     * @param use_as_dc_reference_clock 是否将该设备选为 DC 参考时钟。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 可交给编排层保存和使用的设备定义。
     */
    template <typename DeviceType, typename... Args>
    static DeviceDefinition CreateWithBinding(DeviceBinding binding,
                                              bool use_as_dc_reference_clock,
                                              Args&&... args) {
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

        return DeviceDefinition(std::move(factory), binding, use_as_dc_reference_clock);
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

    /**
     * @brief 返回设备的业务角色和逻辑轴绑定。
     *
     * @return 构建设备定义时保存的业务绑定。
     */
    const DeviceBinding& binding() const noexcept { return binding_; }

private:
    /**
     * @brief 保存设备工厂和 DC 参考时钟属性。
     *
     * @param factory 用于重复创建设备实例的工厂。
     * @param binding 设备业务角色和逻辑轴编号。
     * @param use_as_dc_reference_clock 是否作为 DC 参考时钟。
     */
    DeviceDefinition(DeviceFactory factory, DeviceBinding binding, bool use_as_dc_reference_clock)
        : factory_(std::move(factory)), binding_(binding),
          use_as_dc_reference_clock_(use_as_dc_reference_clock) {}

    DeviceFactory factory_{};                 // 用于每次初始化时创建设备实例。
    DeviceBinding binding_{};                 // 设备角色和逻辑轴业务映射。
    bool use_as_dc_reference_clock_ = false;  // 是否将该设备选为 DC 参考时钟。
};

using DeviceDefinitions = std::vector<DeviceDefinition>;

/**
 * @brief 用模板接口组装一组与具体设备类型无关的设备定义。
 *
 * 四个添加接口分别表达两个相互独立的属性：设备是否映射为机器人逻辑
 * 轴，以及设备是否被选为全站唯一的 DC 参考时钟。设备自身是否启用 DC
 * 仍由具体设备配置决定。
 *
 * AddDevice() 和 AddReferenceClockDevice() 不占用 RobotCycleData 轴编号；
 * AddRobotAxisDevice() 和 AddRobotAxisReferenceClockDevice() 必须指定逻辑
 * 轴编号。Build() 后将完整定义交给业务编排层。
 */
class DeviceSetup {
public:
    /**
     * @brief 添加一个不参与机器人轴映射的普通 EtherCAT 从站。
     *
     * 该设备的角色为 kGeneric，不占用 RobotCycleData 逻辑轴编号，也不会
     * 被选为 DC 参考时钟。适用于 MCU、IO、传感器以及不参与机器人本体
     * 轴数据交换的其他从站。
     *
     * 本接口不禁止具体设备在自身 Configure() 中启用 DC；它只表示该设备
     * 不是主站选定的 DC 参考时钟。
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
     * @brief 添加一个不参与机器人轴映射的 DC 参考时钟从站。
     *
     * 该设备的角色为 kGeneric，不占用 RobotCycleData 逻辑轴编号，但会
     * 在主站激活前被选为 DC 参考时钟。适用于具备 DC 能力的 IO、传感器
     * 或其他非机器人轴设备。
     *
     * 一组定义中，本接口与 AddRobotAxisReferenceClockDevice() 合计最多
     * 使用一次；编排层会拒绝多个 DC 参考设备。调用方必须确保具体设备
     * 支持并已启用 DC。
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
     * @brief 添加一个不作为 DC 参考时钟的机器人逻辑轴设备。
     *
     * 该设备的角色为 kRobotAxis，并映射到 RobotCycleData 对应下标，但
     * 不会被选为 DC 参考时钟。逻辑轴编号由机器人应用装配层确定，与
     * EtherCAT position 和 alias 相互独立。
     *
     * DeviceType 必须继承 Cia402StandardPdoDevice；编排层在 Initialize()
     * 中统一检查类型、编号范围、重复编号和编号断档。
     *
     * @tparam DeviceType 具体机器人轴从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param logical_axis_index RobotCycleData 中的机器人轴下标。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 当前 DeviceSetup，可继续链式添加设备。
     */
    template <typename DeviceType, typename... Args>
    DeviceSetup& AddRobotAxisDevice(uint8_t logical_axis_index, Args&&... args) {
        const DeviceBinding binding{DeviceRole::kRobotAxis, logical_axis_index};
        definitions_.push_back(DeviceDefinition::CreateWithBinding<DeviceType>(
            binding, false, std::forward<Args>(args)...));
        return *this;
    }

    /**
     * @brief 添加一个同时作为 DC 参考时钟的机器人轴设备。
     *
     * 该设备的角色为 kRobotAxis，既映射到 RobotCycleData 对应逻辑轴，
     * 又会在主站激活前被选为全站 DC 参考时钟。通常用于将第一台伺服轴
     * 同时作为机器人逻辑轴 0 和 DC 参考设备。
     *
     * DeviceType 必须继承 Cia402StandardPdoDevice，并且具体设备必须支持
     * 且已启用 DC。一组定义中，本接口与 AddReferenceClockDevice() 合计
     * 最多使用一次。
     *
     * @tparam DeviceType 具体机器人轴从站适配器类型。
     * @tparam Args DeviceType 的构造参数类型。
     * @param logical_axis_index RobotCycleData 中的机器人轴下标。
     * @param args 转发并保存的 DeviceType 构造参数。
     * @return 当前 DeviceSetup，可继续链式添加设备。
     */
    template <typename DeviceType, typename... Args>
    DeviceSetup& AddRobotAxisReferenceClockDevice(uint8_t logical_axis_index, Args&&... args) {
        const DeviceBinding binding{DeviceRole::kRobotAxis, logical_axis_index};
        definitions_.push_back(DeviceDefinition::CreateWithBinding<DeviceType>(
            binding, true, std::forward<Args>(args)...));
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
