#include "master/igh_master.h"

#include <algorithm>

namespace master {

void NativeMasterDeleter::operator()(ec_master_t* master) const noexcept {
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

MasterResult IghMaster::AddDevice(std::unique_ptr<device::IghDevice>& device) {
    // 步骤 1：仅允许在请求 IgH master 前修改设备列表。
    if (state_ != MasterState::kInitial) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：拒绝空设备以及同一对象的重复注册。
    if (!device) {
        return MasterResult::kInvalidArgument;
    }
    if (ContainsDevice(*device)) {
        return MasterResult::kInvalidArgument;
    }

    // 步骤 3：注册成功后由 IghMaster 独占设备生命周期。
    devices_.push_back(std::move(device));
    return MasterResult::kSuccess;
}

MasterResult IghMaster::SetReferenceClockDevice(const device::IghDevice& device) {
    // 步骤 1：参考时钟必须在主站配置和激活前指定。
    if (state_ != MasterState::kInitial) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：只接受已经由当前主站接管的设备。
    if (!ContainsDevice(device)) {
        return MasterResult::kInvalidArgument;
    }

    // 步骤 3：保存观察指针，实际 IgH 参考时钟选择在 Configure() 中完成。
    reference_clock_device_ = &device;
    return MasterResult::kSuccess;
}

MasterResult IghMaster::Configure() {
    // 步骤 1：检查生命周期状态并确保至少注册了一个从站。
    if (state_ != MasterState::kInitial || devices_.empty()) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：请求 IgH master；后续任何失败路径统一由 Release() 回收。
    master_.reset(ecrt_request_master(master_index_));
    if (!master_) {
        return MasterResult::kError;
    }

    // 步骤 3：创建全部设备共享的单个过程数据 Domain。
    domain_ = ecrt_master_create_domain(master_.get());
    if (!domain_) {
        Release();
        return MasterResult::kError;
    }

    // 步骤 4：依次让具体设备完成从站、PDO、SDO 和 DC 初始化。
    const device::DeviceConfiguration configuration{master_.get(), domain_, cycle_time_ns_};
    for (const std::unique_ptr<device::IghDevice>& device : devices_) {
        if (!device->Configure(configuration)) {
            Release();
            return MasterResult::kError;
        }
    }

    // 步骤 5：若上层指定了参考设备，则在主站激活前选择其 DC 时钟。
    if (reference_clock_device_ && (!reference_clock_device_->slave_config() ||
                                    ecrt_master_select_reference_clock(
                                        master_.get(), reference_clock_device_->slave_config()))) {
        Release();
        return MasterResult::kError;
    }

    // 步骤 6：所有激活前配置成功，推进主站生命周期状态。
    state_ = MasterState::kConfigured;
    return MasterResult::kSuccess;
}

MasterResult IghMaster::Activate() {
    // 步骤 1：只有完成全部从站配置后才允许激活主站。
    if (state_ != MasterState::kConfigured) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：激活 IgH master，使 PDO Domain 进入可交换状态。
    if (ecrt_master_activate(master_.get())) {
        Release();
        return MasterResult::kError;
    }

    // 步骤 3：取得激活后的 Domain 过程数据基地址。
    domain_pd_ = ecrt_domain_data(domain_);
    if (!domain_pd_) {
        Release();
        return MasterResult::kError;
    }

    // 步骤 4：进入实时周期可调用状态。
    state_ = MasterState::kActive;
    return MasterResult::kSuccess;
}

MasterResult IghMaster::ReceiveAndProcess(uint64_t application_time_ns) {
    // 步骤 1：实时收帧只允许在主站 kActive 状态执行。
    if (state_ != MasterState::kActive) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：更新时间、接收 EtherCAT 帧并解析公共 Domain。
    ecrt_master_application_time(master_.get(), application_time_ns);
    ecrt_master_receive(master_.get());
    ecrt_domain_process(domain_);

    // 步骤 3：更新内部 Domain 有效性，供 Runtime/PDO 桥接判断反馈是否可用。
    ec_domain_state_t domain_state{};
    domain_data_valid_ =
        ecrt_domain_state(domain_, &domain_state) == 0 && domain_state.wc_state == EC_WC_COMPLETE;

    // 步骤 4：让每个设备从各自的 TxPDO 区域更新周期反馈。
    for (const std::unique_ptr<device::IghDevice>& device : devices_) {
        device->ReadProcessData(domain_pd_);
    }
    return MasterResult::kSuccess;
}

MasterResult IghMaster::QueueAndSend(bool synchronize_dc) {
    // 步骤 1：实时发帧只允许在主站 kActive 状态执行。
    if (state_ != MasterState::kActive) {
        return MasterResult::kInvalidState;
    }

    // 步骤 2：让每个设备把当前命令写入各自的 RxPDO 区域。
    for (const std::unique_ptr<device::IghDevice>& device : devices_) {
        device->WriteProcessData(domain_pd_);
    }

    // 步骤 3：按配置排队 DC 参考时钟及从站时钟同步报文。
    if (synchronize_dc) {
        ecrt_master_sync_reference_clock(master_.get());
        ecrt_master_sync_slave_clocks(master_.get());
    }

    // 步骤 4：将完成写入的 Domain 排队并发送本周期 EtherCAT 帧。
    ecrt_domain_queue(domain_);
    ecrt_master_send(master_.get());
    return MasterResult::kSuccess;
}

void IghMaster::Release() {
    // 步骤 1：先让设备清除即将失效的 IgH 句柄和 PDO offset。
    for (const std::unique_ptr<device::IghDevice>& device : devices_) {
        device->Reset();
    }

    // 步骤 2：释放 master 及其 Domain，并清除本地主站观察指针。
    master_.reset();
    domain_ = nullptr;
    domain_pd_ = nullptr;
    domain_data_valid_ = false;

    // 步骤 3：保留设备对象，恢复为允许再次 Configure() 的初始状态。
    state_ = MasterState::kInitial;
}

bool IghMaster::ContainsDevice(const device::IghDevice& device) const {
    return std::any_of(devices_.begin(),
                       devices_.end(),
                       [&device](const std::unique_ptr<device::IghDevice>& registered) {
                           return registered.get() == &device;
                       });
}

}  // namespace master
