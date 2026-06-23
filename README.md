# IgH EtherCAT 主站示例

这是一个最小化的用户态 IgH EtherCAT 主站工程。

- 通讯周期：1 ms
- 分布式时钟：启用 SYNC0
- 控制逻辑：当前不做使能和运动控制
- 运行输出：每 5 秒打印一次 PDO 数据

## 目录结构

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

- `app_config.h`：从站身份、站号、PDO 编号、DC 参数
- `drive_pdo.*`：CiA 402 PDO 映射、domain 注册、PDO 读写封装
- `ethercat_master.*`：IgH 主站初始化、DC 配置、domain 处理、1 ms 循环
- `main.c`：信号处理和实时进程设置

## 上机前配置

先修改 `include/igh_master/app_config.h`：

```c
#define DRIVE_POSITION      0
#define DRIVE_VENDOR_ID     0x00000000u
#define DRIVE_PRODUCT_CODE  0x00000000u
#define DC_ASSIGN_ACTIVATE  0x0300u
```

从站的 Vendor ID、Product Code、站号等信息可以通过下面命令查看：

```sh
ethercat slaves -v
ethercat pdos
```

`DC_ASSIGN_ACTIVATE` 和厂家有关，建议以伺服 ESI XML 中
`Device -> Dc -> AssignActivate` 的值为准。

## PDO 布局

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

## 编译

```sh
cd /home/js/igh/myproject
cmake -S . -B build
cmake --build build
```

## 运行

IGH主站服务启动
sudo /etc/init.d/ethercat start

运行前需要确认 IgH 主站内核模块和主站服务已经启动。

```sh
cd /home/js/igh/myproject
sudo ./build/igh_master
```

按 `Ctrl+C` 停止程序。
