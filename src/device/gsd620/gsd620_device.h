#ifndef IGH_DEVICE_GSD620_GSD620_DEVICE_H
#define IGH_DEVICE_GSD620_GSD620_DEVICE_H

#include <cstdint>

#include "device/cia402_standard_pdo_device.h"

namespace device {

/**
 * @brief GSD620 从站身份和 DC 配置。
 *
 * 该配置继承标准 CiA402 PDO 配置，并提供 GSD620 的默认厂商 ID、产品代码
 * 和 DC 参数。alias、position、SYNC0 偏移等字段可在装配设备时覆盖。
 */
struct Gsd620Configuration : public Cia402StandardPdoConfiguration {
    /**
     * @brief 使用 GSD620 默认设备身份和 DC 参数创建配置。
     */
    Gsd620Configuration();
};

using Gsd620CyclicData = Cia402StandardPdoData;

/**
 * @brief 基于标准 CiA402 PDO 适配器的 GSD620 设备。
 *
 * 标准对象、PDO offset、Domain 注册、实时数据读写和 DC 配置全部由
 * Cia402StandardPdoDevice 提供。本类只保存 GSD620 配置；后续存在厂商
 * 专有 SDO 或 PDO 差异时，可通过基类保护钩子集中扩展。
 */
class Gsd620Device final : public Cia402StandardPdoDevice {
public:
    /**
     * @brief 使用指定的从站身份和 DC 参数创建 GSD620 适配器。
     *
     * 构造阶段不访问 EtherCAT 总线；实际配置由基类 Configure() 完成。
     *
     * @param configuration GSD620 的从站身份、DC 和 SYNC0 配置。
     */
    explicit Gsd620Device(Gsd620Configuration configuration = {});

    /**
     * @brief 返回创建时保存的 GSD620 静态配置。
     *
     * @return GSD620 配置的只读引用。
     */
    const Gsd620Configuration& configuration() const noexcept;

private:
    Gsd620Configuration configuration_{};  // GSD620 的设备身份和 DC 配置。
};

}  // namespace device

#endif
