#ifndef SINSUN_APP_CONFIG_H
#define SINSUN_APP_CONFIG_H

#include <cstddef>
#include <cstdint>

namespace sinsun {

/* IgH 主站编号；当前工程只使用第 0 个 master。 */
constexpr unsigned int kMasterIndex = 0;

/* EtherCAT 从站 alias 地址；现场未使用 alias 时保持 0。 */
constexpr uint16_t kAlias = 0;

/* 新松 SINSUN 设备 Vendor ID，来自 ESI XML 的 Vendor/Id。 */
constexpr uint32_t kVendorId = 0x000008CF;

/* 伺服从站 ProductCode，来自 SERVO_ECAT.xml。 */
constexpr uint32_t kServoProductCode = 0x00009252;

/* 末端 IO 从站 ProductCode，来自 ECAT_EndIO.xml。 */
constexpr uint32_t kEndIoProductCode = 0x00009250;

/* 主站周期时间，单位 ns；当前为 1 ms DC 通讯周期。 */
constexpr uint32_t kCycleTimeNs = 1000000;

/* DC AssignActivate，来自两个 SINSUN ESI XML 的 DC 模式配置。 */
constexpr uint16_t kDcAssignActivate = 0x0300;

/* 通信质量报告打印间隔，60000 个 1 ms 周期即每分钟打印一次。 */
constexpr uint64_t kPrintPeriodCycles = 60000;

/* DC 质量采样间隔；1000 个 1 ms 周期即每秒采样一次。 */
constexpr uint64_t kDcQualitySamplePeriodCycles = 1000;

/* 严重超周期阈值；超过 150% 标称周期计为严重超周期。 */
constexpr uint32_t kSevereCycleOverrunNs = kCycleTimeNs * 3 / 2;

/* 是否打印每条 XML 参数解析结果。 */
constexpr bool kLogAxisXmlParameterDetails = true;

/* 是否打印每条 SDO 参数下发和 mailbox word 写入结果。 */
constexpr bool kLogSdoDownloadDetails = true;

/* DC 同步报文排队间隔；1 表示每个通讯周期都做一次 DC 同步。 */
constexpr uint64_t kDcSyncPeriodCycles = 1;

/* 实时循环前预触碰的栈大小，用于降低运行期缺页概率。 */
constexpr int kMaxSafeStack = 8 * 1024;

/* 伺服从站数量；拓扑 id 1-6 都是伺服。 */
constexpr size_t kServoCount = 6;

/* 只使能和运动最后一个伺服关节；数组下标 5 对应用户侧 Servo 6。 */
constexpr size_t kMotionServoIndex = kServoCount - 1;

/* 最后一个伺服使用 CSP：Cyclic Synchronous Position。 */
constexpr int8_t kDriveOperationMode = 8;

/* 使能序列中每个控制字保持 1000 个 1 ms 周期，即 1 秒。 */
constexpr uint64_t kEnableStepCycles = 1000;

/* 最后一个关节的正弦运动参数：以 base 为中心，幅值单位 pulse。 */
constexpr int32_t kMotionAmplitudeCounts = 100000;
constexpr int32_t kMotionRangeCounts = kMotionAmplitudeCounts * 2;
constexpr uint64_t kMotionPeriodCycles = 20000;

/* 正弦启动渐变时间；前 2 秒振幅从 0 平滑增加到设定值。 */
constexpr uint64_t kMotionRampCycles = 2000;

/* 末端 IO 的用户侧逻辑 id；用户描述中它是第 7 个从站。 */
constexpr uint16_t kEndIoLogicalId = 7;

/* IgH 使用 0-based position，因此逻辑 id 7 对应 position 6。 */
constexpr uint16_t kEndIoPosition = kEndIoLogicalId - 1;

/*
 * 当前 PDO 映射中，6 个伺服完整交换过程数据时的 Working Counter。
 * 临时允许 EndIO 异常的测试模式只使用该值判断6轴通讯是否完整。
 */
constexpr uint32_t kServoOnlyExpectedWorkingCounter = 18;

}  // namespace sinsun

#endif
