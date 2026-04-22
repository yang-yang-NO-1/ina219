# ESP32-S3 + INA219 双串 18650 电池检测调试说明

本文档用于记录本次 ESP32-S3 驱动 INA219 模块检测 **2S 18650 动力电池** 的接线、代码、校准和故障排查流程。

---

## 1. 系统目标

使用 ESP32-S3 读取 INA219，实现：

- 检测 2S 18650 电池组电压
- 检测充电 / 放电电流
- 判断当前状态：充电、放电、待机
- 粗略估算电池电量
- 为后续低电压保护、功耗统计、充放电管理做基础

当前电池参数：

| 项目 | 参数 |
|---|---|
| 电池类型 | 2S 18650 动力电池 |
| 满电电压 | 8.4V |
| 标称电压 | 7.4V |
| 建议低电保护 | 6.4V 左右 |
| 当前测试电压 | 约 8.18V |
| 目标测量电流 | 约 3A |
| 峰值电流 | 可能接近 5A |

---

## 2. INA219 模块说明

INA219 是高边电流检测芯片，可以测量：

- 总线电压 Bus Voltage
- 分流电阻压降 Shunt Voltage
- 电流 Current
- 功率 Power

注意：

**INA219 只能检测电压、电流，不能负责电池充电，也不能替代 BMS。**

2S 18650 必须配合：

- 2S BMS 保护板
- 2S 8.4V 锂电池充电器
- DC-DC 降压模块给 ESP32-S3 供电

---

## 3. 采样电阻选择

常见 INA219 小模块板上采样电阻为：

```text
R100 = 0.1Ω
```

R100 在大电流下压降和发热较大。

### R100 下功耗计算

| 电流 | 压降 | 发热 |
|---:|---:|---:|
| 0.2A | 0.02V | 0.004W |
| 1A | 0.1V | 0.1W |
| 3A | 0.3V | 0.9W |
| 5A | 0.5V | 2.5W |

所以 R100 只适合短时间、小电流测试，不适合长期 3A~5A。

后续建议改为：

```text
R010 = 0.01Ω
```

### R010 下功耗计算

| 电流 | 压降 | 发热 |
|---:|---:|---:|
| 3A | 0.03V | 0.09W |
| 5A | 0.05V | 0.25W |

推荐采样电阻：

```text
2512 封装
R010
0.01Ω
1%
1W 或 3W
优先 3W
```

推荐型号示例：

```text
RLP25FEGR010：3W
RLP25FECR010：1W
```

两者阻值相同，测量结果相同，区别是允许长期承受的发热功率不同。

---

## 4. 正确接线方式

INA219 应该串在 **电池正极路径**，不是串在负极路径。

### 主电流路径

```text
2S 电池组 / BMS P+  →  INA219 VIN+
INA219 VIN-         →  系统正极 / 负载正极 / 降压模块输入正极

2S 电池组 / BMS P-  →  系统 GND
系统 GND            →  负载负极
系统 GND            →  ESP32-S3 GND
系统 GND            →  INA219 GND
```

### INA219 与 ESP32-S3 接线

```text
INA219 VCC  →  ESP32-S3 3.3V
INA219 GND  →  ESP32-S3 GND
INA219 SDA  →  ESP32-S3 GPIO8
INA219 SCL  →  ESP32-S3 GPIO9
```

推荐使用 3.3V 给 INA219 供电，避免 I2C 被上拉到 5V。

---

## 5. 关于 INA219 供电和 I2C 上拉

INA219 模块如果 VCC 接 5V，且 SDA/SCL 通过 10k 上拉到 5V，会导致 ESP32-S3 GPIO 承受 5V 电平，有损坏风险。

ESP32-S3 是 3.3V GPIO，不耐 5V。

推荐：

```text
INA219 VCC 接 3.3V
SDA/SCL 上拉到 3.3V
```

如果必须 5V 供电，则需要：

- 拆掉模块上拉电阻，改为上拉到 3.3V
- 或使用 I2C 双向电平转换模块

---

## 6. 充放电方向判断

推荐定义：

```text
电池放电：
电流从 VIN+ 流向 VIN-
通常读数为正数

电池充电：
电流从 VIN- 流向 VIN+
通常读数为负数
```

