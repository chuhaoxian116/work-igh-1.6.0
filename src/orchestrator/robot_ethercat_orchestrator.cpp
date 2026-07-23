#include "orchestrator/robot_ethercat_orchestrator.h"

namespace orchestrator {

RobotEthercatOrchestrator::RobotEthercatOrchestrator(
    RobotEthercatConfiguration configuration)
    : configuration_(configuration) {}

RobotEthercatOrchestrator::~RobotEthercatOrchestrator() {
  Shutdown();
}

OrchestratorResult RobotEthercatOrchestrator::Initialize() {
  if (master_ || configuration_.cycle_time_ns == 0) {
    return OrchestratorResult::InvalidState;
  }

  master_ = std::make_unique<master::IghMaster>(
      configuration_.master_index, configuration_.cycle_time_ns);

  std::unique_ptr<device::IghDevice> gsd620 =
      std::make_unique<device::Gsd620Device>(configuration_.gsd620);
  auto *const gsd620_observer = static_cast<device::Gsd620Device *>(gsd620.get());

  if (master_->AddDevice(gsd620) != master::MasterResult::Success ||
      master_->SetReferenceClockDevice(*gsd620_observer) !=
          master::MasterResult::Success ||
      master_->Configure() != master::MasterResult::Success ||
      master_->Activate() != master::MasterResult::Success) {
    Shutdown();
    return OrchestratorResult::MasterError;
  }

  gsd620_device_ = gsd620_observer;
  return OrchestratorResult::Success;
}

OrchestratorResult RobotEthercatOrchestrator::RunCycle(
    uint64_t application_time_ns) {
  if (!master_ || master_->state() != master::MasterState::Active) {
    return OrchestratorResult::InvalidState;
  }

  if (master_->ReceiveAndProcess(application_time_ns) !=
      master::MasterResult::Success) {
    return OrchestratorResult::MasterError;
  }

  // Runtime、算法和业务数据映射将在后续放在这里。

  if (master_->QueueAndSend(configuration_.synchronize_dc) !=
      master::MasterResult::Success) {
    return OrchestratorResult::MasterError;
  }

  return OrchestratorResult::Success;
}

void RobotEthercatOrchestrator::Shutdown() {
  gsd620_device_ = nullptr;
  master_.reset();
}

} // namespace orchestrator
