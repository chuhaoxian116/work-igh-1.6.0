#ifndef IGH_ORCHESTRATOR_ROBOT_ETHERCAT_ORCHESTRATOR_H
#define IGH_ORCHESTRATOR_ROBOT_ETHERCAT_ORCHESTRATOR_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "device/gsd620_device.h"
#include "master/igh_master.h"

namespace orchestrator {

/** @brief 单台 GSD620 机器人 EtherCAT 编排层的静态配置。 */
struct RobotEthercatConfiguration {
  uint32_t master_index = 0;          // IgH master 编号。
  uint32_t cycle_time_ns = 1'000'000; // EtherCAT 标称周期，单位为纳秒。
  bool synchronize_dc = true;         // 每周期是否发送 DC 同步报文。

  device::Gsd620Configuration gsd620{}; // 当前机器人使用的 GSD620 从站配置。
};

/** @brief 编排层接口调用结果。 */
enum class OrchestratorResult : uint8_t {
  Success = 0,      // 本次调用成功。
  InvalidState = 1, // 当前生命周期状态不允许调用。
  MasterError = 2,  // IghMaster 的配置、激活或周期调用失败。
};

/** @brief 供非实时线程读取的 GSD620 反馈快照。 */
struct Gsd620FeedbackSnapshot {
  int32_t actual_position = 0;      // 0x6064:00 实际位置。
  int32_t actual_velocity = 0;      // 0x606C:00 实际速度。
  int16_t actual_torque = 0;        // 0x6077:00 实际转矩。
  uint16_t error_code = 0;          // 0x603F:00 错误码。
  uint16_t statusword = 0;          // 0x6041:00 状态字。
  int8_t mode_display = 0;          // 0x6061:00 实际运行模式。
};

/**
 * @brief 单 GSD620 机器人应用的 EtherCAT 编排层。
 *
 * 该类在业务层组合具体从站与通用 IghMaster，负责固定的生命周期和单次
 * 周期调用顺序：ReceiveAndProcess() -> 业务数据处理位置 -> QueueAndSend()。
 * 当前版本不创建线程、不 sleep、不调用算法，也不调用 RobotRuntime。
 */
class RobotEthercatOrchestrator {
public:
  /**
   * @brief 使用指定静态配置构造编排层对象。
   *
   * 构造阶段不请求 IgH master，也不访问 EtherCAT 总线。
   *
   * @param configuration 主站周期、DC 和 GSD620 从站配置。
   */
  explicit RobotEthercatOrchestrator(RobotEthercatConfiguration configuration);

  ~RobotEthercatOrchestrator();

  RobotEthercatOrchestrator(const RobotEthercatOrchestrator &) = delete;
  RobotEthercatOrchestrator &operator=(const RobotEthercatOrchestrator &) =
      delete;

  /**
   * @brief 创建主站，注册 GSD620，完成 PDO/DC 配置并激活主站。
   *
   * 成功后 IghMaster 独占 GSD620 设备对象；本类仅保留非拥有型观察指针，
   * 供后续业务数据桥接使用。
   *
   * @return Success 主站和 GSD620 已激活。
   * @return InvalidState 已初始化或配置周期为 0。
   * @return MasterError 注册、配置或激活 IgH master 失败。
   */
  OrchestratorResult Initialize();

  /**
   * @brief 执行一次 EtherCAT 周期收发。
   *
   * 由唯一的实时周期线程调用。当前实现只保留 PDO 读取与写入之间的业务
   * 编排位置；后续 Runtime/算法桥接应加入该位置，不应加入 IghMaster。
   *
   * @param application_time_ns 本周期的单调时钟时间，单位为纳秒。
   * @return Success 本周期 PDO 收发完成。
   * @return InvalidState 主站尚未激活。
   * @return MasterError IgH 周期调用失败。
   */
  OrchestratorResult RunCycle(uint64_t application_time_ns);

  /**
   * @brief 停止并释放从站与 IgH master 资源。
   *
   * 调用后可再次调用 Initialize() 建立新的主站实例；本函数可重复调用。
   */
  void Shutdown();

  /**
   * @brief 返回已注册的 GSD620 从站观察指针。
   *
   * 该指针由 IghMaster 持有，调用方不得释放；Shutdown() 后返回 nullptr。
   *
   * @return 已初始化时的 GSD620 从站指针，否则为 nullptr。
   */
  device::Gsd620Device *gsd620_device() { return gsd620_device_; }

  /**
   * @brief 返回只读 GSD620 从站观察指针。
   *
   * @return 已初始化时的 GSD620 从站指针，否则为 nullptr。
   */
  const device::Gsd620Device *gsd620_device() const {
    return gsd620_device_;
  }

  /**
   * @brief 返回内部主站观察指针。
   *
   * 调用方不得释放该指针，也不得在编排层外调用其周期函数。
   *
   * @return 已初始化时的 IghMaster 指针，否则为 nullptr。
   */
  const master::IghMaster *master() const { return master_.get(); }

  /**
   * @brief 无锁读取周期线程发布的最新 GSD620 反馈快照。
   *
   * 本函数可由主线程或诊断线程调用；读取过程不会访问正在被周期线程修改
   * 的 Gsd620CyclicData，而是通过序列锁风格的原子快照取得一致数据。
   *
   * @param snapshot 用于接收反馈数据的输出对象。
   * @return true 取得了一份一致的反馈快照。
   * @return false 主站未初始化或读取期间快照持续更新。
   */
  bool ReadGsd620Feedback(Gsd620FeedbackSnapshot &snapshot) const noexcept;

private:
  /** @brief 将当前周期读取到的 GSD620 数据发布为非实时诊断快照。 */
  void PublishGsd620Feedback() noexcept;

  RobotEthercatConfiguration configuration_{}; // 构造时确定的主站和从站配置。
  std::unique_ptr<master::IghMaster> master_;  // 编排层独占的通用 IgH 主站。
  device::Gsd620Device *gsd620_device_ = nullptr; // 由 IghMaster 独占的从站观察指针。

  std::atomic<uint64_t> feedback_sequence_{0}; // 奇数表示周期线程正在更新反馈快照。
  std::atomic_bool feedback_valid_{false};     // 至少完成一次有效 PDO 读取后置为 true。
  std::atomic<int32_t> actual_position_{0};    // 已发布的实际位置。
  std::atomic<int32_t> actual_velocity_{0};    // 已发布的实际速度。
  std::atomic<int16_t> actual_torque_{0};      // 已发布的实际转矩。
  std::atomic<uint16_t> error_code_{0};        // 已发布的驱动错误码。
  std::atomic<uint16_t> statusword_{0};        // 已发布的状态字。
  std::atomic<int8_t> mode_display_{0};        // 已发布的实际运行模式。
};

} // namespace orchestrator

#endif
