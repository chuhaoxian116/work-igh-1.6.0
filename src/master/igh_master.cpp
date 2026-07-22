#include "master/igh_master.h"

#include <algorithm>

namespace master {

IghMaster::IghMaster(uint32_t master_index, uint32_t cycle_time_ns)
    : master_index_(master_index), cycle_time_ns_(cycle_time_ns) {}

IghMaster::~IghMaster() {
    Release();
}

MasterResult IghMaster::AddDevice(device::BasisDevice &device) {
    if (state_ != MasterState::Initial) {
        return MasterResult::InvalidState;
    }
    if (ContainsDevice(device)) {
        return MasterResult::InvalidArgument;
    }

    devices_.push_back(&device);
    return MasterResult::Success;
}

MasterResult IghMaster::SetReferenceClockDevice(device::BasisDevice &device) {
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

    master_ = ecrt_request_master(master_index_);
    if (!master_) {
        return MasterResult::Error;
    }

    domain_ = ecrt_master_create_domain(master_);
    if (!domain_) {
        Release();
        return MasterResult::Error;
    }

    const device::DeviceConfiguration configuration{
        master_, domain_, cycle_time_ns_};
    for (device::BasisDevice *device : devices_) {
        if (!device->Configure(configuration)) {
            Release();
            return MasterResult::Error;
        }
    }

    if (reference_clock_device_ &&
        (!reference_clock_device_->slave_config() ||
         ecrt_master_select_reference_clock(
             master_, reference_clock_device_->slave_config()))) {
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
    if (ecrt_master_activate(master_)) {
        Release();
        return MasterResult::Error;
    }

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

    ecrt_master_application_time(master_, application_time_ns);
    ecrt_master_receive(master_);
    ecrt_domain_process(domain_);

    for (device::BasisDevice *device : devices_) {
        device->ReadProcessData(domain_pd_);
    }
    return MasterResult::Success;
}

MasterResult IghMaster::QueueAndSend(bool synchronize_dc) {
    if (state_ != MasterState::Active) {
        return MasterResult::InvalidState;
    }

    for (device::BasisDevice *device : devices_) {
        device->WriteProcessData(domain_pd_);
    }

    if (synchronize_dc) {
        ecrt_master_sync_reference_clock(master_);
        ecrt_master_sync_slave_clocks(master_);
    }
    ecrt_domain_queue(domain_);
    ecrt_master_send(master_);
    return MasterResult::Success;
}

void IghMaster::Release() {
    if (master_) {
        ecrt_release_master(master_);
    }

    master_ = nullptr;
    domain_ = nullptr;
    domain_pd_ = nullptr;
    state_ = MasterState::Initial;
}

bool IghMaster::ContainsDevice(const device::BasisDevice &device) const {
    return std::find(devices_.begin(), devices_.end(), &device) !=
           devices_.end();
}

}  // namespace master
