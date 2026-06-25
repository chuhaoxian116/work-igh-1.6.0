#ifndef SINSUN_PDO_CONFIG_H
#define SINSUN_PDO_CONFIG_H

#include <array>

#include "app_config.h"
#include "ecrt.h"

namespace sinsun {

/*
 * 伺服 RxPDO entry 列表。
 *
 * RxPDO 是主站写给伺服的数据。本程序只做通讯和打印，不做使能/运动，
 * 但仍按 ESI 的固定 PDO 映射完整配置，输出过程数据保持默认 0。
 */
inline const ec_pdo_entry_info_t servo_rxpdo_entries[] = {
    {0x607A, 0x00, 32}, {0x60FE, 0x00, 32}, {0x60FF, 0x00, 32},
    {0x6040, 0x00, 16}, {0x6071, 0x00, 16}, {0x6060, 0x00, 8},
    {0x7006, 0x00, 8},  {0x7007, 0x00, 32}, {0x7008, 0x00, 32},
};

/*
 * 伺服 TxPDO entry 列表。
 *
 * TxPDO 是主站从伺服读取的数据，包含位置、速度、状态字、错误码、
 * Sync0 时间差以及安全板相关数据。
 */
inline const ec_pdo_entry_info_t servo_txpdo_entries[] = {
    {0x6064, 0x00, 32}, {0x60FD, 0x00, 32}, {0x6063, 0x00, 32},
    {0x6069, 0x00, 32}, {0x603F, 0x00, 32}, {0x606C, 0x00, 32},
    {0x6041, 0x00, 16}, {0x6077, 0x00, 16}, {0x6078, 0x00, 16},
    {0x6061, 0x00, 8},  {0x0000, 0x00, 8},  {0x600B, 0x00, 16},
    {0x600C, 0x00, 32}, {0x600D, 0x01, 32}, {0x600D, 0x02, 16},
    {0x600D, 0x03, 32}, {0x600D, 0x04, 32}, {0x600D, 0x05, 16},
    {0x600D, 0x06, 16}, {0x600D, 0x07, 32}, {0x600D, 0x08, 32},
    {0x600D, 0x09, 16}, {0x600D, 0x0A, 16}, {0x600D, 0x0B, 32},
    {0x600D, 0x0C, 32}, {0x600D, 0x0D, 32},
};

/* 伺服 PDO assignment：0x1600 挂到 SM2，0x1A00 挂到 SM3。 */
inline const ec_pdo_info_t servo_pdos[] = {
    {0x1600, std::size(servo_rxpdo_entries), servo_rxpdo_entries},
    {0x1A00, std::size(servo_txpdo_entries), servo_txpdo_entries},
};

/* 伺服 SyncManager 配置；SM0/SM1 为邮箱，SM2 输出，SM3 输入。 */
inline const ec_sync_info_t servo_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
    {1, EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, &servo_pdos[0], EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, &servo_pdos[1], EC_WD_DISABLE},
    {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
};

/*
 * 末端 IO RxPDO entry 列表。
 *
 * 这些 entry 是主站写给末端 IO 的 LED、数字输出和 RS485 输出数据。
 * 当前程序不写业务输出，只保持 PDO 映射完整。
 */
inline const ec_pdo_entry_info_t endio_rxpdo_entries[] = {
    {0x7000, 0x01, 8},  {0x7000, 0x02, 8},  {0x7000, 0x03, 16},
    {0x7000, 0x04, 16}, {0x7000, 0x05, 8},  {0x7000, 0x06, 8},
    {0x7000, 0x07, 8},  {0x7000, 0x08, 8},  {0x7000, 0x09, 8},
    {0x7000, 0x0A, 8},  {0x7000, 0x0B, 8},  {0x7000, 0x0C, 8},
    {0x7000, 0x0D, 8},  {0x7000, 0x0E, 8},  {0x7000, 0x0F, 8},
    {0x7000, 0x10, 8},  {0x7000, 0x11, 8},  {0x7000, 0x12, 8},
    {0x7000, 0x13, 8},  {0x7000, 0x14, 8},  {0x7000, 0x15, 8},
    {0x7000, 0x16, 8},  {0x7000, 0x17, 8},  {0x7000, 0x18, 8},
    {0x7000, 0x19, 8},  {0x7000, 0x1A, 8},  {0x7000, 0x1B, 8},
    {0x7000, 0x1C, 8},  {0x7000, 0x1D, 8},  {0x7000, 0x1E, 8},
    {0x7000, 0x1F, 8},  {0x7000, 0x20, 8},  {0x7000, 0x21, 8},
    {0x7000, 0x22, 8},  {0x7000, 0x23, 8},  {0x7000, 0x24, 8},
};

/*
 * 末端 IO TxPDO entry 列表。
 *
 * 这些 entry 是主站读取的末端 IO 状态，包括错误码、数字输入、
 * 两路模拟量、温度、三轴加速度和 RS485 输入缓存。
 */
inline const ec_pdo_entry_info_t endio_txpdo_entries[] = {
    {0x6000, 0x01, 8},  {0x6000, 0x02, 8},  {0x6000, 0x03, 16},
    {0x6000, 0x04, 16}, {0x6000, 0x05, 16}, {0x6000, 0x06, 16},
    {0x6000, 0x07, 16}, {0x6000, 0x08, 16}, {0x6000, 0x09, 16},
    {0x6000, 0x0A, 16}, {0x6000, 0x0B, 8},  {0x6000, 0x0C, 8},
    {0x6000, 0x0D, 8},  {0x6000, 0x0E, 8},  {0x6000, 0x0F, 8},
    {0x6000, 0x10, 8},  {0x6000, 0x11, 8},  {0x6000, 0x12, 8},
    {0x6000, 0x13, 8},  {0x6000, 0x14, 8},  {0x6000, 0x15, 8},
    {0x6000, 0x16, 8},  {0x6000, 0x17, 8},  {0x6000, 0x18, 8},
    {0x6000, 0x19, 8},  {0x6000, 0x1A, 8},  {0x6000, 0x1B, 8},
    {0x6000, 0x1C, 8},  {0x6000, 0x1D, 8},  {0x6000, 0x1E, 8},
    {0x6000, 0x1F, 8},  {0x6000, 0x20, 8},  {0x6000, 0x21, 8},
    {0x6000, 0x22, 8},  {0x6000, 0x23, 8},  {0x6000, 0x24, 8},
    {0x6000, 0x25, 8},  {0x6000, 0x26, 8},  {0x6000, 0x27, 8},
    {0x6000, 0x28, 8},  {0x6000, 0x29, 8},  {0x6000, 0x2A, 8},
};

/* 末端 IO PDO assignment：0x1600 挂到 SM2，0x1A00 挂到 SM3。 */
inline const ec_pdo_info_t endio_pdos[] = {
    {0x1600, std::size(endio_rxpdo_entries), endio_rxpdo_entries},
    {0x1A00, std::size(endio_txpdo_entries), endio_txpdo_entries},
};

/* 末端 IO SyncManager 配置；SM0/SM1 为邮箱，SM2 输出，SM3 输入。 */
inline const ec_sync_info_t endio_syncs[] = {
    {0, EC_DIR_OUTPUT, 0, nullptr, EC_WD_DISABLE},
    {1, EC_DIR_INPUT, 0, nullptr, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, &endio_pdos[0], EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, &endio_pdos[1], EC_WD_DISABLE},
    {0xff, EC_DIR_INVALID, 0, nullptr, EC_WD_DEFAULT},
};

/* 伺服中需要打印的 TxPDO entry 在 process data 区里的字节偏移。 */
struct ServoOffsets {
    /* 0x6064:00，伺服实际位置。 */
    unsigned int actual_position = 0;
    /* 0x60FD:00，伺服数字输入。 */
    unsigned int digital_input = 0;
    /* 0x603F:00，伺服错误码。 */
    unsigned int error_code = 0;
    /* 0x606C:00，伺服实际速度。 */
    unsigned int actual_velocity = 0;
    /* 0x6041:00，伺服状态字。 */
    unsigned int status_word = 0;
    /* 0x6077:00，伺服实际转矩。 */
    unsigned int actual_torque = 0;
    /* 0x6061:00，伺服当前模式显示。 */
    unsigned int operation_mode_display = 0;
    /* 0x600B:00，伺服反馈的 Sync0 时间差。 */
    unsigned int sync0_time_difference = 0;
};

/* 末端 IO 中需要打印的 TxPDO entry 在 process data 区里的字节偏移。 */
struct EndIoOffsets {
    /* 0x6000:01，末端 IO 错误码。 */
    unsigned int error_code = 0;
    /* 0x6000:02，末端 IO 数字输入。 */
    unsigned int digital_inputs = 0;
    /* 0x6000:03，末端 IO 模拟量输入通道 1。 */
    unsigned int analog_voltage_1 = 0;
    /* 0x6000:04，末端 IO 模拟量输入通道 2。 */
    unsigned int analog_voltage_2 = 0;
    /* 0x6000:05，末端 IO 温度值。 */
    unsigned int temperature = 0;
    /* 0x6000:06，末端 IO 加速度通道 1。 */
    unsigned int acceleration_1 = 0;
    /* 0x6000:07，末端 IO 加速度通道 2。 */
    unsigned int acceleration_2 = 0;
    /* 0x6000:08，末端 IO 加速度通道 3。 */
    unsigned int acceleration_3 = 0;
    /* 0x6000:09，末端 IO RS485 输入帧计数。 */
    unsigned int rs485_inputs_count = 0;
    /* 0x6000:0A，末端 IO RS485 输入数据长度。 */
    unsigned int rs485_inputs_len = 0;
};

}  // namespace sinsun

#endif