软件判断：

```cpp
if (current_mA > 30) {
    // 放电
} else if (current_mA < -30) {
    // 充电
} else {
    // 待机
}
```

如果实测方向相反，不要急着改线，可以软件取反：

```cpp
#define CURRENT_DIRECTION -1.0f
```

---

## 7. PlatformIO 配置

`platformio.ini` 示例：

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
```

说明：

```text
-DARDUINO_USB_CDC_ON_BOOT=1
```

用于让 ESP32-S3 使用芯片自带 USB CDC 串口输出普通程序日志。

注意：

- USB CDC 可以看到 `Serial.println()` 输出
- Boot ROM 最早期硬重启信息不一定会打印到 USB CDC
- 如果要稳定看启动 / 崩溃 / Boot 日志，建议使用 UART0 或开发板 USB-UART 口

---

## 8. INA219 R100 测试代码

当前先按照 R100 = 0.1Ω 采样电阻测试。

新建：

```text
src/main.cpp
```

代码如下：

```cpp
#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 8
#define I2C_SCL 9

#define INA219_ADDR 0x40

#define INA219_REG_CONFIG        0x00
#define INA219_REG_SHUNT_VOLTAGE 0x01
#define INA219_REG_BUS_VOLTAGE   0x02
#define INA219_REG_POWER         0x03
#define INA219_REG_CURRENT       0x04
#define INA219_REG_CALIBRATION   0x05

#define SHUNT_OHMS 0.1f
#define CURRENT_LSB_A 0.0001f
#define POWER_LSB_W (20.0f * CURRENT_LSB_A)
#define INA219_CALIBRATION_VALUE 4096

#define CURRENT_DIRECTION 1.0f
#define CURRENT_ZERO_OFFSET_MA 0.0f
#define CURRENT_GAIN 1.0f
#define VOLTAGE_GAIN 1.0f

#define CURRENT_DEAD_ZONE_MA 30.0f
#define BATTERY_LOW_VOLTAGE 6.4f

void ina219WriteRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  Wire.write((value >> 8) & 0xFF);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

uint16_t ina219ReadRegister(uint8_t reg) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);

  Wire.requestFrom(INA219_ADDR, (uint8_t)2);

  uint16_t value = 0;
  if (Wire.available() >= 2) {
    value = Wire.read() << 8;
    value |= Wire.read();
  }

  return value;
}

bool ina219Detect() {
  Wire.beginTransmission(INA219_ADDR);
  return Wire.endTransmission() == 0;
}

void ina219Init() {
  uint16_t config = 0x399F;

  ina219WriteRegister(INA219_REG_CONFIG, config);
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);
}

float ina219GetShuntVoltage_mV() {
  int16_t raw = (int16_t)ina219ReadRegister(INA219_REG_SHUNT_VOLTAGE);
  return raw * 0.01f;
}

float ina219GetBusVoltage_V() {
  uint16_t raw = ina219ReadRegister(INA219_REG_BUS_VOLTAGE);
  raw = raw >> 3;
  return raw * 0.004f;
}

float ina219GetCurrent_mA() {
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);

  int16_t raw = (int16_t)ina219ReadRegister(INA219_REG_CURRENT);

  float current_A = raw * CURRENT_LSB_A;
  float current_mA = current_A * 1000.0f;

  current_mA = current_mA * CURRENT_DIRECTION;
  current_mA = current_mA - CURRENT_ZERO_OFFSET_MA;
  current_mA = current_mA * CURRENT_GAIN;

  return current_mA;
}

float ina219GetPower_mW() {
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);

  uint16_t raw = ina219ReadRegister(INA219_REG_POWER);

  float power_W = raw * POWER_LSB_W;
  float power_mW = power_W * 1000.0f;

  return power_mW;
}

float calcCurrentFromShunt_mA(float shuntVoltage_mV) {
  float shuntVoltage_V = shuntVoltage_mV / 1000.0f;
  float current_A = shuntVoltage_V / SHUNT_OHMS;
  return current_A * 1000.0f;
}

