#include "device/gsd620_device.h"

namespace device {

Gsd620Configuration::Gsd620Configuration() {
    // 步骤 1：设置 GSD620 固定的 EtherCAT 设备身份。
    vendor_id = 0x00000911U;
    product_code = 0x00000620U;

    // 步骤 2：默认按照设备 ESI 使用 SYNC0 DC。
    dc_assign_activate = 0x0300U;
    sync0_shift_ns = 0;
    enable_dc = true;
}

Gsd620Device::Gsd620Device(Gsd620Configuration configuration)
    : Cia402StandardPdoDevice(configuration), configuration_(configuration) {}

const Gsd620Configuration& Gsd620Device::configuration() const noexcept {
    return configuration_;
}

}  // namespace device
