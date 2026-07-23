#include "master/igh_master.h"

#include <algorithm>

namespace master {

void NativeMasterDeleter::operator()(ec_master_t *master) const noexcept {
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
    if (state_ != MasterState::Initial || !device) {
        return MasterResult::InvalidState;
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

    master_.reset(ecrt_request_master(master_index_));
    if (!master_) {
        return MasterResult::Error;
    }

    domain_ = ecrt_master_create_domain(master_.get());
    if (!domain_) {
        Release();
        return MasterResult::Error;
    }

    const device::DeviceConfiguration configuration{
        master_.get(), domain_, cycle_time_ns_};
    for (const std::unique_ptr<device::IghDevice> &device : devices_) {
        if (!device->Configure(configuration)) {
            Release();
            return MasterResult::Error;
        }
    }

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