int estimateBatteryPercent_2S(float voltage) {
  if (voltage >= 8.40f) return 100;
  if (voltage >= 8.20f) return 90;
  if (voltage >= 8.00f) return 80;
  if (voltage >= 7.80f) return 70;
  if (voltage >= 7.60f) return 60;
  if (voltage >= 7.40f) return 50;
  if (voltage >= 7.20f) return 35;
  if (voltage >= 7.00f) return 25;
  if (voltage >= 6.80f) return 15;
  if (voltage >= 6.60f) return 8;
  if (voltage >= 6.40f) return 3;
  return 0;
}

const char* getPowerState(float current_mA) {
  if (current_mA > CURRENT_DEAD_ZONE_MA) {
    return "DISCHARGING";
  }

  if (current_mA < -CURRENT_DEAD_ZONE_MA) {
    return "CHARGING";
  }

  return "IDLE";
}

void printCalibrationInfo() {
  Serial.println();
  Serial.println("========== INA219 Calibration Info ==========");
  Serial.println("Current shunt resistor: R100 = 0.1 ohm");
  Serial.print("SHUNT_OHMS: ");
  Serial.println(SHUNT_OHMS, 4);

  Serial.print("CURRENT_LSB_A: ");
  Serial.println(CURRENT_LSB_A, 7);

  Serial.print("POWER_LSB_W: ");
  Serial.println(POWER_LSB_W, 7);

  Serial.print("CALIBRATION VALUE: ");
  Serial.println(INA219_CALIBRATION_VALUE);

  Serial.println();
  Serial.println("If discharging current is negative, change:");
  Serial.println("#define CURRENT_DIRECTION -1.0f");
  Serial.println();
  Serial.println("If idle current is not zero, change:");
  Serial.println("#define CURRENT_ZERO_OFFSET_MA xxx");
  Serial.println();
  Serial.println("If current has scale error, change:");
  Serial.println("#define CURRENT_GAIN xxx");
  Serial.println("=============================================");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-S3 + INA219 Battery Monitor");
  Serial.println("Battery: 2S 18650, max 8.4V");
  Serial.println("Mode: R100 calibration");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  if (!ina219Detect()) {
    Serial.println("ERROR: INA219 not found!");
    Serial.println("Check wiring: VCC, GND, SDA, SCL, address 0x40");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("INA219 detected.");

  ina219Init();
  printCalibrationInfo();
}

void loop() {
  float busVoltage_V = ina219GetBusVoltage_V() * VOLTAGE_GAIN;
  float shuntVoltage_mV = ina219GetShuntVoltage_mV();

  float batteryVoltage_V = busVoltage_V + (shuntVoltage_mV / 1000.0f);

  float current_mA = ina219GetCurrent_mA();
  float power_mW = ina219GetPower_mW();

  float currentByShunt_mA = calcCurrentFromShunt_mA(shuntVoltage_mV);

  int soc = estimateBatteryPercent_2S(batteryVoltage_V);
  const char* state = getPowerState(current_mA);

  Serial.println("------------------------------------------------");
  Serial.print("Bus Voltage      : ");
  Serial.print(busVoltage_V, 3);
  Serial.println(" V");

  Serial.print("Battery Voltage  : ");
  Serial.print(batteryVoltage_V, 3);
  Serial.println(" V");

  Serial.print("Shunt Voltage    : ");
  Serial.print(shuntVoltage_mV, 3);
  Serial.println(" mV");

  Serial.print("Current Register : ");
  Serial.print(current_mA, 2);
  Serial.println(" mA");

  Serial.print("Current By Shunt : ");
  Serial.print(currentByShunt_mA, 2);
  Serial.println(" mA");

  Serial.print("Power            : ");
  Serial.print(power_mW, 2);
  Serial.println(" mW");

  Serial.print("Battery SOC      : ");
  Serial.print(soc);
  Serial.println(" %");

  Serial.print("Power State      : ");
  Serial.println(state);

  if (batteryVoltage_V <= BATTERY_LOW_VOLTAGE) {
    Serial.println("WARNING: Battery low! Suggested stop discharge.");
  }

  delay(1000);
}
```

---

## 9. R100 校准说明

### 9.1 空载零点校准

如果空载时显示：

```text
Current Register : 15.30 mA
```

修改：

```cpp
#define CURRENT_ZERO_OFFSET_MA 15.3f
```

### 9.2 电流方向校准

如果电池放电时显示正电流：

```text
Power State : DISCHARGING
```

保持：

```cpp
#define CURRENT_DIRECTION 1.0f
```

如果电池放电时显示负电流：

```text
Power State : CHARGING
```

修改：

```cpp
#define CURRENT_DIRECTION -1.0f
```

### 9.3 电流比例校准

用万用表或电子负载测真实电流。

例如：

```text
真实电流：1.50A
INA219 显示：1.42A
```

则：

```text
CURRENT_GAIN = 1.50 / 1.42 = 1.056
```

修改：

```cpp
#define CURRENT_GAIN 1.056f
```

### 9.4 电压比例校准

例如：

```text
万用表测电池：8.20V
INA219 显示：8.16V
```

则：

```text
VOLTAGE_GAIN = 8.20 / 8.16 = 1.0049
```

修改：

```cpp
#define VOLTAGE_GAIN 1.0049f
```

---

## 10. 后续改 R010 后需要修改的代码

当采样电阻改为：

```text
R010 = 0.01Ω
```

修改如下：

```cpp
#define SHUNT_OHMS 0.01f
#define CURRENT_LSB_A 0.0002f
#define POWER_LSB_W (20.0f * CURRENT_LSB_A)
#define INA219_CALIBRATION_VALUE 20480
```

也就是：

| 项目 | R100 | R010 |
|---|---:|---:|
| SHUNT_OHMS | 0.1 | 0.01 |
| CURRENT_LSB_A | 0.0001 | 0.0002 |
| 校准寄存器 | 4096 | 20480 |
| 电流分辨率 | 0.1mA | 0.2mA |
| 理论量程 | 约 3.2A | 约 6.5A |

---

## 11. 故障排查记录

### 11.1 现象：Bus Voltage 只有 0.8V 左右

串口示例：

```text
Bus Voltage      : 0.844 V
Battery Voltage  : 0.863 V
Shunt Voltage    : 19.280 mV
Current Register : 192.80 mA
Power State      : DISCHARGING
```

但实际电池电压为 8.18V。

这种情况下：

- 电流测量是正常的
- 因为 R100 下 19.28mV / 0.1Ω = 192.8mA
- 但电池电压测量不正常

原因通常是：

```text
INA219 GND 没有和电池负极 / BMS P- 共地
或者 INA219 被串到了负极路径
或者 VIN+ / VIN- 没有正确串在正极路径
```

### 11.2 正常情况下应该测到

用万用表黑表笔接 INA219 GND：

| 测量点 | 正常值 |
|---|---:|
| VIN+ 对 GND | 约 8.18V |
| VIN- 对 GND | 约 8.16V~8.18V |
| VIN+ 与 VIN- 之间 | 约 20mV，取决于电流 |

修正共地和正极串联接法后，串口应该显示：

```text
Bus Voltage      : 8.1V 左右
Battery Voltage  : 8.18V 左右
Current          : 200mA 左右
Power            : 1.6W 左右
Power State      : DISCHARGING
```

---

## 12. 判断充电 / 放电 / 待机

正常逻辑：

| 电流值 | 状态 |
|---:|---|
| > +30mA | 放电 |
| < -30mA | 充电 |
| -30mA ~ +30mA | 待机 |

注意：

- 小于几十 mA 的读数可能是噪声或零点误差
- 不要用 `>0` 或 `<0` 直接判断
- 建议使用 30mA 左右死区

---

## 13. 最终建议

当前原型调试阶段：

```text
可以先用 INA219 + R100 跑通通信、电压、电流和方向判断
```

正式使用阶段：

```text
建议改 R010 = 0.01Ω
建议用 2512 3W 采样电阻
大电流路径走粗线
INA219 测量端尽量靠近采样电阻两端
2S 18650 必须使用 2S BMS
ESP32-S3 必须通过 DC-DC 降压供电
```

核心原则：

```text
INA219 负责检测
ESP32-S3 负责逻辑和显示
BMS 负责电池保护
2S 充电器负责安全充电
```
