#ifndef IGH_MASTER_APP_CONFIG_H
#define IGH_MASTER_APP_CONFIG_H

/*
 * Slave identity.
 *
 * Update these values before running on hardware. They can usually be read
 * with:
 *   ethercat slaves -v
 */
#define MASTER_INDEX        0
#define DRIVE_ALIAS         0
#define DRIVE_POSITION      0
#define DRIVE_VENDOR_ID     0x00000000u
#define DRIVE_PRODUCT_CODE  0x00000000u

/*
 * Generic CiA 402 PDO layout:
 *   SM2 output: RxPDO 0x1600
 *   SM3 input : TxPDO 0x1A00
 *
 * Adjust these indexes if the drive ESI or "ethercat pdos" output uses
 * different PDO assignment objects.
 */
#define DRIVE_RXPDO_INDEX   0x1600
#define DRIVE_TXPDO_INDEX   0x1A00

/*
 * 1 ms EtherCAT cycle with DC SYNC0.
 *
 * DC_ASSIGN_ACTIVATE is vendor-specific. 0x0300 is common for SYNC0-only
 * CiA 402 drives, but the exact value should be checked in the ESI XML:
 * Device -> Dc -> AssignActivate.
 */
#define CYCLE_TIME_NS       1000000u
#define DC_ASSIGN_ACTIVATE  0x0300u
#define DC_SYNC0_SHIFT_NS   0

#define PRINT_PERIOD_NS     5000000000ULL

#endif
