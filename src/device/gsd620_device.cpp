#include "device/gsd620_device.h"

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

Gsd620Device::Gsd620Device(Gsd620Configuration configuration) : configuration_(configuration) {}

bool Gsd620Device::Configure(const DeviceConfiguration& configuration) {
    // 步骤 1：检查主站配置上下文以及启用 DC 时所需的周期参数。
    if (!configuration.master || !configuration.domain ||
        (configuration_.enable_dc && configuration.cycle_time_ns == 0)) {
        return false;
    }

    // 步骤 2：按照 alias、position 和设备身份取得 IgH 从站配置对象。
    slave_config_ = ecrt_master_slave_config(configuration.master,
                                             configuration_.alias,
                                             configuration_.position,
                                             configuration_.vendor_id,
                                             configuration_.product_code);
    if (!slave_config_) {
        return false;
    }

    // 步骤 3：将设备固定的 SM/PDO 映射写入从站配置。
    if (ecrt_slave_config_pdos(slave_config_, EC_END, kSyncs)) {
        Reset();
        return false;
    }

    // 步骤 4：生成 PDO entry 注册表，并交给公共 Domain 计算偏移。
    BuildPdoEntryRegistrations();
    if (ecrt_domain_reg_pdo_entry_list(configuration.domain, pdo_entry_regs_.data())) {
        Reset();
        return false;
    }

    // 步骤 5：仅对启用 DC 的设备配置 SYNC0；非 DC 从站跳过此步骤。
    if (configuration_.enable_dc && ecrt_slave_config_dc(slave_config_,
                                                         configuration_.dc_assign_activate,
                                                         configuration.cycle_time_ns,
                                                         configuration_.sync0_shift_ns,
                                                         0,
                                                         0)) {
        Reset();
        return false;
    }

    return true;
}

ec_slave_config_t* Gsd620Device::slave_config() const {
    return slave_config_;
}

void Gsd620Device::ReadProcessData(const uint8_t* domain_pd) noexcept {
    // 步骤 1：Domain 尚未激活或数据地址无效时不访问过程数据。
    if (!domain_pd) {
        return;
    }

    // 步骤 2：依据 Configure() 得到的偏移读取本周期全部 TxPDO 反馈。
    cyclic_data_.actual_position = EC_READ_S32(domain_pd + pdo_offsets_.actual_position);
    cyclic_data_.actual_velocity = EC_READ_S32(domain_pd + pdo_offsets_.actual_velocity);
    cyclic_data_.actual_torque = EC_READ_S16(domain_pd + pdo_offsets_.actual_torque);
    cyclic_data_.error_code = EC_READ_U16(domain_pd + pdo_offsets_.error_code);
    cyclic_data_.statusword = EC_READ_U16(domain_pd + pdo_offsets_.statusword);
    cyclic_data_.mode_display = EC_READ_S8(domain_pd + pdo_offsets_.mode_display);
}

void Gsd620Device::WriteProcessData(uint8_t* domain_pd) noexcept {
    // 步骤 1：Domain 尚未激活或数据地址无效时不写过程数据。
    if (!domain_pd) {
        return;
    }

    // 步骤 2：依据 Configure() 得到的偏移写入本周期全部 RxPDO 命令。
    EC_WRITE_S32(domain_pd + pdo_offsets_.target_position, cyclic_data_.target_position);
    EC_WRITE_S32(domain_pd + pdo_offsets_.target_velocity, cyclic_data_.target_velocity);
    EC_WRITE_U16(domain_pd + pdo_offsets_.controlword, cyclic_data_.controlword);
    EC_WRITE_S16(domain_pd + pdo_offsets_.target_torque, cyclic_data_.target_torque);
    EC_WRITE_S8(domain_pd + pdo_offsets_.mode, cyclic_data_.mode);
}

void Gsd620Device::Reset() noexcept {
    // 步骤 1：清除随 IgH master 释放而失效的从站句柄和 PDO 偏移。
    slave_config_ = nullptr;
    pdo_offsets_ = {};
    pdo_entry_regs_ = {};

    // 步骤 2：清空反馈与命令，避免下次初始化沿用旧周期数据。
    cyclic_data_ = {};
}

void Gsd620Device::BuildPdoEntryRegistrations() noexcept {
    // 步骤 1：缓存当前设备身份，供每个 PDO entry 使用同一组匹配信息。
    const uint16_t alias = configuration_.alias;
    const uint16_t position = configuration_.position;
    const uint32_t vendor_id = configuration_.vendor_id;
    const uint32_t product_code = configuration_.product_code;

    // 步骤 2：按 PDO 映射顺序建立 entry 与本地 offset 字段的对应关系。
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
        {},  // IgH PDO entry 注册列表结束标记。
    }};
}

}  // namespace device
