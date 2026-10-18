/* 纯主机及 EtherCAT 周期实时性诊断接口。 */
#ifndef ECAT2CAN_RT_H
#define ECAT2CAN_RT_H

#include <stdint.h>

#include "ecat2can_fieldbus.h"

void rt_print_env(void);
int cmd_rt_host(int seconds);
int cmd_rt(fieldbus_t *fb, int seconds, uint8_t channel);

#endif
