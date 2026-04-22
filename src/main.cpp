#include <Arduino.h>
#include <Wire.h>

// =======================
// ESP32-S3 I2C 引脚
// =======================
#define I2C_SDA 8
#define I2C_SCL 9

// =======================
// INA219 地址
// 默认一般是 0x40
// =======================
#define INA219_ADDR 0x40

// =======================
// INA219 寄存器
// =======================
#define INA219_REG_CONFIG        0x00
#define INA219_REG_SHUNT_VOLTAGE 0x01
#define INA219_REG_BUS_VOLTAGE   0x02
#define INA219_REG_POWER         0x03
#define INA219_REG_CURRENT       0x04
#define INA219_REG_CALIBRATION   0x05

// =======================
// 当前先按 R100 = 0.1Ω
// =======================
// 原装 INA219 小模块常见采样电阻：R100 = 0.1Ω
#define SHUNT_OHMS 0.1f

// R100 推荐 current LSB = 100uA
// 也就是电流寄存器每 1 个计数 = 0.1mA
#define CURRENT_LSB_A 0.0001f

// 功率 LSB = 20 × Current LSB
#define POWER_LSB_W (20.0f * CURRENT_LSB_A)

// INA219 校准寄存器公式：
// CAL = 0.04096 / (CURRENT_LSB_A × SHUNT_OHMS)
// R100 + 100uA 时：CAL = 4096
#define INA219_CALIBRATION_VALUE 4096

// =======================
// 方向与误差校准参数
// =======================
// 如果实测放电时电流是正数，保持 1.0
// 如果实测放电时电流是负数，改成 -1.0
#define CURRENT_DIRECTION 1.0f

// 空载零点偏移，先写 0
// 例如空载显示 12.5mA，则改成 12.5
#define CURRENT_ZERO_OFFSET_MA 0.0f

// 电流比例校准
// 如果万用表测 2.00A，INA219 显示 1.92A
// CURRENT_GAIN = 2.00 / 1.92 = 1.0417
#define CURRENT_GAIN 1.0f

// 电压比例校准
// 如果万用表测 8.20V，INA219 显示 8.16V
// VOLTAGE_GAIN = 8.20 / 8.16 = 1.0049
#define VOLTAGE_GAIN 1.0f

// 判断充放电的死区
#define CURRENT_DEAD_ZONE_MA 30.0f

// 2S 锂电池低电压保护提示
#define BATTERY_LOW_VOLTAGE 6.4f

// =======================
// I2C 写寄存器
// =======================
void ina219WriteRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  Wire.write((value >> 8) & 0xFF);
  Wire.write(value & 0xFF);
  Wire.endTransmission();
}

// =======================
// I2C 读寄存器
// =======================
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

// =======================
// 检测 INA219 是否在线
// =======================
bool ina219Detect() {
  Wire.beginTransmission(INA219_ADDR);
  return Wire.endTransmission() == 0;
}

// =======================
// INA219 初始化
// =======================
void ina219Init() {
  // 配置说明：
  // 0x399F / 0x3C1F 都常见
  //
  // 这里使用：
  // Bus voltage range: 32V
  // PGA gain: ±320mV
  // Bus ADC: 12-bit
  // Shunt ADC: 12-bit
  // Mode: Shunt and Bus continuous
  //
  // 0x399F 更偏向常见连续测量配置
  // 0x3C1F 是 Adafruit 32V/2A 风格配置之一
  uint16_t config = 0x399F;

  ina219WriteRegister(INA219_REG_CONFIG, config);
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);
}

// =======================
// 读取分流电压，单位 mV
// INA219 shunt voltage LSB = 10uV = 0.01mV
// =======================
float ina219GetShuntVoltage_mV() {
  int16_t raw = (int16_t)ina219ReadRegister(INA219_REG_SHUNT_VOLTAGE);
  return raw * 0.01f;
}

// =======================
// 读取总线电压，单位 V
// INA219 bus voltage LSB = 4mV
// 寄存器低 3 位是状态位，要右移 3 位
// =======================
float ina219GetBusVoltage_V() {
  uint16_t raw = ina219ReadRegister(INA219_REG_BUS_VOLTAGE);
  raw = raw >> 3;
  return raw * 0.004f;
}

// =======================
// 读取电流，单位 mA
// 电流寄存器是有符号数
// =======================
float ina219GetCurrent_mA() {
  // 每次读 current 前重新写 calibration
  // INA219 有时复位或异常后 calibration 会丢
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);

  int16_t raw = (int16_t)ina219ReadRegister(INA219_REG_CURRENT);

  float current_A = raw * CURRENT_LSB_A;
  float current_mA = current_A * 1000.0f;

  // 方向修正、零点修正、比例修正
  current_mA = current_mA * CURRENT_DIRECTION;
  current_mA = current_mA - CURRENT_ZERO_OFFSET_MA;
  current_mA = current_mA * CURRENT_GAIN;

  return current_mA;
}

// =======================
// 读取功率，单位 mW
// =======================
float ina219GetPower_mW() {
  ina219WriteRegister(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE);

  uint16_t raw = ina219ReadRegister(INA219_REG_POWER);

  float power_W = raw * POWER_LSB_W;
  float power_mW = power_W * 1000.0f;

  return power_mW;
}

// =======================
// 通过分流电压手动计算电流
// 用来交叉检查 INA219 电流寄存器是否合理
// I = Vshunt / Rshunt
// =======================
float calcCurrentFromShunt_mA(float shuntVoltage_mV) {
  float shuntVoltage_V = shuntVoltage_mV / 1000.0f;
  float current_A = shuntVoltage_V / SHUNT_OHMS;
  return current_A * 1000.0f;
}

// =======================
// 2S 锂电池电量粗略估算
// 注意：带负载时电压会被拉低，只能粗略判断
// =======================
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

// =======================
// 判断充电 / 放电 / 待机
// =======================
const char* getPowerState(float current_mA) {
  if (current_mA > CURRENT_DEAD_ZONE_MA) {
    return "DISCHARGING";
  }

  if (current_mA < -CURRENT_DEAD_ZONE_MA) {
    return "CHARGING";
  }

  return "IDLE";
}

// =======================
// 打印校准说明
// =======================
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

  // INA219 总线电压是 IN- 对 GND 的电压
  // 电池侧真实电压约等于 busVoltage + shuntVoltage
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