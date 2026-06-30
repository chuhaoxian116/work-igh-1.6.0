# SIASUN 实时通讯测试

这里的 demo 链接 `siasun_core`，直接复用 `SIASUN/src` 和
`SIASUN/include` 的主站配置、SDO、PDO 和 DC 接口。

`siasun_realtime_communication_test` 默认执行现有 Servo 6 单关节正弦老化
运动，同时记录周期抖动、超周期、Working Counter、DC 误差、进程 CPU
时间、峰值内存、缺页和上下文切换。设置 `enable_motion=0` 后可切换为
所有输出保持 0 的纯通讯模式。

## 编译和运行

```sh
cd /home/js/ETCAT/igh/myproject
cmake -S . -B build
cmake --build build

sudo ./build/SIASUN/test/siasun_realtime_communication_test \
    ./doc/SIASUN/gcr10_1300 60 2 fifo -1 1 1 1 1
```

位置参数依次为：

```text
AxisXmlDirectory duration_s cpu_id policy priority mlock dc_sync_cycles enable_motion require_endio_op
```

- `cpu_id=-1`：不设置 CPU 亲和性。
- `policy=fifo|rr|other`：调度策略。
- `priority=-1`：使用当前策略的最高优先级；Linux 上 FIFO 通常为 99。
- `mlock=1|0`：开启或关闭内存锁定。
- `dc_sync_cycles=1`：每个 1 ms 周期发送一次 DC 同步请求。
- `enable_motion=1|0`：开启或关闭现有 Servo 6 单关节老化运动。
- `require_endio_op=1|0`：`1` 要求7站全部 OP；`0` 只使用6个伺服
  的 OP 状态和 `WC>=18` 判断通信通过。

实时设置需要 root 或相应的 `CAP_SYS_NICE`、`CAP_IPC_LOCK` 能力。必须检查
日志中的 `[RT] actual`，不能只根据命令行认定实时设置已经生效。

## EndIO 临时异常模式

默认 `REQUIRE_ENDIO_OP=1`，保持6个伺服和 EndIO 共7站的完整判断。现场
EndIO 临时异常、只测试6个关节通讯时设置：

```sh
sudo env TEST_TAG=T-SERVO6 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 ENABLE_MOTION=1 REQUIRE_ENDIO_OP=0 \
    LOAD_PROFILE=idle ./run_realtime_test.sh
```

该开关只改变质量统计的启动条件和 WC 通过门槛。EndIO 的主站配置、PDO、
状态读取和日志保持不变；报告中会显示 `6 Servo only` 和通过门槛
`WC>=18`。恢复 EndIO 后应改回 `REQUIRE_ENDIO_OP=1`。

## 运动配置

所有测试组默认复用 `app_config.h` 中已经验证过的 Servo 6 老化轨迹：

```text
控制对象：Servo 6
运行模式：CSP（模式 8）
运动中心：进入运动状态时的实际位置
幅值：100000 pulse
峰峰值：200000 pulse
周期：20 s
渐入时间：2 s
```

其它 5 个伺服和末端 IO 输出保持为 0。开始压力测试前，先运行 T01 确认
Servo 6 方向、范围、机械空间和急停状态正常。所有对照组必须保持同一套
轨迹参数，否则运动负载本身会成为额外变量。

## 测试前准备

目标机停止正式程序，同一时刻只能有一个程序请求 IgH Master：

```sh
sudo pkill -INT igh_master_sinsun
```

确认目标环境和从站状态：

```sh
uname -a
lsmod | grep '^ec_'
ethercat slaves
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | sort -u
```

当前目标机固定使用 `PREEMPT_RT + ec_igc`，CPU 拓扑为 4 个物理核心、
8 个逻辑 CPU：

```text
物理核心 0：逻辑 CPU 0、4
物理核心 1：逻辑 CPU 1、5
物理核心 2：逻辑 CPU 2、6
物理核心 3：逻辑 CPU 3、7
```

建议的基础分工：

```text
CPU 0、4：系统和测试监控
CPU 1、5：CPU/内存压力
CPU 2、6：APP 物理核心；APP 绑定 CPU 2，CPU 6 保持空闲
CPU 3、7：EtherCAT-OP 物理核心；OP 绑定 CPU 3，CPU 7 保持空闲
```

只做 `taskset` 绑定不等于物理核心完全隔离，后台内核任务仍可能进入。
第一阶段先测试绑核效果，后续再单独测试 `isolcpus/nohz_full/rcu_nocbs`。

## 压力脚本

从 `SIASUN/test` 目录运行：

```sh
cd /home/js/ETCAT/igh/myproject/SIASUN/test

sudo env \
    TEST_TAG=T07 \
    TEST_BIN=../../build/SIASUN/test/siasun_realtime_communication_test \
    AXIS_DIR=../../doc/SIASUN/gcr10_1300 \
    DURATION_S=600 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 ENABLE_MOTION=1 REQUIRE_ENDIO_OP=1 \
    LOAD_PROFILE=mixed LOAD_CPUSET=1,5 \
    CPU_WORKERS=2 CPU_LOAD=100 VM_WORKERS=1 VM_BYTES=8G \
    ./run_realtime_test.sh
```

