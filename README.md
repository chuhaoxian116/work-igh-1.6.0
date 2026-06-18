# IgH EtherCAT Master Demo

This project is a minimal userspace IgH EtherCAT master application.

- Cycle time: 1 ms
- Distributed clocks: enabled with SYNC0
- Control behavior: no drive enabling/control sequence yet
- Runtime output: prints PDO data every 5 seconds

## Structure

```text
myproject/
├── CMakeLists.txt
├── include/igh_master/
│   ├── app_config.h
│   ├── drive_pdo.h
│   └── ethercat_master.h
└── src/
    ├── drive_pdo.c
    ├── ethercat_master.c
    └── main.c
```

- `app_config.h`: slave identity, position, PDO indexes, and DC parameters
- `drive_pdo.*`: CiA 402 PDO map, domain registration, PDO read/write helpers
- `ethercat_master.*`: IgH master setup, DC setup, domain processing, 1 ms loop
- `main.c`: signal handling and realtime process setup

## Before Running On Hardware

Edit `include/igh_master/app_config.h`:

```c
#define DRIVE_POSITION      0
#define DRIVE_VENDOR_ID     0x00000000u
#define DRIVE_PRODUCT_CODE  0x00000000u
#define DC_ASSIGN_ACTIVATE  0x0300u
```

Use the values reported by:

```sh
ethercat slaves -v
ethercat pdos
```

`DC_ASSIGN_ACTIVATE` is vendor-specific. Check the drive ESI XML under
`Device -> Dc -> AssignActivate`.

## PDO Layout

RxPDO:

```c
int32_t target_position; /* 0x607A:00 */
uint16_t control_word;   /* 0x6040:00 */
int8_t operation_mode;   /* 0x6060:00 */
```

TxPDO:

```c
int32_t actual_position;       /* 0x6064:00 */
uint16_t status_word;          /* 0x6041:00 */
int8_t operation_mode_display; /* 0x6061:00 */
```

## Build

```sh
cd /home/js/igh/myproject
cmake -S . -B build
cmake --build build
```

## Run

The IgH master kernel modules and master service must already be running.

```sh
cd /home/js/igh/myproject
sudo ./build/igh_master
```

Stop with `Ctrl+C`.
