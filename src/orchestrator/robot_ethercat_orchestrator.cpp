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

  PublishGsd620Feedback();

  // Runtime、算法和业务数据映射将在后续放在这里。

  if (master_->QueueAndSend(configuration_.synchronize_dc) !=
      master::MasterResult::Success) {
    return OrchestratorResult::MasterError;
  }

  return OrchestratorResult::Success;
}

void RobotEthercatOrchestrator::Shutdown() {
  feedback_valid_.store(false, std::memory_order_release);
  gsd620_device_ = nullptr;
  master_.reset();
}

bool RobotEthercatOrchestrator::ReadGsd620Feedback(
    Gsd620FeedbackSnapshot &snapshot) const noexcept {
  if (!gsd620_device_ || !feedback_valid_.load(std::memory_order_acquire)) {
    return false;
  }

  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    const uint64_t begin = feedback_sequence_.load(std::memory_order_acquire);
    if ((begin & 1U) != 0U) {
      continue;
    }

    snapshot.actual_position = actual_position_.load(std::memory_order_relaxed);
    snapshot.actual_velocity = actual_velocity_.load(std::memory_order_relaxed);
    snapshot.actual_torque = actual_torque_.load(std::memory_order_relaxed);
    snapshot.error_code = error_code_.load(std::memory_order_relaxed);
    snapshot.statusword = statusword_.load(std::memory_order_relaxed);
    snapshot.mode_display = mode_display_.load(std::memory_order_relaxed);

    const uint64_t end = feedback_sequence_.load(std::memory_order_acquire);
    if (begin == end && (end & 1U) == 0U) {
      return true;
    }
  }

  return false;
}

void RobotEthercatOrchestrator::PublishGsd620Feedback() noexcept {
  if (!gsd620_device_) {
    return;
  }

  const device::Gsd620CyclicData &data = gsd620_device_->cyclic_data();
  feedback_sequence_.fetch_add(1, std::memory_order_release);
  actual_position_.store(data.actual_position, std::memory_order_relaxed);
  actual_velocity_.store(data.actual_velocity, std::memory_order_relaxed);
  actual_torque_.store(data.actual_torque, std::memory_order_relaxed);
  error_code_.store(data.error_code, std::memory_order_relaxed);
  statusword_.store(data.axis.inData.statusword, std::memory_order_relaxed);
  mode_display_.store(data.axis.inData.mode_display, std::memory_order_relaxed);
  feedback_sequence_.fetch_add(1, std::memory_order_release);
  feedback_valid_.store(true, std::memory_order_release);
}

} // namespace orchestrator
