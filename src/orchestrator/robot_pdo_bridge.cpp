#include "orchestrator/robot_pdo_bridge.h"

#include <cstddef>
#include <utility>

namespace orchestrator {
namespace {

/**
 * @brief 对全部已配置机器人轴执行一次相同类型的 CiA402 操作。
 *
 * @tparam Operation 接收 cia402::AxisData& 并返回 cia402::FbStatus 的操作。
 * @param cycle_data 当前公共周期数据。
 * @param runtime_axes 桥接层私有的 CiA402 轴状态。
 * @param operation 当前需要对每个有效轴执行的操作。
 * @return kSuccess 全部轴通信有效且没有操作返回 kError。
 * @return kError 至少一个轴通信无效或操作失败。
 */
template <typename Operation>
RobotCommandResult ProcessRobotAxes(
    const robot_interface::RobotCycleData& cycle_data,
    std::array<cia402::AxisData, robot_interface::kMaxRobotAxisCount>& runtime_axes,
    Operation operation) {
    if (cycle_data.robot_axis_count > robot_interface::kMaxRobotAxisCount) {
        return RobotCommandResult::kError;
    }

    bool has_error = false;
    for (uint8_t axis_index = 0; axis_index < cycle_data.robot_axis_count; ++axis_index) {
        if (cycle_data.robot_feedback[axis_index].communication_valid == 0 ||
            operation(runtime_axes[axis_index]) == cia402::FbStatus::kError) {
            has_error = true;
        }
    }

    return has_error ? RobotCommandResult::kError : RobotCommandResult::kSuccess;
}

}  // namespace

bool RobotCommandCycleResult::success() const noexcept {
    return clear_error == RobotCommandResult::kSuccess && power == RobotCommandResult::kSuccess &&
           switch_mode == RobotCommandResult::kSuccess && home == RobotCommandResult::kSuccess;
}

bool RobotPdoBridge::Configure(uint32_t cycle_time_ns, std::vector<RobotAxisPdoBinding> bindings) {
    if (cycle_time_ns == 0 || bindings.size() > robot_interface::kMaxRobotAxisCount) {
        return false;
    }

    for (std::size_t index = 0; index < bindings.size(); ++index) {
        if (!bindings[index].device || bindings[index].logical_axis_index != index) {
            return false;
        }
    }

    Reset();
    bindings_ = std::move(bindings);
    cycle_data_.robot_axis_count = static_cast<uint8_t>(bindings_.size());
    cycle_data_.cycle_time_ns = cycle_time_ns;
    return true;
}

void RobotPdoBridge::UpdateFeedbackFromPdo(bool domain_data_valid) noexcept {
    cycle_data_.cycle_count = next_cycle_count_++;

    for (const RobotAxisPdoBinding& binding : bindings_) {
        const uint8_t axis_index = binding.logical_axis_index;
        const device::Cia402StandardPdoData& pdo = binding.device->cyclic_data();
        cia402::AxisData& runtime_axis = runtime_axes_[axis_index];
        robot_interface::AxisFeedback& feedback = cycle_data_.robot_feedback[axis_index];

        // 步骤 1：把 Runtime 所需的原始 CiA402 输入从 TxPDO 导入 AxisData。
        runtime_axis.inData.statusword = pdo.statusword;
        runtime_axis.inData.mode_display = pdo.mode_display;

        // 步骤 2：把算法可见的标准运动反馈写入公共周期数据。
        feedback.actual_position = pdo.actual_position;
        feedback.actual_velocity = pdo.actual_velocity;
        feedback.actual_torque = pdo.actual_torque;
        feedback.error_code = pdo.error_code;
        feedback.active_mode = static_cast<robot_interface::RobotMode>(pdo.mode_display);

        // 步骤 3：组合 Domain 完整性和从站 OP 状态生成轴通信有效标志。
        const bool communication_valid =
            domain_data_valid && binding.device->communication_operational();
        feedback.communication_valid = communication_valid ? 1U : 0U;
        feedback.enabled = communication_valid && cia402::GetAxisState(runtime_axis) ==
                                                      cia402::AxisState::kOperationEnabled
                               ? 1U
                               : 0U;

        // 步骤 4：首次取得有效反馈时，以当前位置建立安全的初始运动目标。
        if (communication_valid && !setpoint_initialized_[axis_index]) {
            robot_interface::AxisSetpoint& setpoint = cycle_data_.robot_setpoints[axis_index];
            setpoint.target_position = pdo.actual_position;
            setpoint.target_velocity = 0;
            setpoint.target_torque = 0;
            setpoint_initialized_[axis_index] = true;
        }
    }
}

const RobotCommandCycleResult& RobotPdoBridge::ProcessCommands() {
    const robot_interface::RobotServiceRequest& request = cycle_data_.service;

    // 步骤 1：每周期对全部轴执行或撤销 fault reset。
    last_result_.clear_error =
        ProcessRobotAxes(cycle_data_, runtime_axes_, [&request](cia402::AxisData& axis) {
            return cia402::ClearAxisError(axis, request.clear_error != 0);
        });

    // 步骤 2：power_request_valid 未置位时不改变使能状态机输出。
    if (request.power_request_valid != 0) {
        last_result_.power =
            ProcessRobotAxes(cycle_data_, runtime_axes_, [&request](cia402::AxisData& axis) {
                return cia402::PowerAxis(axis, request.power_enable != 0);
            });
    } else {
        last_result_.power = RobotCommandResult::kSuccess;
    }

    // 步骤 3：switch_mode 置位时对所有轴请求相同的目标模式。
    if (request.switch_mode != 0) {
        last_result_.switch_mode =
            ProcessRobotAxes(cycle_data_, runtime_axes_, [&request](cia402::AxisData& axis) {
                return cia402::SwitchMode(axis, static_cast<cia402::AxisMode>(request.target_mode));
            });
    } else {
        last_result_.switch_mode = RobotCommandResult::kSuccess;
    }

    // 步骤 4：每周期对全部轴执行或撤销 Homing start。
    last_result_.home =
        ProcessRobotAxes(cycle_data_, runtime_axes_, [&request](cia402::AxisData& axis) {
            return cia402::Homing(axis, request.home != 0, false);
        });

    // 步骤 5：将算法运动目标和 CiA402 输出写入设备 RxPDO 缓冲。
    for (const RobotAxisPdoBinding& binding : bindings_) {
        const uint8_t axis_index = binding.logical_axis_index;
        const robot_interface::AxisSetpoint& setpoint = cycle_data_.robot_setpoints[axis_index];
        const cia402::AxisData& runtime_axis = runtime_axes_[axis_index];
        device::Cia402StandardPdoData& pdo = binding.device->cyclic_data();

        pdo.target_position = setpoint.target_position;
        pdo.target_velocity = setpoint.target_velocity;
        pdo.target_torque = setpoint.target_torque;
        pdo.controlword = runtime_axis.outData.controlword;
        pdo.mode = runtime_axis.outData.mode;
    }

    return last_result_;
}

robot_interface::RobotCycleData& RobotPdoBridge::cycle_data() noexcept {
    return cycle_data_;
}

const robot_interface::RobotCycleData& RobotPdoBridge::cycle_data() const noexcept {
    return cycle_data_;
}

void RobotPdoBridge::Reset() noexcept {
    bindings_.clear();
    cycle_data_ = {};
    runtime_axes_ = {};
    setpoint_initialized_ = {};
    last_result_ = {};
    next_cycle_count_ = 0;
}

}  // namespace orchestrator
