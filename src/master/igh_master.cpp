#include "master/igh_master.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace master {
namespace {

constexpr uint32_t kDcStableThresholdNs = 10'000U;
constexpr uint32_t kDcStableRequiredCycles = 100U;
constexpr uint32_t kDcMonitorReportIntervalCycles = 1'000U;

const char* WorkingCounterStateName(ec_wc_state_t state) noexcept {
    switch (state) {
        case EC_WC_ZERO:
            return "zero";
        case EC_WC_INCOMPLETE:
            return "incomplete";
        case EC_WC_COMPLETE:
            return "complete";
        default:
            return "unknown";
    }
}

bool MasterStateChanged(const ec_master_state_t& left, const ec_master_state_t& right) noexcept {
    return left.slaves_responding != right.slaves_responding || left.al_states != right.al_states ||
           left.link_up != right.link_up;
}

bool DomainStateChanged(const ec_domain_state_t& left, const ec_domain_state_t& right) noexcept {
    return left.working_counter != right.working_counter || left.wc_state != right.wc_state ||
           left.redundancy_active != right.redundancy_active;
}

bool SlaveStateChanged(const ec_slave_config_state_t& left,
                       const ec_slave_config_state_t& right) noexcept {
    return left.online != right.online || left.operational != right.operational ||
           left.al_state != right.al_state;
}

}  // namespace

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
        std::fprintf(stderr,
                     "[ethercat][master] configure rejected: state=%u, devices=%zu\n",
                     static_cast<unsigned int>(state_),
                     devices_.size());
        return MasterResult::kInvalidState;
    }

    // 步骤 2：请求 IgH master；后续任何失败路径统一由 Release() 回收。
    std::printf("[ethercat][master] requesting master %u, cycle=%u ns, devices=%zu\n",
                master_index_,
                cycle_time_ns_,
                devices_.size());
    master_.reset(ecrt_request_master(master_index_));
    if (!master_) {
        std::fprintf(stderr, "[ethercat][master] failed to request master %u\n", master_index_);
        return MasterResult::kError;
    }
    std::printf("[ethercat][master] master %u requested\n", master_index_);

    // 步骤 3：创建全部设备共享的单个过程数据 Domain。
    domain_ = ecrt_master_create_domain(master_.get());
    if (!domain_) {
        std::fprintf(stderr, "[ethercat][master] failed to create process-data domain\n");
        Release();
        return MasterResult::kError;
    }
    std::printf("[ethercat][master] process-data domain created\n");

    // 步骤 4：依次让具体设备完成从站、PDO、SDO 和 DC 初始化。
    const device::DeviceConfiguration configuration{master_.get(), domain_, cycle_time_ns_};
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        std::printf("[ethercat][master] configuring device %zu/%zu\n", index + 1U, devices_.size());
        if (!devices_[index]->Configure(configuration)) {
            std::fprintf(stderr,
                         "[ethercat][master] device %zu/%zu configuration failed\n",
                         index + 1U,
                         devices_.size());
            Release();
            return MasterResult::kError;
        }
    }

    // 步骤 5：若上层指定了参考设备，则在主站激活前选择其 DC 时钟。
    if (reference_clock_device_ && (!reference_clock_device_->slave_config() ||
                                    ecrt_master_select_reference_clock(
                                        master_.get(), reference_clock_device_->slave_config()))) {
        std::fprintf(stderr, "[ethercat][dc] failed to select reference clock device\n");
        Release();
        return MasterResult::kError;
    }
    if (reference_clock_device_) {
        std::printf("[ethercat][dc] reference clock device selected\n");
    } else {
        std::printf("[ethercat][dc] no explicit reference clock device configured\n");
    }

    // 步骤 6：所有激活前配置成功，推进主站生命周期状态。
    state_ = MasterState::kConfigured;
    std::printf("[ethercat][master] configuration completed\n");
    return MasterResult::kSuccess;
}

MasterResult IghMaster::Activate() {
    // 步骤 1：只有完成全部从站配置后才允许激活主站。
    if (state_ != MasterState::kConfigured) {
        std::fprintf(stderr,
                     "[ethercat][master] activate rejected: state=%u\n",
                     static_cast<unsigned int>(state_));
        return MasterResult::kInvalidState;
    }

    // 步骤 2：激活 IgH master，使 PDO Domain 进入可交换状态。
    std::printf("[ethercat][master] activating master %u\n", master_index_);
    if (ecrt_master_activate(master_.get())) {
        std::fprintf(stderr, "[ethercat][master] master activation failed\n");
        Release();
        return MasterResult::kError;
    }

    // 步骤 3：取得激活后的 Domain 过程数据基地址。
    domain_pd_ = ecrt_domain_data(domain_);
    if (!domain_pd_) {
        std::fprintf(stderr, "[ethercat][master] failed to obtain domain process data\n");
        Release();
        return MasterResult::kError;
    }

    // 步骤 4：进入实时周期可调用状态。
    ResetRuntimeDiagnostics();
    state_ = MasterState::kActive;
    std::printf("[ethercat][master] activated; cyclic communication may start\n");
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

    // 步骤 4：仅在通信状态变化时输出诊断，并处理上一周期的 DC monitor。
    UpdateRuntimeDiagnostics(domain_state);
    ProcessDcMonitorResult();

    // 步骤 5：让每个设备从各自的 TxPDO 区域更新周期反馈。
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

        // DC 尚未稳定时额外排队监测报文；稳定后不再监测和打印。
        if (reference_clock_device_ && !dc_monitor_stable_ && !dc_monitor_pending_) {
            if (ecrt_master_sync_monitor_queue(master_.get()) == 0) {
                dc_monitor_pending_ = true;
            } else if (!dc_monitor_error_reported_) {
                std::fprintf(stderr, "[ethercat][dc] failed to queue DC monitor datagram\n");
                dc_monitor_error_reported_ = true;
            }
        }
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
    ResetRuntimeDiagnostics();

    // 步骤 3：保留设备对象，恢复为允许再次 Configure() 的初始状态。
    state_ = MasterState::kInitial;
}

