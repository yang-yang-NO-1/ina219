# INA219 v1.3 — Orange Pi 5 / 2S Li-ion / R010 10mΩ / SOC + UI API

本版本基于用户已经在 Orange Pi 5 实机验证的 v1.2：`/dev/i2c-5`、地址 `0x40`、`0x299B` 软件触发读取、R010 10mΩ 的电流数据均保持原有方式。**不修改 ESP32-S3 版，不强制断电，不替代 2S BMS。**

## 功能与限制

- 采样：电池电压、电流、分流电压、功率、充/放电/待机状态；错误时输出 `valid=false`，数值为 `null`。
- 报警：沿用 v1.2 的 6.80 V 预警、6.50 V 严重、连续 3 次确认、0.20 V 恢复回差；与 SOC 估计**相互独立**。
- 新增 **库仑计数**：采样电流在单调时钟上积分；电池放电扣除 mAh、充电添加 mAh（默认 98% 近似效率）。
- 新增 SOC 持久化：默认 `/var/lib/ina219/soc_state.txt`，正常运行每 120 秒保存，Ctrl+C / SIGTERM 正常退出时也保存；采用临时文件原子替换，保存到 eMMC/SD 的写入频率有限。
- 新增 SOC 静置电压修正：只在**连续 30 分钟**、电流绝对值不超过 30mA 时，基于通用 2S 开路电压表校准一次；已有积分值只调整其与电压估算差值的 20%，避免剧烈跳变。表格不是用户实测电芯曲线，仍然是粗估。
- 避免跨错误数据或断连时间段补算电量。超过 10 秒的测量间隔跳过积分，标记 `uncertain=true`；传感器失败时保留最近估算并标记 `stale=true`。
- SOC 以电池包**实际可用容量**（mAh）作为积分分母。**2S1P 两节串联时，电池包 mAh 等于单节电芯 mAh，而不是两倍；2S2P 则约等于每节 mAh 的两倍。**容量不知道时不擅自猜测，SOC 返回 `null`。
- 如果香橙派断电/关机，INA219 的读数不会被持续积分；期间可能存在电池自放电、GEK100/LDO 静态耗电以及离线充电。重启后可恢复保存的 SOC，但会标记 `uncertain=true`，直到人工重新标定；普通静置校准只能降低部分误差。
- 电流测量精度受 10mΩ 实际阻值、走线和温度影响；高负载时电压 SOC 不可靠，低电压报警也无法判断两节电芯各自的健康状态。参考 TI INA219 数据手册和通用库仑计数方法。

## 一、下载、上传、安装、解压、编译（完整命令）

先从聊天下载 `ina219_orangepi5_2s_r010_v1.3_soc_api.zip`，用 WinSCP 上传到香橙派 5 的 `/home/orangepi/`。以下在香橙派终端执行，旧版 `/home/orangepi/ina219` **不会被覆盖**：

```bash
sudo apt update
sudo apt install -y unzip build-essential i2c-tools curl python3
cd /home/orangepi
mkdir -p /home/orangepi/ina219_v1_3
unzip -o /home/orangepi/ina219_orangepi5_2s_r010_v1.3_soc_api.zip -d /home/orangepi/ina219_v1_3
cd /home/orangepi/ina219_v1_3
make clean
make
make test
```

若尚未确认硬件：

```bash
ls -l /dev/i2c-5
sudo i2cdetect -y 5
```

预期 `0x40`，你之前已实测该地址存在。测试时不要让新旧监控进程同时操作同一颗 INA219。

## 二、设置正确容量与初始电量

**必须设置电池包额定/实测可用容量 `--capacity-mah`。** 下面仅举例 `2500mAh`，不是你电池的实际容量，按真实电池规格更换。

首次使用、有可靠的初始 SOC（例如你已确认电池充满并停止充电，静置后）时，可以只在**第一次**运行时显式标定：

```bash
cd /home/orangepi/ina219_v1_3
sudo ./ina219_monitor --bus /dev/i2c-5 --address 0x40 --interval 1 --http-port 8765 --capacity-mah 2500 --initial-soc 100
```

**不要直接用 `--initial-soc 100` 测试你目前约 7.2 V、处于负载下的电池**，因为它并非已知满电状态；否则 UI 数字会从错误的 100% 开始积分。

如果现在不知道初始 SOC，先不传 `--initial-soc`，保持电流、电压和报警接口正常，但 `soc.available=false`、`soc_percent=null`，直到符合连续静置 30 分钟要求后用通用 OCV 表自动建立一次**粗略**起始 SOC。香橙派在正常运行时负载约 0.4~1.6A，可能无法满足静置条件，因此**建议先完成一次充满后的人工标定**。

下次正常启动不再传 `--initial-soc`，使用自动保存的状态：

```bash
cd /home/orangepi/ina219_v1_3
sudo ./ina219_monitor --bus /dev/i2c-5 --address 0x40 --interval 1 --http-port 8765 --capacity-mah 2500
```

