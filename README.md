# IgH EtherCAT 主站工程

这是一个用户态 IgH EtherCAT 主站工程，当前包含两个设备目录：

- `GSD620`：单伺服通讯示例，保留原有 CiA 402 相关逻辑。
- `SIASUN`：新松 6 轴伺服 + 末端 IO 主站通讯，只做参数下发、PDO 通讯和打印，不做使能和运动业务。

顶层 `CMakeLists.txt` 只做公共环境配置，例如 C/C++ 标准、IgH EtherCAT 路径和子目录接入；具体 target、源码、include、链接方式由各设备目录自己的 `CMakeLists.txt` 决定。

## 目录结构

```text
myproject/
├── CMakeLists.txt
├── GSD620/
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── SIASUN/
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── app_config.h
│   │   ├── axis_config.h
│   │   ├── ethercat_app.h
│   │   ├── pdo_config.h
│   │   └── sdo_parameter.h
│   └── src/
│       ├── axis_config.cpp
│       ├── ethercat_app.cpp
│       ├── main.cpp
│       └── sdo_parameter.cpp
├── doc/
│   ├── GSD620/
│   └── SIASUN/gcr10_1300/Axis1.xml ... Axis6.xml
└── thirdparty/
    ├── ethercat/
    └── tinyxml2/
        ├── tinyxml2.h
        ├── tinyxml2.cpp
        └── LICENSE.txt
```

## 编译

```sh
cd /home/js/ETCAT/igh/myproject
cmake -S . -B build
cmake --build build
```

构建产物默认输出到 `build/`：

```text
build/igh_master_gsd620
build/igh_master_sinsun
```

## IgH 环境

工程默认使用随工程携带的 IgH EtherCAT 安装目录：

```cmake
thirdparty/ethercat
```

如果现场路径不同，可以配置时覆盖：

```sh
cmake -S . -B build -DETHERCAT_PREFIX=/path/to/ethercat
```

运行前确认 IgH 主站服务和内核模块已经启动：

```sh
sudo /etc/init.d/ethercat start
ethercat slaves -v
ethercat pdos
```

## SIASUN 说明

SIASUN 拓扑：

- 逻辑 id `1-6`：伺服从站
- 逻辑 id `7`：末端 IO 从站
- IgH `position` 使用 0-based，因此伺服是 `0-5`，末端 IO 是 `6`
- 通讯周期：`1 ms`
- DC：`AssignActivate = 0x0300`

设备身份：

```text
Vendor ID          : 0x000008CF
Servo ProductCode : 0x00009252
EndIO ProductCode : 0x00009250
```

SIASUN 启动流程：

1. 读取 `Axis1.xml` 到 `Axis6.xml`
2. 解析每个 `<ServoParameters>` 的 `Id / Name / Value / qFmt`
3. 对 1-6 号伺服通过 SDO `0x2020:00` 下发参数
4. 每个伺服最后写入 `id=2,value=1,qFmt=0` 使参数生效
5. 配置 PDO / DC
6. 激活 master，进入 1 ms PDO 通讯循环
7. 周期打印伺服和末端 IO 的输入状态

SIASUN 不执行控制字使能序列，不下发运动轨迹。

## SIASUN 运行

推荐显式传入 Axis XML 目录：

```sh
cd /home/js/ETCAT/igh/myproject
sudo ./build/igh_master_sinsun ./doc/SIASUN/gcr10_1300
```

也可以使用绝对路径：

```sh
sudo ./build/igh_master_sinsun /home/js/ETCAT/igh/myproject/doc/SIASUN/gcr10_1300
```

按 `Ctrl+C` 停止程序。

## SIASUN 日志

SIASUN 默认会打印 XML 读取和 SDO 下发明细，方便现场确认参数是否正确写入。

日志开关在 `SIASUN/include/app_config.h`：

```cpp
constexpr bool kLogAxisXmlParameterDetails = true;
constexpr bool kLogSdoDownloadDetails = true;
```

如果现场输出太多，可以改成 `false`，保留汇总和错误日志。

## tinyxml2

SIASUN 使用 `tinyxml2` 读取 `Axis*.xml`。

当前工程只保留 tinyxml2 的最小可用文件：

```text
thirdparty/tinyxml2/tinyxml2.h
thirdparty/tinyxml2/tinyxml2.cpp
thirdparty/tinyxml2/LICENSE.txt
```

`SIASUN/CMakeLists.txt` 直接把 `tinyxml2.cpp` 编译进 `igh_master_sinsun`，不依赖系统安装包。

## GSD620 运行

```sh
cd /home/js/ETCAT/igh/myproject
sudo ./build/igh_master_gsd620
```

GSD620 的从站身份、PDO、DC、控制和打印配置在：

```text
GSD620/include/app_config.h
```