void IghMaster::UpdateRuntimeDiagnostics(const ec_domain_state_t& domain_state) noexcept {
    ec_master_state_t master_state{};
    if (ecrt_master_state(master_.get(), &master_state) != 0) {
        return;
    }

    const bool first_sample = !runtime_diagnostics_initialized_;
    if (first_sample || MasterStateChanged(master_state, last_master_state_)) {
        std::printf("[ethercat][state] master link=%s, responding=%u, al_states=0x%02X\n",
                    master_state.link_up ? "up" : "down",
                    master_state.slaves_responding,
                    master_state.al_states);
    }
    if (first_sample || DomainStateChanged(domain_state, last_domain_state_)) {
        std::printf("[ethercat][state] domain wc=%u, state=%s, redundancy=%s\n",
                    domain_state.working_counter,
                    WorkingCounterStateName(domain_state.wc_state),
                    domain_state.redundancy_active ? "active" : "inactive");
    }

    bool all_slaves_operational = !devices_.empty();
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        ec_slave_config_state_t slave_state{};
        if (!devices_[index]->slave_config() ||
            ecrt_slave_config_state(devices_[index]->slave_config(), &slave_state) != 0) {
            all_slaves_operational = false;
            continue;
        }

        if (first_sample || SlaveStateChanged(slave_state, last_slave_states_[index])) {
            std::printf(
                "[ethercat][state] device[%zu] online=%s, operational=%s, al_state=0x%02X\n",
                index,
                slave_state.online ? "yes" : "no",
                slave_state.operational ? "yes" : "no",
                slave_state.al_state);
        }
        all_slaves_operational = all_slaves_operational && slave_state.online &&
                                 slave_state.operational && slave_state.al_state == 0x08U;
        last_slave_states_[index] = slave_state;
    }

    const bool stable =
        master_state.link_up && all_slaves_operational && domain_state.wc_state == EC_WC_COMPLETE;
    if (stable != communication_stable_) {
        if (stable) {
            std::printf("[ethercat][state] process-data communication stable\n");
        } else if (communication_stable_) {
            std::fprintf(stderr,
                         "[ethercat][state] communication lost stable state; DC monitoring "
                         "restarted\n");
            dc_monitor_stable_ = false;
            dc_stable_cycle_count_ = 0;
            dc_monitor_sample_count_ = 0;
        }
        communication_stable_ = stable;
    }

    last_master_state_ = master_state;
    last_domain_state_ = domain_state;
    runtime_diagnostics_initialized_ = true;
}

void IghMaster::ProcessDcMonitorResult() noexcept {
    if (!dc_monitor_pending_) {
        return;
    }
    dc_monitor_pending_ = false;

    const uint32_t maximum_difference_ns = ecrt_master_sync_monitor_process(master_.get());
    if (maximum_difference_ns == std::numeric_limits<uint32_t>::max()) {
        if (!dc_monitor_error_reported_) {
            std::fprintf(stderr, "[ethercat][dc] DC monitor result is not available\n");
            dc_monitor_error_reported_ = true;
        }
        return;
    }
    dc_monitor_error_reported_ = false;
    ++dc_monitor_sample_count_;

    if (!communication_stable_) {
        dc_stable_cycle_count_ = 0;
        return;
    }

    if (maximum_difference_ns <= kDcStableThresholdNs) {
        ++dc_stable_cycle_count_;
    } else {
        dc_stable_cycle_count_ = 0;
    }

    if (dc_monitor_sample_count_ == 1U ||
        dc_monitor_sample_count_ % kDcMonitorReportIntervalCycles == 0U) {
        std::printf("[ethercat][dc] maximum clock difference=%u ns, stable_samples=%u/%u\n",
                    maximum_difference_ns,
                    dc_stable_cycle_count_,
                    kDcStableRequiredCycles);
    }

    if (dc_stable_cycle_count_ >= kDcStableRequiredCycles) {
        dc_monitor_stable_ = true;
        std::printf(
            "[ethercat][dc] synchronization stable: max difference <= %u ns for %u cycles; "
            "monitor logging stopped\n",
            kDcStableThresholdNs,
            kDcStableRequiredCycles);
    }
}

void IghMaster::ResetRuntimeDiagnostics() noexcept {
    runtime_diagnostics_initialized_ = false;
    communication_stable_ = false;
    dc_monitor_pending_ = false;
    dc_monitor_stable_ = false;
    dc_monitor_error_reported_ = false;
    dc_stable_cycle_count_ = 0;
    dc_monitor_sample_count_ = 0;
    last_master_state_ = {};
    last_domain_state_ = {};
    last_slave_states_.assign(devices_.size(), {});
}

bool IghMaster::ContainsDevice(const device::IghDevice& device) const {
    return std::any_of(devices_.begin(),
                       devices_.end(),
                       [&device](const std::unique_ptr<device::IghDevice>& registered) {
                           return registered.get() == &device;
                       });
}

}  // namespace master
