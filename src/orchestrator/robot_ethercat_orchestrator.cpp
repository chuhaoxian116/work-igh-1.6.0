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
    if (master_ || configuration_.cycle_time_ns == 0 || device_definitions_.empty()) {
        return OrchestratorResult::InvalidState;
    }

    master_ = std::make_unique<master::IghMaster>(configuration_.master_index,
                                                  configuration_.cycle_time_ns);

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

        // 支持零个或一个 DC 参考时钟,多个设备被标记为 DC 参考时钟时初始化失败。
        if (definition.use_as_dc_reference_clock()) {
            if (reference_clock_registered || master_->SetReferenceClockDevice(*device_observer) !=
                                                  master::MasterResult::Success) {
                Shutdown();
                return OrchestratorResult::MasterError;
            }
            reference_clock_registered = true;
        }
    }

    if (master_->Configure() != master::MasterResult::Success ||
        master_->Activate() != master::MasterResult::Success) {
        Shutdown();
        return OrchestratorResult::MasterError;
    }

    return OrchestratorResult::Success;
}

OrchestratorResult RobotEthercatOrchestrator::RunCycle(uint64_t application_time_ns) {
    if (!master_ || master_->state() != master::MasterState::Active) {
        return OrchestratorResult::InvalidState;
    }

    if (master_->ReceiveAndProcess(application_time_ns) != master::MasterResult::Success) {
        return OrchestratorResult::MasterError;
    }

    // Runtime、算法和业务数据映射将在后续放在这里。

    if (master_->QueueAndSend(configuration_.synchronize_dc) != master::MasterResult::Success) {
        return OrchestratorResult::MasterError;
    }

    return OrchestratorResult::Success;
}

void RobotEthercatOrchestrator::Shutdown() {
    master_.reset();
}

}  // namespace orchestrator
