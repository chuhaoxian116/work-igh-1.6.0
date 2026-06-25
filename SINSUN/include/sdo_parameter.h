#ifndef SINSUN_SDO_PARAMETER_H
#define SINSUN_SDO_PARAMETER_H

#include <cstddef>
#include <string>

#include "axis_config.h"
#include "ecrt.h"

namespace sinsun {

/*
 * 将 Axis*.xml 中的伺服参数写入 1-6 号伺服。
 *
 * master：IgH master 句柄。
 * parameters：6 个轴的参数集合。
 */
int write_axis_parameters(ec_master_t *master,
                          const AxisParameterSet &parameters);

/*
 * 将单个参数写入指定伺服。
 *
 * master：IgH master 句柄。
 * slave_position：IgH 0-based 从站位置。
 * parameter：需要写入的参数。
 */
int write_servo_parameter(ec_master_t *master,
                          uint16_t slave_position,
                          const ServoParameter &parameter);

}  // namespace sinsun

#endif
