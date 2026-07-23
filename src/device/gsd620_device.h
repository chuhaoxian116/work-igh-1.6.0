#ifndef IGH_DEVICE_GSD620_DEVICE_H
#define IGH_DEVICE_GSD620_DEVICE_H

#include <array>
#include <cstdint>

#include <cia402/cia402.h>

#include "device/igh_device.h"

namespace device {

/** @brief GSD620 从站的静态配置。 */
struct Gsd620Configuration {
    uint16_t alias = 0;                  // EtherCAT alias 地址。
    uint16_t position = 0;               // EtherCAT 环网从站位置。
    uint32_t vendor_id = 0x00000911U;    // GSD620 厂商 ID。
    uint32_t product_code = 0x00000620U; // GSD620 产品代码。

    uint16_t dc_assign_activate = 0x0300U; // 来自设备 ESI 的 DC AssignActivate。
    int32_t sync0_shift_ns = 0;            // SYNC0 相对 application time 的相位偏移。
    bool enable_dc = true;                 // 是否配置 SYNC0 DC。
};

/** @brief GSD620 当前周期的私有 PDO 数据。 */
struct Gsd620CyclicData {
    cia402::AxisData axis{};       // 供 CiA402 与 RobotRuntime 使用的状态字、模式和控制字。

    int32_t target_position = 0;   // 0x607A:00 目标位置。
    int32_t target_velocity = 0;   // 0x60FF:00 目标速度。
    int16_t target_torque = 0;     // 0x6071:00 目标转矩。

    int32_t actual_position = 0;   // 0x6064:00 实际位置。
    int32_t actual_velocity = 0;   // 0x606C:00 实际速度。
    int16_t actual_torque = 0;     // 0x6077:00 实际转矩。
    uint16_t error_code = 0;       // 0x603F:00 错误码。
};

/**
 * @brief GSD620 CiA402 伺服的最小 IgH 从站适配器。
 *
 * 本类仅完成 GSD620 的从站识别、PDO/SM、DC 配置与类型化 PDO 映射。
 * 上层后续可通过 cyclic_data() 将这些私有数据映射到 RobotRuntimeData。
 */
class Gsd620Device final : public IghDevice {
public:
    explicit Gsd620Device(Gsd620Configuration configuration = {});

    bool Configure(const DeviceConfiguration &configuration) override;
    ec_slave_config_t *slave_config() const override;
    void ReadProcessData(const uint8_t *domain_pd) noexcept override;
    void WriteProcessData(uint8_t *domain_pd) noexcept override;
    void Reset() noexcept override;

    Gsd620CyclicData &cyclic_data() { return cyclic_data_; }
    const Gsd620CyclicData &cyclic_data() const { return cyclic_data_; }
    const Gsd620Configuration &configuration() const { return configuration_; }

private:
    struct PdoOffsets {
        unsigned int target_position = 0;      // 0x607A:00。
        unsigned int target_velocity = 0;      // 0x60FF:00。
        unsigned int controlword = 0;          // 0x6040:00。
        unsigned int target_torque = 0;        // 0x6071:00。
        unsigned int mode = 0;                 // 0x6060:00。
        unsigned int actual_position = 0;      // 0x6064:00。
        unsigned int error_code = 0;           // 0x603F:00。
        unsigned int actual_velocity = 0;      // 0x606C:00。
        unsigned int statusword = 0;           // 0x6041:00。
        unsigned int actual_torque = 0;        // 0x6077:00。
        unsigned int mode_display = 0;         // 0x6061:00。
    };

    static constexpr std::size_t kPdoEntryCount = 11;

    void BuildPdoEntryRegistrations() noexcept;

    Gsd620Configuration configuration_{};
    ec_slave_config_t *slave_config_ = nullptr;
    PdoOffsets pdo_offsets_{};
    std::array<ec_pdo_entry_reg_t, kPdoEntryCount + 1> pdo_entry_regs_{};
    Gsd620CyclicData cyclic_data_{};
};

}  // namespace device

#endif
