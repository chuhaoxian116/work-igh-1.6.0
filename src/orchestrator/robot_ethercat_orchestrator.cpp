#include "orchestrator/robot_ethercat_orchestrator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "device/cia402_standard_pdo_device.h"
#include "orchestrator/robot_pdo_bridge.h"
#include "robot_data.h"

namespace orchestrator {
namespace {

/**
 * @brief 校验设备定义中的机器人逻辑轴编号。
 *
 * @param definitions 待校验的全部设备定义。
 * @param robot_axis_count 返回机器人轴定义数量。
 * @return true 普通设备未占用轴编号，机器人轴编号唯一且从 0 连续排列。
 * @return false 角色无效、编号越界、重复或存在断档。
 */
bool ValidateDeviceBindings(const device::DeviceDefinitions& definitions,
                            std::size_t& robot_axis_count) noexcept {
    std::array<bool, robot_interface::kMaxRobotAxisCount> configured_axes{};
    robot_axis_count = 0;

    for (const device::DeviceDefinition& definition : definitions) {
        const device::DeviceBinding& binding = definition.binding();
        switch (binding.role) {
            case device::DeviceRole::kGeneric:
                if (binding.logical_axis_index != device::kUnassignedLogicalAxisIndex) {
                    return false;
                }
                break;

            case device::DeviceRole::kRobotAxis:
                if (binding.logical_axis_index >= robot_interface::kMaxRobotAxisCount ||
                    configured_axes[binding.logical_axis_index]) {
                    return false;
                }
                configured_axes[binding.logical_axis_index] = true;
                ++robot_axis_count;
                break;

            default:
                return false;
        }
    }

    // RobotCycleData 使用 [0, robot_axis_count) 连续区间，拒绝中间缺轴。
    for (std::size_t index = 0; index < robot_axis_count; ++index) {
        if (!configured_axes[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

RobotEthercatOrchestrator::RobotEthercatOrchestrator(RobotEthercatConfiguration configuration,
                                                     device::DeviceDefinitions device_definitions)
    : configuration_(configuration), device_definitions_(std::move(device_definitions)) {}

RobotEthercatOrchestrator::~RobotEthercatOrchestrator() {
    Shutdown();
}

OrchestratorResult RobotEthercatOrchestrator::Initialize() {
    // 步骤 1：检查编排层生命周期以及建立主站所需的静态配置。
    if (master_ || configuration_.cycle_time_ns == 0 || device_definitions_.empty()) {
        return OrchestratorResult::kInvalidState;
    }

    // 步骤 2：校验逻辑轴编号的范围、唯一性和连续性。
    std::size_t robot_axis_count = 0;
    if (!ValidateDeviceBindings(device_definitions_, robot_axis_count)) {
        return OrchestratorResult::kInvalidConfiguration;
    }

    // 步骤 3：创建不包含具体设备逻辑的通用 IgH 主站。
    master_ = std::make_unique<master::IghMaster>(configuration_.master_index,
                                                  configuration_.cycle_time_ns);
    std::vector<RobotAxisPdoBinding> robot_axes;
    robot_axes.reserve(robot_axis_count);

    // 步骤 4：按定义顺序创建设备，并将设备所有权统一移交给主站。
    bool reference_clock_registered = false;
    for (const device::DeviceDefinition& definition : device_definitions_) {
        std::unique_ptr<device::IghDevice> ethercat_device = definition.CreateDevice();
        if (!ethercat_device) {
            Shutdown();
            return OrchestratorResult::kMasterError;
        }

        device::IghDevice* const device_observer = ethercat_device.get();
        device::Cia402StandardPdoDevice* robot_axis_device = nullptr;
        if (definition.binding().role == device::DeviceRole::kRobotAxis) {
            // 只在初始化阶段确认轴设备能力，实时周期不执行 RTTI 判断。
            robot_axis_device = dynamic_cast<device::Cia402StandardPdoDevice*>(device_observer);
            if (!robot_axis_device) {
                Shutdown();
                return OrchestratorResult::kInvalidConfiguration;
            }
        }

        if (master_->AddDevice(ethercat_device) != master::MasterResult::kSuccess) {
            Shutdown();
            return OrchestratorResult::kMasterError;
        }

        if (robot_axis_device) {
            robot_axes.push_back({definition.binding().logical_axis_index, robot_axis_device});
        }

        // 步骤 5：允许零个或一个 DC 参考设备，拒绝互相冲突的重复标记。
        if (definition.use_as_dc_reference_clock()) {
            if (reference_clock_registered) {
                Shutdown();
                return OrchestratorResult::kInvalidConfiguration;
            }
            if (master_->SetReferenceClockDevice(*device_observer) !=
                master::MasterResult::kSuccess) {
                Shutdown();
                return OrchestratorResult::kMasterError;
            }
            reference_clock_registered = true;
        }
    }

    // 步骤 6：固定按逻辑轴编号遍历，为下一步 RobotCycleData 映射做准备。
    std::sort(robot_axes.begin(),
              robot_axes.end(),
              [](const RobotAxisPdoBinding& left, const RobotAxisPdoBinding& right) {
                  return left.logical_axis_index < right.logical_axis_index;
              });

    // 步骤 7：全部设备注册后，集中完成总线配置并激活实时通信。
    if (master_->Configure() != master::MasterResult::kSuccess ||
        master_->Activate() != master::MasterResult::kSuccess) {
        Shutdown();
        return OrchestratorResult::kMasterError;
    }

    // 步骤 8：主站激活后建立公共周期数据、CiA402 轴状态与 PDO 的内部桥接。
    pdo_bridge_ = std::make_unique<RobotPdoBridge>();
    if (!pdo_bridge_->Configure(configuration_.cycle_time_ns, std::move(robot_axes))) {
        Shutdown();
        return OrchestratorResult::kInvalidConfiguration;
    }

    return OrchestratorResult::kSuccess;
}

OrchestratorResult RobotEthercatOrchestrator::RunCycle(uint64_t application_time_ns) {
    // 步骤 1：确认主站已经成功激活。
    if (!master_ || !pdo_bridge_ || master_->state() != master::MasterState::kActive) {
        return OrchestratorResult::kInvalidState;
    }

    // 步骤 2：接收帧、处理 Domain，并读取所有设备的 TxPDO。
    if (master_->ReceiveAndProcess(application_time_ns) != master::MasterResult::kSuccess) {
        return OrchestratorResult::kMasterError;
    }

    // 步骤 3：将 TxPDO 导入公共反馈和私有 CiA402 轴输入。
    pdo_bridge_->UpdateFeedbackFromPdo(master_->domain_data_valid());

    // 步骤 4：算法同步回调后续插入此处，只接触 cycle_data() 公共数据。

    // 步骤 5：执行各项多轴请求，并将控制字、模式和目标值导出到 RxPDO。
    // 单周期命令 kError 由桥接层记录，不应因此中断 EtherCAT 周期收发。
    pdo_bridge_->ProcessCommands();

    // 步骤 6：写入所有设备的 RxPDO，执行可选 DC 同步并发送本周期帧。
    if (master_->QueueAndSend(configuration_.synchronize_dc) != master::MasterResult::kSuccess) {
        return OrchestratorResult::kMasterError;
    }

    return OrchestratorResult::kSuccess;
}

void RobotEthercatOrchestrator::Shutdown() {
    // 步骤 1：先释放桥接层，清除其保存的全部设备观察指针。
    pdo_bridge_.reset();

    // 步骤 2：销毁主站，由其依次重置设备并释放全部 IgH 资源。
    master_.reset();
}

}  // namespace orchestrator
