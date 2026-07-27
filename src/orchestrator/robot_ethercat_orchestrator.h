#ifndef IGH_ORCHESTRATOR_ROBOT_ETHERCAT_ORCHESTRATOR_H
#define IGH_ORCHESTRATOR_ROBOT_ETHERCAT_ORCHESTRATOR_H

#include <cstdint>
#include <memory>

#include "device/device_setup.h"
#include "master/igh_master.h"

namespace orchestrator {

class RobotPdoBridge;

/** @brief 机器人 EtherCAT 编排层的主站静态配置。 */
struct RobotEthercatConfiguration {
    uint32_t master_index = 0;           // IgH master 编号。
    uint32_t cycle_time_ns = 1'000'000;  // EtherCAT 标称周期，单位为纳秒。
    bool synchronize_dc = true;          // 每周期是否发送 DC 同步报文。
};

/** @brief 编排层接口调用结果。 */
enum class OrchestratorResult : uint8_t {
    kSuccess = 0,               // 本次调用成功。
    kInvalidState = 1,          // 当前生命周期状态不允许调用。
    kMasterError = 2,           // IghMaster 的配置、激活或周期调用失败。
    kInvalidConfiguration = 3,  // 设备角色、逻辑轴编号或参考时钟定义无效。
};

/**
 * @brief 与具体从站类型无关的机器人 EtherCAT 编排层。
 *
 * 该类接收由应用入口组装的设备定义，通过通用 IghMaster 统一注册全部
 * 从站，并负责固定的生命周期和单次周期调用顺序：
 * ReceiveAndProcess() -> PDO/命令桥接 -> QueueAndSend()。
 * 当前版本不创建线程、不 sleep，也不调用算法。
 */
class RobotEthercatOrchestrator {
public:
    /**
     * @brief 使用主站配置和设备定义构造编排层对象。
     *
     * 构造阶段仅保存配置和设备工厂，不创建具体设备，也不访问 EtherCAT
     * 总线。设备定义可包含任意 IghDevice 派生类型。
     *
     * @param configuration 主站周期和 DC 同步配置。
     * @param device_definitions 需要初始化的全部 EtherCAT 设备定义。
     */
    RobotEthercatOrchestrator(RobotEthercatConfiguration configuration,
                              device::DeviceDefinitions device_definitions);

    ~RobotEthercatOrchestrator();

    RobotEthercatOrchestrator(const RobotEthercatOrchestrator&) = delete;
    RobotEthercatOrchestrator& operator=(const RobotEthercatOrchestrator&) = delete;

    /**
     * @brief 创建主站，注册全部设备，完成 PDO/DC 配置并激活主站。
     *
     * 设备按照定义顺序创建和注册。最多允许一个定义标记为 DC 参考时钟；
     * 机器人轴编号必须从 0 连续排列、不能重复，并且对应设备必须继承
     * Cia402StandardPdoDevice。
     *
     * @return kSuccess 主站和全部设备已激活。
     * @return kInvalidState 已初始化、配置周期为 0 或设备列表为空。
     * @return kInvalidConfiguration 设备角色、轴编号或参考时钟定义无效。
     * @return kMasterError 设备注册、配置或激活 IgH master 失败。
     */
    OrchestratorResult Initialize();

    /**
     * @brief 执行一次 EtherCAT 周期收发。
     *
     * 由唯一的实时周期线程调用。读取 TxPDO 后更新公共反馈，预留算法同步
     * 回调位置，再通过内部 CiA402 调度生成控制字并写入设备 RxPDO 缓冲。
     *
     * @param application_time_ns 本周期的单调时钟时间，单位为纳秒。
     * @return kSuccess 本周期 PDO 收发完成。
     * @return kInvalidState 主站尚未激活。
     * @return kMasterError IgH 周期调用失败。
     */
    OrchestratorResult RunCycle(uint64_t application_time_ns);

    /**
     * @brief 停止并释放从站与 IgH master 资源。
     *
     * 调用后可再次调用 Initialize() 建立新的主站实例；本函数可重复调用。
     */
    void Shutdown();

    /**
     * @brief 返回内部主站观察指针。
     *
     * 调用方不得释放该指针，也不得在编排层外调用其周期函数。
     *
     * @return 已初始化时的 IghMaster 指针，否则为 nullptr。
     */
    const master::IghMaster* master() const { return master_.get(); }

private:
    RobotEthercatConfiguration configuration_{};      // 构造时确定的主站配置。
    device::DeviceDefinitions device_definitions_{};  // 可重复创建设备的通用定义。
    std::unique_ptr<master::IghMaster> master_;       // 编排层独占的通用 IgH 主站。
    std::unique_ptr<RobotPdoBridge> pdo_bridge_;  // 对外隐藏 CiA402 的内部周期桥接。
};

}  // namespace orchestrator

#endif