容量修改之后，旧的状态文件会自动拒绝载入，需要重新建立初始值。`--initial-soc` 每用一次都会覆盖旧积分状态，**不要每次启动都加这个参数**。

### 可调参数

```text
--capacity-mah 2500          2S 电池包可用容量示例，不自动猜测
--initial-soc 100            手动指定已知 SOC，覆盖保存状态（0~100）
--soc-state-file PATH        持久化路径，默认 /var/lib/ina219/soc_state.txt
--no-soc-state               禁用 SOC 持久化
--soc-save-seconds 120       保存间隔（30~3600 秒）
--rest-current-ma 30         静置判据电流绝对值上限
--rest-seconds 1800          静置时间（600~7200 秒）
--charge-efficiency 0.98     充电积分效率（0.80~1.00）
```

当启用 SOC 时，采样间隔需要不大于 5 秒，默认 1 秒。出现长时间 I²C 故障、关机、休眠及程序停止，积分都不能覆盖这段时间。

## 三、终端与 UI 读取

监控进程运行时，**另开一个终端**执行：

```bash
curl -s http://127.0.0.1:8765/api/v1/battery | python3 -m json.tool
curl -s http://127.0.0.1:8765/api/v1/alarm | python3 -m json.tool
curl -s http://127.0.0.1:8765/api/v1/soc | python3 -m json.tool
python3 -m json.tool /run/ina219_battery.json
python3 examples/read_battery.py
```

HTTP 只监听本机 `127.0.0.1`，无用户名密码，不对外网或局域网开放。独立 Qt/Python/网页 UI 可以调用 HTTP；同进程 C++ UI 可以包含 `include/ina219_api.h`，使用 `battery::BatterySnapshot` 和其中的 `soc` 字段，但不建议 UI 与后台两个进程同时访问 INA219。

### JSON 样例（**模拟数据**，不是实测）

```json
{
  "schema_version": 2,
  "valid": true,
  "battery_voltage_v": 7.65,
  "current_a": 1.20,
  "pack_power_w": 9.18,
  "state": "discharging",
  "soc": {
    "available": true,
    "soc_percent": 73.25,
    "remaining_capacity_mah": 1831.25,
    "nominal_capacity_mah": 2500.0,
    "charged_mah_session": 50.0,
    "discharged_mah_session": 218.75,
    "source": "coulomb",
    "uncertain": false,
    "stale": false,
    "rest_qualified": false,
    "skipped_gaps": 0
  },
  "alarm": {"level":"normal","active":false,"sensor_fault":false}
}
```

实际上 `/api/v1/battery` 的对象还包括 `timestamp_ms`、`error`、`chip_current_a`、`current_mismatch` 等 v1.2 字段，以及报警阈值，保持原来的字段名。为兼容旧版 `/api/v1/alarm`，其结构不改变。新增 `/api/v1/soc` 只输出时间、传感器状态和 SOC 对象。

UI 建议始终检查 `valid`、`soc.available`、`soc.uncertain`、`soc.stale` 和 `alarm.sensor_fault`。**`uncertain=false` 也不代表电量计达到了专业电池管理芯片的精度**，只是表示自最近一次标定以来没有发现积分空档。

## 四、升级注意事项

1. 此项目实现的是**用户态软件 SOC 估计器**，不是硬件库仑计；关机后 SOC 无法持续跟踪。若未来想支持设备关机时充放电的准确容量累计，应考虑始终供电的独立电量计或低功耗 MCU。
2. INA219 只能检测流过它的电流。必须确保电池的充电和放电路径都经过 R010，否则某一方向不被积分，SOC 将失准。
3. 充电效率 98%、通用静置电压表都是工程近似；电池温度、容量衰减、均衡、充电截止特性会引入误差。
4. 6.8/6.5 V 报警保持不变。软件并不直接执行 `shutdown` 或触发 GEK100 RST，避免意外关机；如需增加安全关机，应单独联调。
5. 程序每 120 秒写一次状态文件，保存介质可能是 SD/eMMC。正常 Ctrl+C 也会保存；突然掉电仍可能丢失最近 120 秒的电量积分，重启后标记不确定。
6. `SOC` 未标定或未设置容量时返回 `null`，**不是 0%**；传感器故障时本次电压/电流数值 `null`，保存的 SOC 可以继续显示但会标记 `stale=true`。

## 五、工程文件

```text
Makefile                              C++11 编译与测试
src/main_orangepi.cpp                 Linux I2C、单次触发采样、终端、HTTP、存储
include/ina219_math.h                 R010/INA219 换算
include/ina219_api.h                  UI 快照、报警、JSON API
include/battery_soc.h                 SOC 积分与静置校准（不依赖硬件）
include/battery_soc_storage.h         SOC 断电保存/读取
examples/read_battery.py             Python UI 调用示例
tests/ina219_api_test.cpp             INA219 报警/API 测试
tests/battery_soc_test.cpp            SOC 积分、恢复、异常与 JSON 测试
README_orangepi.md                    本说明
```
