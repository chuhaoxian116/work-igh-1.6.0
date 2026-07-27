#include "device/cia402_standard_pdo_device.h"

namespace device {
namespace {

constexpr ec_pdo_entry_info_t kRxPdoEntries[] = {
    {0x607A, 0x00, 32},  // Target position.
    {0x60FF, 0x00, 32},  // Target velocity.
    {0x6040, 0x00, 16},  // Controlword.
    {0x6071, 0x00, 16},  // Target torque.
    {0x6060, 0x00, 8},   // Modes of operation.
};

constexpr ec_pdo_entry_info_t kTxPdoEntries[] = {
    {0x6064, 0x00, 32},  // Position actual value.
    {0x603F, 0x00, 16},  // Error code.
    {0x606C, 0x00, 32},  // Velocity actual value.
    {0x6041, 0x00, 16},  // Statusword.
    {0x6077, 0x00, 16},  // Torque actual value.
    {0x6061, 0x00, 8},   // Modes of operation display.
};

constexpr ec_pdo_info_t kPdos[] = {
    {0x1600, 5, kRxPdoEntries},  // RxPDO，SM2。
    {0x1A00, 6, kTxPdoEntries},  // TxPDO，SM3。
};

constexpr ec_sync_info_t kSyncs[] = {
    {0, EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
    {1, EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, &kPdos[0], EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, &kPdos[1], EC_WD_DISABLE},
    {0xFF, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
};

}  // namespace

Cia402StandardPdoDevice::Cia402StandardPdoDevice(Cia402StandardPdoConfiguration configuration)
    : configuration_(configuration) {}

bool Cia402StandardPdoDevice::Configure(const DeviceConfiguration& configuration) {
    // 步骤 1：检查主站上下文、从站身份以及启用 DC 时需要的周期参数。
    if (!configuration.master || !configuration.domain || configuration_.vendor_id == 0 ||
        configuration_.product_code == 0 ||
        (configuration_.enable_dc && configuration.cycle_time_ns == 0)) {
        return false;
    }

    // 步骤 2：根据位置和设备身份取得 IgH 从站配置对象。
    slave_config_ = ecrt_master_slave_config(configuration.master,
                                             configuration_.alias,
                                             configuration_.position,
                                             configuration_.vendor_id,
                                             configuration_.product_code);
    if (!slave_config_) {
        return false;
    }

    // 步骤 3：使用标准或派生类提供的同步表配置 SM/PDO 映射。
    const ec_sync_info_t* const syncs = PdoSyncs();
    if (!syncs || ecrt_slave_config_pdos(slave_config_, EC_END, syncs)) {
        Reset();
        return false;
    }

    // 步骤 4：注册标准 PDO entry，由 Domain 计算实时过程数据偏移。
    BuildPdoEntryRegistrations();
    if (ecrt_domain_reg_pdo_entry_list(configuration.domain, pdo_entry_regs_.data())) {
        Reset();
        return false;
    }

    // 步骤 5：仅对明确启用 DC 的从站配置 SYNC0。
    if (configuration_.enable_dc && ecrt_slave_config_dc(slave_config_,
                                                         configuration_.dc_assign_activate,
                                                         configuration.cycle_time_ns,
                                                         configuration_.sync0_shift_ns,
                                                         0,
                                                         0)) {
        Reset();
        return false;
    }

    // 步骤 6：执行具体伺服覆盖的厂商专有 SDO/PDO 初始化。
    if (!ConfigureDeviceSpecific(configuration)) {
        Reset();
        return false;
    }

    return true;
}

ec_slave_config_t* Cia402StandardPdoDevice::slave_config() const {
    return slave_config_;
}

void Cia402StandardPdoDevice::ReadProcessData(const uint8_t* domain_pd) noexcept {
    if (!domain_pd) {
        return;
    }

    // 从 Domain 的标准 TxPDO 区域读取本周期伺服反馈。
    cyclic_data_.actual_position = EC_READ_S32(domain_pd + pdo_offsets_.actual_position);
    cyclic_data_.actual_velocity = EC_READ_S32(domain_pd + pdo_offsets_.actual_velocity);
    cyclic_data_.actual_torque = EC_READ_S16(domain_pd + pdo_offsets_.actual_torque);
    cyclic_data_.error_code = EC_READ_U16(domain_pd + pdo_offsets_.error_code);
    cyclic_data_.statusword = EC_READ_U16(domain_pd + pdo_offsets_.statusword);
    cyclic_data_.mode_display = EC_READ_S8(domain_pd + pdo_offsets_.mode_display);
}

void Cia402StandardPdoDevice::WriteProcessData(uint8_t* domain_pd) noexcept {
    if (!domain_pd) {
        return;
    }

    // 将下一周期伺服命令写入 Domain 的标准 RxPDO 区域。
    EC_WRITE_S32(domain_pd + pdo_offsets_.target_position, cyclic_data_.target_position);
    EC_WRITE_S32(domain_pd + pdo_offsets_.target_velocity, cyclic_data_.target_velocity);
    EC_WRITE_U16(domain_pd + pdo_offsets_.controlword, cyclic_data_.controlword);
    EC_WRITE_S16(domain_pd + pdo_offsets_.target_torque, cyclic_data_.target_torque);
    EC_WRITE_S8(domain_pd + pdo_offsets_.mode, cyclic_data_.mode);
}

void Cia402StandardPdoDevice::Reset() noexcept {
    // 步骤 1：先让派生类清除厂商专有的 IgH 配置状态。
    ResetDeviceSpecific();

    // 步骤 2：清除随 master 释放而失效的标准句柄、offset 和注册表。
    slave_config_ = nullptr;
    pdo_offsets_ = {};
    pdo_entry_regs_ = {};

    // 步骤 3：清空反馈和命令，避免重新初始化时沿用旧周期数据。
    cyclic_data_ = {};
}

Cia402StandardPdoData& Cia402StandardPdoDevice::cyclic_data() noexcept {
    return cyclic_data_;
}

const Cia402StandardPdoData& Cia402StandardPdoDevice::cyclic_data() const noexcept {
    return cyclic_data_;
}

const Cia402StandardPdoConfiguration& Cia402StandardPdoDevice::standard_configuration()
    const noexcept {
    return configuration_;
}

bool Cia402StandardPdoDevice::communication_operational() const noexcept {
    if (!slave_config_) {
        return false;
    }

    ec_slave_config_state_t state{};
    return ecrt_slave_config_state(slave_config_, &state) == 0 && state.online && state.operational;
}

const ec_sync_info_t* Cia402StandardPdoDevice::PdoSyncs() const noexcept {
    return kSyncs;
}

bool Cia402StandardPdoDevice::ConfigureDeviceSpecific(const DeviceConfiguration& configuration) {
    (void)configuration;
    return true;
}

void Cia402StandardPdoDevice::ResetDeviceSpecific() noexcept {}

void Cia402StandardPdoDevice::BuildPdoEntryRegistrations() noexcept {
    const uint16_t alias = configuration_.alias;
    const uint16_t position = configuration_.position;
    const uint32_t vendor_id = configuration_.vendor_id;
    const uint32_t product_code = configuration_.product_code;

    pdo_entry_regs_ = {{
        {alias,
         position,
         vendor_id,
         product_code,
         0x607A,
         0x00,
         &pdo_offsets_.target_position,
         nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x60FF,
         0x00,
         &pdo_offsets_.target_velocity,
         nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x6040,
         0x00,
         &pdo_offsets_.controlword,
         nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x6071,
         0x00,
         &pdo_offsets_.target_torque,
         nullptr},
        {alias, position, vendor_id, product_code, 0x6060, 0x00, &pdo_offsets_.mode, nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x6064,
         0x00,
         &pdo_offsets_.actual_position,
         nullptr},
        {alias, position, vendor_id, product_code, 0x603F, 0x00, &pdo_offsets_.error_code, nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x606C,
         0x00,
         &pdo_offsets_.actual_velocity,
         nullptr},
        {alias, position, vendor_id, product_code, 0x6041, 0x00, &pdo_offsets_.statusword, nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x6077,
         0x00,
         &pdo_offsets_.actual_torque,
         nullptr},
        {alias,
         position,
         vendor_id,
         product_code,
         0x6061,
         0x00,
         &pdo_offsets_.mode_display,
         nullptr},
        {},
    }};
}

}  // namespace device
