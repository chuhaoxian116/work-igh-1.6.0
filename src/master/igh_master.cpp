#include "master/igh_master.h"

#include <algorithm>

namespace master {

void NativeMasterDeleter::operator()(ec_master_t *master) const noexcept {
    // ecrt_release_master() 会同时释放该 master 创建的 domain 和从站配置。
    if (master) {
        ecrt_release_master(master);
    }
}

IghMaster::IghMaster(uint32_t master_index, uint32_t cycle_time_ns)
    : master_index_(master_index), cycle_time_ns_(cycle_time_ns) {}

IghMaster::~IghMaster() {
    Release();
}

MasterResult IghMaster::AddDevice(std::unique_ptr<device::IghDevice> &device) {
    if (state_ != MasterState::Initial) {
        return MasterResult::InvalidState;
    }
    if (!device) {
        return MasterResult::InvalidArgument;
    }
    if (ContainsDevice(*device)) {
        return MasterResult::InvalidArgument;
    }

    devices_.push_back(std::move(device));
    return MasterResult::Success;
}

MasterResult IghMaster::SetReferenceClockDevice(
    const device::IghDevice &device) {
    if (state_ != MasterState::Initial) {
        return MasterResult::InvalidState;
    }
    if (!ContainsDevice(device)) {
        return MasterResult::InvalidArgument;
    }

    reference_clock_device_ = &device;
    return MasterResult::Success;
}

MasterResult IghMaster::Configure() {
    if (state_ != MasterState::Initial || devices_.empty()) {
        return MasterResult::InvalidState;
    }

    // 从此处开始取得 IgH 资源；后续任何失败路径统一由 Release() 回收。
    master_.reset(ecrt_request_master(master_index_));
    if (!master_) {
        return MasterResult::Error;
    }

    domain_ = ecrt_master_create_domain(master_.get());
    if (!domain_) {
        Release();
        return MasterResult::Error;
    }

    // 所有从站共享同一个 master、单个 PDO domain 和标称通信周期。
    const device::DeviceConfiguration configuration{
        master_.get(), domain_, cycle_time_ns_};
    for (const std::unique_ptr<device::IghDevice> &device : devices_) {
        if (!device->Configure(configuration)) {
            Release();
            return MasterResult::Error;
        }
    }

    // 参考时钟必须在主站激活前选择。
    if (reference_clock_device_ &&
        (!reference_clock_device_->slave_config() ||
             ecrt_master_select_reference_clock(
             master_.get(), reference_clock_device_->slave_config()))) {
        Release();
        return MasterResult::Error;
    }

    state_ = MasterState::Configured;
    return MasterResult::Success;
}

MasterResult IghMaster::Activate() {
    if (state_ != MasterState::Configured) {
        return MasterResult::InvalidState;
    }
    if (ecrt_master_activate(master_.get())) {
        Release();
        return MasterResult::Error;
    }

    // 只有激活成功后 domain process data 的基地址才有效。
    domain_pd_ = ecrt_domain_data(domain_);
    if (!domain_pd_) {
        Release();
        return MasterResult::Error;
    }

    state_ = MasterState::Active;
    return MasterResult::Success;
}

MasterResult IghMaster::ReceiveAndProcess(uint64_t application_time_ns) {
    if (state_ != MasterState::Active) {
        return MasterResult::InvalidState;
    }

    // 收帧后先处理 domain，再让每个设备从 TxPDO 区域读取输入数据。
    ecrt_master_application_time(master_.get(), application_time_ns);
    ecrt_master_receive(master_.get());
    ecrt_domain_process(domain_);

    for (const std::unique_ptr<device::IghDevice> &device : devices_) {
        device->ReadProcessData(domain_pd_);
    }
    return MasterResult::Success;
}

MasterResult IghMaster::QueueAndSend(bool synchronize_dc) {
    if (state_ != MasterState::Active) {
        return MasterResult::InvalidState;
    }

    // 先由设备写入 RxPDO，再将整个 domain 排队并发送。
    for (const std::unique_ptr<device::IghDevice> &device : devices_) {
        device->WriteProcessData(domain_pd_);
    }

    if (synchronize_dc) {
        ecrt_master_sync_reference_clock(master_.get());
        ecrt_master_sync_slave_clocks(master_.get());
    }
    ecrt_domain_queue(domain_);
    ecrt_master_send(master_.get());
    return MasterResult::Success;
}

void IghMaster::Release() {
    // 设备先清除保存的 IgH 句柄和 PDO offset，避免保留已失效的地址。
    for (const std::unique_ptr<device::IghDevice> &device : devices_) {
        device->Reset();
    }

    master_.reset();
    domain_ = nullptr;
    domain_pd_ = nullptr;
    state_ = MasterState::Initial;
}

bool IghMaster::ContainsDevice(const device::IghDevice &device) const {
    return std::any_of(
        devices_.begin(), devices_.end(),
        [&device](const std::unique_ptr<device::IghDevice> &registered) {
            return registered.get() == &device;
        });
}

}  // namespace master