`LOAD_PROFILE` 支持 `idle`、`cpu`、`memory` 和 `mixed`。后三项依赖
`stress-ng`；压力进程会一直运行到通讯测试结束。每轮结果保存到：

```text
test/results/<时间>-<TEST_TAG>-<压力类型>/
```

## 第一阶段测试表

所有测试固定使用：

```text
PREEMPT_RT、ec_igc、1 ms、FIFO 99、mlock 开、Servo 6 老化运动
```

先运行 10 分钟冒烟测试，确认命令正确；正式测试每组运行 30 分钟并重复
3 次。除了表中变量，其它参数保持不变。

| 编号 | APP CPU | 压力类型 | 压力 CPU | 压力参数 | DC 间隔 | 目的 |
|---|---:|---|---|---|---:|---|
| T00 | 不绑定 | idle | - | - | 1 | 保存当前未调优基线 |
| T01 | CPU 2 | idle | - | CPU 6 保持空闲 | 1 | 测试只绑定通讯线程的收益 |
| T02 | CPU 2 | cpu | CPU 1、5 | 50% | 1 | 测试其它物理核普通负载 |
| T03 | CPU 2 | cpu | CPU 1、5 | 100% | 1 | 测试其它物理核满载 |
| T04A | CPU 2 | cpu | CPU 2 | 100% | 1 | 测试同一逻辑 CPU 竞争 |
| T04B | CPU 2 | cpu | CPU 6 | 100% | 1 | 测试 SMT 兄弟线程竞争 |
| T05 | CPU 2 | memory | CPU 1、5 | 1 worker / 8G | 1 | 测试内存带宽和回收压力 |
| T06 | CPU 2 | memory | CPU 1、5 | 1 worker / 16G | 1 | 测试约 50% 物理内存压力 |
| T07 | CPU 2 | mixed | CPU 1、5 | CPU 100% + 8G | 1 | 测试混合高负载 |
| T08 | CPU 2 | idle | - | CPU 6 保持空闲 | 2 | 比较每 2 周期 DC 同步 |
| T09 | CPU 2 | idle | - | CPU 6 保持空闲 | 5 | 比较每 5 周期 DC 同步 |
| T10 | CPU 2 | mixed | CPU 2 | CPU 100% + 16G | 1 | 混合负载下比较 DC 间隔 |
| T11 | 最优配置 | mixed | 其它核心 | CPU 100% + 8G | 最优值 | 8 小时长稳测试 |

## 第一阶段命令

进入测试目录并定义公共路径：

```sh
cd /home/js/ETCAT/igh/myproject/SIASUN/test

export TEST_BIN=../../build/SIASUN/test/siasun_realtime_communication_test
export AXIS_DIR=../../doc/SIASUN/gcr10_1300
```

T00 未绑定基线：

```sh
sudo env TEST_TAG=T00 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=-1 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=idle ./run_realtime_test.sh
```

T01 绑定 APP CPU，下面以 CPU 2 为例：

```sh
sudo env TEST_TAG=T01 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=idle ./run_realtime_test.sh
```

CPU 压力测试通过 `TEST_TAG`、`CPU_LOAD` 和 `LOAD_CPUSET` 组合：

```sh
sudo env TEST_TAG=T03 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=cpu LOAD_CPUSET=1,5 \
    CPU_WORKERS=2 CPU_LOAD=100 ./run_realtime_test.sh
```

T04A 测试同一个逻辑 CPU 竞争：

```sh
sudo env TEST_TAG=T04A TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=cpu LOAD_CPUSET=2 \
    CPU_WORKERS=1 CPU_LOAD=100 ./run_realtime_test.sh
```

T04B 测试 SMT 兄弟线程竞争：

```sh
sudo env TEST_TAG=T04B TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=cpu LOAD_CPUSET=6 \
    CPU_WORKERS=1 CPU_LOAD=100 ./run_realtime_test.sh
```

内存压力从 8G 开始，确认系统没有 OOM 后再运行 16G：

```sh
sudo env TEST_TAG=T05 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=memory LOAD_CPUSET=1,5 \
    VM_WORKERS=1 VM_BYTES=8G ./run_realtime_test.sh
```

混合压力：

```sh
sudo env TEST_TAG=T07 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=mixed LOAD_CPUSET=1,5 \
    CPU_WORKERS=2 CPU_LOAD=100 VM_WORKERS=1 VM_BYTES=8G \
    ./run_realtime_test.sh
```

T08/T09 只修改 `TEST_TAG` 和 `DC_SYNC_CYCLES`，不要同时修改负载。

混合压力：

