#ifndef IGH_ORCHESTRATOR_ROBOT_PDO_BRIDGE_H
#define IGH_ORCHESTRATOR_ROBOT_PDO_BRIDGE_H

#include <array>
#include <cstdint>
#include <vector>

#include "cia402/cia402.h"
#include "device/cia402_standard_pdo_device.h"
#include "robot_data.h"

namespace orchestrator {

/**
 * @brief 一个机器人逻辑轴到标准 CiA402 PDO 设备的内部绑定。
 */
struct RobotAxisPdoBinding {
    uint8_t logical_axis_index = 0;                     // RobotCycleData 逻辑轴下标。
    device::Cia402StandardPdoDevice* device = nullptr;  // IghMaster 持有的设备观察指针。
};

/**
 * @brief 桥接层单类命令的聚合结果。
 */
enum class RobotCommandResult : uint8_t {
    kSuccess = 0,  // 本周期全部参与轴均正常处理。
    kError = 1,    // 至少一个轴的 CiA402 命令转换失败。
};

/**
 * @brief 本周期四类机器人命令的独立结果。
 *
 * 四个动作允许在同一周期同时处理，因此分别保存结果，不使用互斥 action
 * 或 switch-case。单周期 kError 不会中断 EtherCAT 收发。
 */
struct RobotCommandCycleResult {
    RobotCommandResult clear_error = RobotCommandResult::kSuccess;
    RobotCommandResult power = RobotCommandResult::kSuccess;
    RobotCommandResult switch_mode = RobotCommandResult::kSuccess;
    RobotCommandResult home = RobotCommandResult::kSuccess;

    /**
     * @brief 判断本周期全部机器人命令是否成功执行。
     *
     * @return true 四个调用均返回 kSuccess。
     * @return false 至少一个调用返回 kError。
     */
    bool success() const noexcept;
};

/**
 * @brief 公共机器人周期数据与标准 CiA402 PDO 之间的内部实时桥接。
 *
 * 本类在初始化阶段接收已排序的逻辑轴绑定；实时周期中先把 TxPDO 反馈
 * 导入 RobotCycleData 和私有 AxisData，再直接调度四类多轴 CiA402
 * 命令，最后把控制输出和运动目标写回设备 RxPDO 缓冲。
 *
 * 算法同步回调后续插入 UpdateFeedbackFromPdo() 与
 * ProcessCommands() 之间，不改变本桥接接口。
 */
class RobotPdoBridge {
public:
    /**
     * @brief 配置逻辑轴绑定和标称周期。
     *
     * bindings 必须按 logical_axis_index 从 0 连续排列。本函数只在主站
     * 初始化阶段调用，不访问 PDO。
     *
     * @param cycle_time_ns 标称 EtherCAT 周期，单位为纳秒。
     * @param bindings 已注册机器人轴的 PDO 设备绑定。
     * @return true 参数有效且桥接配置完成。
     * @return false 周期为 0、轴数量越界、空指针或轴编号不连续。
     */
    bool Configure(uint32_t cycle_time_ns, std::vector<RobotAxisPdoBinding> bindings);

    /**
     * @brief 将本周期 TxPDO 反馈导入公共周期数据和私有 CiA402 轴数据。
     *
     * 同时更新公共 AxisFeedback、AxisData 的 statusword/mode_display、
     * enabled、communication_valid 和 cycle_count。本函数不修改算法
     * 拥有的 setpoint 或 service。
     *
     * @param domain_data_valid 最近一次 Domain working counter 是否完整。
     */
    void UpdateFeedbackFromPdo(bool domain_data_valid) noexcept;

    /**
     * @brief 执行全部多轴命令并把结果导出到设备 PDO 缓冲。
     *
     * 固定调用顺序为清错、使能、切模式、回零。每个函数都会读取自身的
     * service 标志，因此多个请求可以在同一周期同时执行。随后将 setpoint、
     * controlword 和 mode 写入 Cia402StandardPdoData。
     *
     * @return 本周期四类命令的独立结果。
     */
    const RobotCommandCycleResult& ProcessCommands();

    /**
     * @brief 返回后续算法同步回调使用的公共周期数据。
     *
     * @return 桥接层持有的 RobotCycleData 可写引用。
     */
    robot_interface::RobotCycleData& cycle_data() noexcept;

    /**
     * @brief 返回公共周期数据的只读引用。
     *
     * @return 桥接层持有的 RobotCycleData 只读引用。
     */
    const robot_interface::RobotCycleData& cycle_data() const noexcept;

    /**
     * @brief 清除轴绑定、CiA402 状态和周期计数。
     */
    void Reset() noexcept;

private:
    std::vector<RobotAxisPdoBinding> bindings_{};  // 按逻辑轴编号连续排列的设备绑定。
    robot_interface::RobotCycleData cycle_data_{};  // IgH 与后续算法交换的公共周期数据。
    std::array<cia402::AxisData, robot_interface::kMaxRobotAxisCount>
        runtime_axes_{};                     // 只在桥接层内部使用的 CiA402 轴状态。
    RobotCommandCycleResult last_result_{};  // 最近一次四类命令的独立结果。
    uint64_t next_cycle_count_ = 0;          // 下一个写入公共数据的周期号。
};

}  // namespace orchestrator

#endif
