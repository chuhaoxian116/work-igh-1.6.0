#ifndef IGH_DEVICE_GSD620_DEVICE_H
#define IGH_DEVICE_GSD620_DEVICE_H

#include <array>
#include <cstdint>

#include "device/igh_device.h"

namespace device {

/** @brief GSD620 从站的静态配置。 */
struct Gsd620Configuration {
    uint16_t alias = 0;                   // EtherCAT alias 地址。
    uint16_t position = 0;                // EtherCAT 环网从站位置。
    uint32_t vendor_id = 0x00000911U;     // GSD620 厂商 ID。
    uint32_t product_code = 0x00000620U;  // GSD620 产品代码。

    uint16_t dc_assign_activate = 0x0300U;  // 来自设备 ESI 的 DC AssignActivate。
    int32_t sync0_shift_ns = 0;             // SYNC0 相对 application time 的相位偏移。
    bool enable_dc = true;                  // 是否配置 SYNC0 DC。
};

/** @brief GSD620 当前周期的私有 PDO 数据。 */
struct Gsd620CyclicData {
    int32_t target_position = 0;  // 0x607A:00 目标位置。
    int32_t target_velocity = 0;  // 0x60FF:00 目标速度。
    uint16_t controlword = 0;     // 0x6040:00 控制字。
    int16_t target_torque = 0;    // 0x6071:00 目标转矩。
    int8_t mode = 0;              // 0x6060:00 目标运行模式。

    int32_t actual_position = 0;  // 0x6064:00 实际位置。
    uint16_t error_code = 0;      // 0x603F:00 错误码。
    int32_t actual_velocity = 0;  // 0x606C:00 实际速度。
    uint16_t statusword = 0;      // 0x6041:00 状态字。
    int16_t actual_torque = 0;    // 0x6077:00 实际转矩。
    int8_t mode_display = 0;      // 0x6061:00 实际运行模式。
};

/**
 * @brief GSD620 CiA402 伺服的最小 IgH 从站适配器。
 *
 * 本类仅完成 GSD620 的从站识别、PDO/SM、DC 配置与类型化 PDO 映射。
 * 上层后续可通过独立桥接层将 cyclic_data() 映射到 RobotRuntimeData，
 * 本设备类不依赖 CiA402 状态机、RobotRuntime 或机器人业务定义。
 */
class Gsd620Device final : public IghDevice {
public:
    /**
   * @brief 使用指定的从站身份和 DC 参数创建 GSD620 适配器。
   *
   * 此时不访问 EtherCAT 总线；实际从站和 PDO 配置在 Configure() 中完成。
   *
   * @param configuration GSD620 的从站身份、DC 和 SYNC0 配置。
   */
    explicit Gsd620Device(Gsd620Configuration configuration = {});

    /**
   * @brief 创建 GSD620 从站配置，设置 PDO 映射、PDO entry 和 DC。
   *
   * 仅由 IghMaster 在激活前调用一次。失败时会重置本设备的 IgH 配置
   * 句柄和 PDO offset，不会激活 master。
   *
   * @param configuration 由 IghMaster 提供的 master、domain 和周期配置。
   * @return true 从站、PDO 和 DC 均配置成功。
   * @return false 参数无效或任一 IgH 配置调用失败。
   */
    bool Configure(const DeviceConfiguration& configuration) override;

    /**
   * @brief 返回 Configure() 创建的 IgH 从站配置句柄。
   *
   * IghMaster 使用该句柄选择 DC 参考时钟；未配置或已 Reset() 时返回
   * nullptr。
   *
   * @return 当前 GSD620 的 IgH 从站配置句柄；未配置时为 nullptr。
   */
    ec_slave_config_t* slave_config() const override;

    /**
   * @brief 从当前 domain process data 读取 GSD620 的全部已注册 TxPDO。
   *
   * 本函数只进行无分配、无锁的内存映射，必须由实时周期线程调用。
   *
   * @param domain_pd IghMaster 激活后取得的 domain process data 基地址。
   */
    void ReadProcessData(const uint8_t* domain_pd) noexcept override;

    /**
   * @brief 将当前命令和 CiA402 输出写入 GSD620 的已注册 RxPDO。
   *
   * 本函数只进行无分配、无锁的内存映射，必须由实时周期线程调用。
   *
   * @param domain_pd IghMaster 激活后取得的可写 domain process data 基地址。
   */
    void WriteProcessData(uint8_t* domain_pd) noexcept override;

    /**
   * @brief 清除与某个 IgH master 绑定的句柄、PDO offset 和周期数据。
   *
   * 由 IghMaster 在配置失败、显式 Release() 或析构时调用。
   */
    void Reset() noexcept override;

    /**
   * @brief 返回供 IgH 上层桥接使用的可读写 PDO 周期数据。
   *
   * @return GSD620 私有周期数据的可写引用。
   */
    Gsd620CyclicData& cyclic_data() { return cyclic_data_; }

    /**
   * @brief 返回供诊断使用的只读 PDO 周期数据。
   *
   * @return GSD620 私有周期数据的只读引用。
   */
    const Gsd620CyclicData& cyclic_data() const { return cyclic_data_; }

    /**
   * @brief 返回创建时保存的 GSD620 静态配置。
   *
   * @return GSD620 静态配置的只读引用。
   */
    const Gsd620Configuration& configuration() const { return configuration_; }

private:
    struct PdoOffsets {
        unsigned int target_position = 0;  // 0x607A:00。
        unsigned int target_velocity = 0;  // 0x60FF:00。
        unsigned int controlword = 0;      // 0x6040:00。
        unsigned int target_torque = 0;    // 0x6071:00。
        unsigned int mode = 0;             // 0x6060:00。
        unsigned int actual_position = 0;  // 0x6064:00。
        unsigned int error_code = 0;       // 0x603F:00。
        unsigned int actual_velocity = 0;  // 0x606C:00。
        unsigned int statusword = 0;       // 0x6041:00。
        unsigned int actual_torque = 0;    // 0x6077:00。
        unsigned int mode_display = 0;     // 0x6061:00。
    };

    static constexpr std::size_t kPdoEntryCount = 11;  // 当前注册的 RxPDO 与 TxPDO entry 总数。

    /**
     * @brief 按静态配置生成以全零元素结尾的 IgH PDO entry 注册表。
     */
    void BuildPdoEntryRegistrations() noexcept;

    Gsd620Configuration configuration_{};        // 构造时确定的从站身份和 DC 配置。
    ec_slave_config_t* slave_config_ = nullptr;  // Configure() 从 IgH 取得的从站配置句柄。
    PdoOffsets pdo_offsets_{};  // IgH 注册 PDO entry 后写入的 process data 字节偏移。
    std::array<ec_pdo_entry_reg_t, kPdoEntryCount + 1>
        pdo_entry_regs_{};            // 含结束标记的 IgH PDO entry 注册表。
    Gsd620CyclicData cyclic_data_{};  // 本设备私有的当前周期输入反馈和输出命令。
};

}  // namespace device

#endif