```sh
sudo env TEST_TAG=T10 TEST_BIN="$TEST_BIN" AXIS_DIR="$AXIS_DIR" \
    DURATION_S=1800 CPU_ID=2 POLICY=fifo PRIORITY=-1 MLOCK=1 \
    DC_SYNC_CYCLES=1 LOAD_PROFILE=mixed LOAD_CPUSET=2 \
    CPU_WORKERS=1 CPU_LOAD=100 VM_WORKERS=1 VM_BYTES=8G \
    ./run_realtime_test.sh
```

## EtherCAT-OP 测试

`ec_igc` 的周期 RX/TX 主要由用户态通讯线程轮询，因此先完成 T00-T10，
再单独测试 `EtherCAT-OP`。它会在应用激活 Master 时重新创建，所以必须
在测试程序启动并打印 `[RT] actual` 后调整。

| 编号 | APP 线程 | EtherCAT-OP | 目的 |
|---|---|---|---|
| O00 | CPU 2 / FIFO 99 | 不绑定 / SCHED_OTHER | OP 基线 |
| O01 | CPU 2 / FIFO 99 | CPU 3 / SCHED_OTHER | 只测试 OP 绑核 |
| O02 | CPU 2 / FIFO 99 | CPU 3 / FIFO 40 | 测试 OP 实时调度 |
| O03 | CPU 2 / FIFO 99 | CPU 3 / FIFO 60 | 比较 OP 优先级 |

使用 `run_igh_master_test.sh` 自动等待新创建的 `EtherCAT-OP`，然后设置
它的 CPU 和优先级。O00 保持 OP 默认配置：

```sh
sudo env TEST_TAG=O00 DURATION_S=1800 APP_CPU=2 \
    OP_CPU=-1 OP_POLICY=other OP_PRIORITY=0 \
    LOAD_PROFILE=idle ./run_igh_master_test.sh
```

O01 只绑定 OP 到 CPU 3：

```sh
sudo env TEST_TAG=O01 DURATION_S=1800 APP_CPU=2 \
    OP_CPU=3 OP_POLICY=other OP_PRIORITY=0 \
    LOAD_PROFILE=idle ./run_igh_master_test.sh
```

O02/O03 设置 OP 实时优先级：

```sh
sudo env TEST_TAG=O02 DURATION_S=1800 APP_CPU=2 \
    OP_CPU=3 OP_POLICY=fifo OP_PRIORITY=40 \
    LOAD_PROFILE=idle ./run_igh_master_test.sh

sudo env TEST_TAG=O03 DURATION_S=1800 APP_CPU=2 \
    OP_CPU=3 OP_POLICY=fifo OP_PRIORITY=60 \
    LOAD_PROFILE=idle ./run_igh_master_test.sh
```

脚本每秒记录 `EtherCAT-OP` 的 CPU、调度类别、实时优先级、CPU 占用和
`schedstat`，同时保留应用侧周期抖动、WC 和 DC 报告。不要把
`EtherCAT-OP` 也设置为 FIFO 99。

## 每轮结果记录

| 指标 | 单位/记录方式 | 建议判断 |
|---|---|---|
| 平均、最大绝对抖动 | us | 与 T00/T01 相比越低越稳定 |
| 最小、最大实际周期 | us | 不应出现不可接受的长尾 |
| 严重超周期次数 | 次 | 目标为 0 |
| Domain 完整周期率 | % | 目标接近 100% |
| WC 不完整/无数据 | 次 | 目标为 0 |
| DC 平均/最大误差 | us | 越低越稳定 |
| 进程用户/内核 CPU | s | 同等通讯质量下越低越好 |
| 进程 CPU 占用 | `%CPU` | 由 `/usr/bin/time -v` 记录 |
| 峰值常驻内存 | KiB | 观察是否随时间增长 |
| major/minor faults | 次 | OP 后 major fault 目标为 0 |
| 主动/被动上下文切换 | 次 | 用于解释抖动尖峰 |
| 从站掉线/AL 状态变化 | 次 | 目标为 0 |

每轮首先确认日志中：

```text
[RT] actual policy=1 priority=99 cpu=<目标 CPU>
motion: only Servo 6 enabled
从站在线 / OP: 7 / 7
```

硬性失败条件：

```text
严重超周期次数 > 0
Domain 不完整周期或无过程数据周期 > 0
OP 之后 major fault > 0
从站掉线、AL 状态异常或程序非零退出
```

如果各组都没有硬性失败，再按最大抖动、DC 最大误差、CPU 占用和上下文
切换次数进行排序。

当前程序可直接改变 DC 同步频率，但尚未实现应用层 DC PI 控制器，因此
`Kp/Ki` 不应先放进同一轮矩阵。后续加入参考时钟误差反馈和 PI 接口后，
再固定系统调度参数，对 `Kp`、`Ki`、积分限幅和输出限幅做独立扫描。
