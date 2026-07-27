#ifndef IGH_DEVICE_CIA402_STANDARD_PDO_DEVICE_H
#define IGH_DEVICE_CIA402_STANDARD_PDO_DEVICE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "device/igh_device.h"

namespace device {

/**
 * @brief 标准 CiA402 PDO 设备的从站身份与 DC 配置。
 */
struct Cia402StandardPdoConfiguration {
    uint16_t alias = 0;         // EtherCAT alias 地址。
    uint16_t position = 0;      // EtherCAT 环网从站位置。
    uint32_t vendor_id = 0;     // 从站厂商 ID。
    uint32_t product_code = 0;  // 从站产品代码。

    uint16_t dc_assign_activate = 0;  // 来自设备 ESI 的 DC AssignActivate。
    int32_t sync0_shift_ns = 0;       // SYNC0 相对 application time 的相位偏移。
    bool enable_dc = false;           // 是否为该从站配置 SYNC0 DC。
};

/**
 * @brief 标准 CiA402 位置、速度、转矩 PDO 周期数据。
 *
 * 本结构只表达 EtherCAT PDO 数据，不实现使能、清错、模式切换或回零等
 * 跨周期 CiA402 状态机。
 */
struct Cia402StandardPdoData {
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
 * @brief 复用标准 CiA402 PDO 映射和实时数据搬运的 IgH 设备基类。
 *
 * 默认 PDO 布局包含控制字、状态字、模式、位置、速度、转矩和错误码。
 * 具体伺服只需要提供从站身份和 DC 参数；PDO 排列不同但仍包含这些标准
 * 对象时，可以覆盖 PdoSyncs()。厂商专有 SDO/PDO 初始化可以放入
 * ConfigureDeviceSpecific()。
 *
 * 本类不依赖 CiA402 状态机库、RobotRuntime 或算法接口。
 */
class Cia402StandardPdoDevice : public IghDevice {
public:
    /**
     * @brief 使用标准从站身份和 DC 参数创建 PDO 设备。
     *
     * 构造阶段不访问 EtherCAT 总线。
     *
     * @param configuration 从站身份、DC 和 SYNC0 配置。
     */
    explicit Cia402StandardPdoDevice(Cia402StandardPdoConfiguration configuration);

    /**
     * @brief 配置从站、标准 PDO、Domain entry、DC 和厂商扩展。
     *
     * @param configuration IghMaster 提供的 master、domain 和周期配置。
     * @return true 全部激活前配置成功。
     * @return false 参数无效或任一 IgH 配置步骤失败。
     */
    bool Configure(const DeviceConfiguration& configuration) override;

    /**
     * @brief 返回 Configure() 取得的 IgH 从站配置句柄。
     *
     * @return 当前从站配置句柄；未配置或 Reset() 后为 nullptr。
     */
    ec_slave_config_t* slave_config() const override;

    /**
     * @brief 从 Domain 读取标准 CiA402 TxPDO 反馈。
     *
     * @param domain_pd 激活后的 Domain process data 基地址。
     */
    void ReadProcessData(const uint8_t* domain_pd) noexcept override;

    /**
     * @brief 向 Domain 写入标准 CiA402 RxPDO 命令。
     *
     * @param domain_pd 激活后的可写 Domain process data 基地址。
     */
    void WriteProcessData(uint8_t* domain_pd) noexcept override;

    /**
     * @brief 清除 IgH 句柄、PDO offset 和周期数据。
     */
    void Reset() noexcept override;

    /**
     * @brief 返回标准 PDO 周期数据的可写引用。
     *
     * @return 当前反馈与下一周期命令。
     */
    Cia402StandardPdoData& cyclic_data() noexcept;

    /**
     * @brief 返回标准 PDO 周期数据的只读引用。
     *
     * @return 当前反馈与下一周期命令。
     */
    const Cia402StandardPdoData& cyclic_data() const noexcept;

    /**
     * @brief 返回创建时保存的标准从站配置。
     *
     * @return 从站身份与 DC 配置。
     */
    const Cia402StandardPdoConfiguration& standard_configuration() const noexcept;

protected:
    /**
     * @brief 返回具体设备使用的 PDO/SM 同步表。
     *
     * 默认返回标准位置、速度、转矩 PDO 布局。覆盖后的布局仍须包含本类
     * 注册和读写的全部标准对象。
     *
     * @return 以 0xFF sync 结束的 IgH 同步表。
     */
    virtual const ec_sync_info_t* PdoSyncs() const noexcept;

    /**
     * @brief 执行厂商专有的激活前配置。
     *
     * 默认实现不执行额外操作并返回 true。派生类可以在这里配置专有 SDO
     * 或注册额外 PDO entry。
     *
     * @param configuration IghMaster 提供的配置上下文。
     * @return true 厂商专有配置成功。
     * @return false 厂商专有配置失败。
     */
    virtual bool ConfigureDeviceSpecific(const DeviceConfiguration& configuration);

    /**
     * @brief 清除派生设备保存的厂商专有配置状态。
     *
     * 默认实现为空。
     */
    virtual void ResetDeviceSpecific() noexcept;

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

    static constexpr std::size_t kPdoEntryCount = 11;  // 标准 RxPDO 与 TxPDO entry 总数。

    /**
     * @brief 建立标准对象与本地 PDO offset 字段的注册关系。
     */
    void BuildPdoEntryRegistrations() noexcept;

    Cia402StandardPdoConfiguration configuration_{};  // 从站身份和 DC 配置。
    ec_slave_config_t* slave_config_ = nullptr;       // IgH 从站配置观察指针。
    PdoOffsets pdo_offsets_{};                        // Domain process data 字节偏移。
    std::array<ec_pdo_entry_reg_t, kPdoEntryCount + 1>
        pdo_entry_regs_{};                 // 含结束标记的 PDO entry 注册表。
    Cia402StandardPdoData cyclic_data_{};  // 当前反馈和下一周期命令。
};

}  // namespace device

#endif
