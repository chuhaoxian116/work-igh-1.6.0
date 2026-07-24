#include "orchestrator/robot_ethercat_orchestrator.h"

#include <utility>

namespace orchestrator {

RobotEthercatOrchestrator::RobotEthercatOrchestrator(RobotEthercatConfiguration configuration,
                                                     device::DeviceDefinitions device_definitions)
    : configuration_(configuration), device_definitions_(std::move(device_definitions)) {}

RobotEthercatOrchestrator::~RobotEthercatOrchestrator() {
    Shutdown();
}

OrchestratorResult RobotEthercatOrchestrator::Initialize() {
    // 步骤 1：检查编排层生命周期以及建立主站所需的静态配置。
    if (master_ || configuration_.cycle_time_ns == 0 || device_definitions_.empty()) {
        return OrchestratorResult::InvalidState;
    }

    // 步骤 2：创建不包含具体设备逻辑的通用 IgH 主站。
    master_ = std::make_unique<master::IghMaster>(configuration_.master_index,
                                                  configuration_.cycle_time_ns);

    // 步骤 3：按定义顺序创建设备，并将设备所有权统一移交给主站。
    bool reference_clock_registered = false;
    for (const device::DeviceDefinition& definition : device_definitions_) {
        std::unique_ptr<device::IghDevice> ethercat_device = definition.CreateDevice();
        if (!ethercat_device) {
            Shutdown();
            return OrchestratorResult::MasterError;
        }

        device::IghDevice* const device_observer = ethercat_device.get();
        if (master_->AddDevice(ethercat_device) != master::MasterResult::Success) {
            Shutdown();
            return OrchestratorResult::MasterError;
        }

        // 步骤 4：允许零个或一个 DC 参考设备，拒绝互相冲突的重复标记。
        if (definition.use_as_dc_reference_clock()) {
            if (reference_clock_registered || master_->SetReferenceClockDevice(*device_observer) !=
                                                  master::MasterResult::Success) {
                Shutdown();
                return OrchestratorResult::MasterError;
            }
            reference_clock_registered = true;
        }
    }

    // 步骤 5：全部设备注册后，集中完成总线配置并激活实时通信。
    if (master_->Configure() != master::MasterResult::Success ||
        master_->Activate() != master::MasterResult::Success) {
        Shutdown();
        return OrchestratorResult::MasterError;
    }

    return OrchestratorResult::Success;
}

OrchestratorResult RobotEthercatOrchestrator::RunCycle(uint64_t application_time_ns) {
    // 步骤 1：确认主站已经成功激活。
    if (!master_ || master_->state() != master::MasterState::Active) {
        return OrchestratorResult::InvalidState;
    }

    // 步骤 2：接收帧、处理 Domain，并读取所有设备的 TxPDO。
    if (master_->ReceiveAndProcess(application_time_ns) != master::MasterResult::Success) {
        return OrchestratorResult::MasterError;
    }

    // 步骤 3：在输入已更新、输出尚未发送的窗口执行 Runtime 和业务数据映射。
    // TODO: 后续在这里接入 Runtime、算法与设备周期数据交互。

    // 步骤 4：写入所有设备的 RxPDO，执行可选 DC 同步并发送本周期帧。
    if (master_->QueueAndSend(configuration_.synchronize_dc) != master::MasterResult::Success) {
        return OrchestratorResult::MasterError;
    }

    return OrchestratorResult::Success;
}

void RobotEthercatOrchestrator::Shutdown() {
    // 步骤 1：销毁主站，由其依次重置设备并释放全部 IgH 资源。
    master_.reset();
}

}  // namespace orchestrator
