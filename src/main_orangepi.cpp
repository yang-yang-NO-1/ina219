#include <wiringPi.h>
#include <wiringPiI2C.h>

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <string.h>

// ======================================================
// Orange Pi 5 Ultra + INA219 Battery Monitor
// I2C Bus : /dev/i2c-2
// I2C Addr: 0x40
// Battery : 2S 18650
// Shunt   : R100 = 0.1 ohm
// ======================================================

// =======================
// I2C 设置
// =======================
#define I2C_DEV "/dev/i2c-2"
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
// INA219 配置
// =======================
// 0x399F 含义大致为：
// - Bus Voltage Range: 32V
// - PGA: +/- 320mV
// - Bus ADC: 12-bit
// - Shunt ADC: 12-bit
// - Continuous shunt and bus mode
#define INA219_CONFIG_VALUE 0x399F

// =======================
// R100 = 0.1Ω 分流电阻校准
// =======================
#define SHUNT_OHMS 0.1f

// Current_LSB = 0.1mA = 0.0001A
#define CURRENT_LSB_A 0.0001f

// Power_LSB = 20 * Current_LSB
#define POWER_LSB_W (20.0f * CURRENT_LSB_A)

// CAL = 0.04096 / (Current_LSB * Rshunt)
//     = 0.04096 / (0.0001 * 0.1)
//     = 4096
#define INA219_CALIBRATION_VALUE 4096

// =======================
// 修正参数
// =======================
#define CURRENT_DIRECTION 1.0f
#define CURRENT_ZERO_OFFSET_MA 0.0f
#define CURRENT_GAIN 1.0f
#define VOLTAGE_GAIN 1.0f

// 小电流死区，避免待机时正负几 mA 抖动
#define CURRENT_DEAD_ZONE_MA 30.0f

// 2S 锂电池低压保护粗略阈值
#define BATTERY_LOW_VOLTAGE 6.4f

// =======================
// 全局 fd
// =======================
static int fd = -1;

// ======================================================
// 工具函数：大小端转换
// INA219 寄存器是高字节在前。
// wiringPiI2CReadReg16 读出来通常需要交换字节。
// ======================================================
static uint16_t swap16(uint16_t value) {
    return (uint16_t)((value >> 8) | (value << 8));
}

// ======================================================
// 安全写寄存器
// 成功返回 0，失败返回 -1
// ======================================================
int ina219WriteRegisterSafe(uint8_t reg, uint16_t value) {
    if (fd < 0) {
        return -1;
    }

    uint16_t swapped = swap16(value);
    int ret = wiringPiI2CWriteReg16(fd, reg, swapped);

    if (ret < 0) {
        return -1;
    }

    return 0;
}

// ======================================================
// 安全读寄存器
// 成功返回 0，失败返回 -1
// ======================================================
int ina219ReadRegisterSafe(uint8_t reg, uint16_t *out) {
    if (fd < 0 || out == NULL) {
        return -1;
    }

    int value = wiringPiI2CReadReg16(fd, reg);

    if (value < 0) {
        return -1;
    }

    uint16_t raw = (uint16_t)value;
    *out = swap16(raw);

    return 0;
}

// ======================================================
// 初始化 I2C
// 明确打开 /dev/i2c-2 上的 0x40 地址
// ======================================================
int ina219Open() {
    fd = wiringPiI2CSetupInterface(I2C_DEV, INA219_ADDR);

    if (fd < 0) {
        printf("ERROR: Cannot open %s addr 0x%02X\n", I2C_DEV, INA219_ADDR);
        printf("errno: %d, %s\n", errno, strerror(errno));
        return -1;
    }

    return 0;
}

// ======================================================
// 检测 INA219 是否真的在线
// 注意：不能只判断 fd >= 0
// 必须尝试读取寄存器，并排除 0xFFFF / 0x0000 这类异常值
// ======================================================
int ina219Detect() {
    uint16_t config = 0;

    if (fd < 0) {
        return 0;
    }

    if (ina219ReadRegisterSafe(INA219_REG_CONFIG, &config) < 0) {
        return 0;
    }

    // 未接设备、总线异常、读失败后经常会得到 0xFFFF
    if (config == 0xFFFF) {
        return 0;
    }

    // 0x0000 也不符合正常 INA219 上电配置
    if (config == 0x0000) {
        return 0;
    }

    return 1;
}

// ======================================================
// 初始化 INA219
// ======================================================
int ina219Init() {
    if (ina219WriteRegisterSafe(INA219_REG_CONFIG, INA219_CONFIG_VALUE) < 0) {
        return -1;
    }

    usleep(10000);

    if (ina219WriteRegisterSafe(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE) < 0) {
        return -1;
    }

    usleep(10000);

    return 0;
}

// ======================================================
// 读取分流电压，单位 mV
// INA219 Shunt Voltage LSB = 10uV = 0.01mV
// ======================================================
int ina219GetShuntVoltage_mV(float *out_mV) {
    uint16_t raw_u16 = 0;

    if (out_mV == NULL) {
        return -1;
    }

    if (ina219ReadRegisterSafe(INA219_REG_SHUNT_VOLTAGE, &raw_u16) < 0) {
        return -1;
    }

    if (raw_u16 == 0xFFFF) {
        return -1;
    }

    int16_t raw = (int16_t)raw_u16;
    *out_mV = raw * 0.01f;

    return 0;
}

// ======================================================
// 读取总线电压，单位 V
// Bus Voltage 寄存器：右移 3 位后，LSB = 4mV
// ======================================================
int ina219GetBusVoltage_V(float *out_V) {
    uint16_t raw = 0;

    if (out_V == NULL) {
        return -1;
    }

    if (ina219ReadRegisterSafe(INA219_REG_BUS_VOLTAGE, &raw) < 0) {
        return -1;
    }

    // 如果没接设备，常见错误值是 0xFFFF
    if (raw == 0xFFFF) {
        return -1;
    }

    raw = raw >> 3;

    // 0xFFFF >> 3 会变成 8191，对应 32.764V，这是典型假数据
    if (raw >= 8191) {
        return -1;
    }

    *out_V = raw * 0.004f;

    return 0;
}

// ======================================================
// 读取电流寄存器，单位 mA
// ======================================================
int ina219GetCurrent_mA(float *out_mA) {
    uint16_t raw_u16 = 0;

    if (out_mA == NULL) {
        return -1;
    }

    // INA219 的 Current / Power 寄存器依赖 Calibration
    if (ina219WriteRegisterSafe(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE) < 0) {
        return -1;
    }

    if (ina219ReadRegisterSafe(INA219_REG_CURRENT, &raw_u16) < 0) {
        return -1;
    }

    if (raw_u16 == 0xFFFF) {
        return -1;
    }

    int16_t raw = (int16_t)raw_u16;

    float current_A = raw * CURRENT_LSB_A;
    float current_mA = current_A * 1000.0f;

    current_mA = current_mA * CURRENT_DIRECTION;
    current_mA = current_mA - CURRENT_ZERO_OFFSET_MA;
    current_mA = current_mA * CURRENT_GAIN;

    *out_mA = current_mA;

    return 0;
}

// ======================================================
// 读取功率寄存器，单位 mW
// ======================================================
int ina219GetPower_mW(float *out_mW) {
    uint16_t raw = 0;

    if (out_mW == NULL) {
        return -1;
    }

    if (ina219WriteRegisterSafe(INA219_REG_CALIBRATION, INA219_CALIBRATION_VALUE) < 0) {
        return -1;
    }

    if (ina219ReadRegisterSafe(INA219_REG_POWER, &raw) < 0) {
        return -1;
    }

    if (raw == 0xFFFF) {
        return -1;
    }

    float power_W = raw * POWER_LSB_W;
    *out_mW = power_W * 1000.0f;

    return 0;
}

// ======================================================
// 通过分流电压手动计算电流
// 单位：mA
// ======================================================
float calcCurrentFromShunt_mA(float shuntVoltage_mV) {
    float shuntVoltage_V = shuntVoltage_mV / 1000.0f;
    float current_A = shuntVoltage_V / SHUNT_OHMS;
    return current_A * 1000.0f;
}

// ======================================================
// 2S 锂电池电量粗略估算
// 注意：这个只适合无充电器干扰、静置或轻载时粗略判断。
// 充电中/大电流放电中，电压法 SOC 会偏。
// ======================================================
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

// ======================================================
// 判断充电 / 放电 / 待机
// 你当前方向：
// current_mA > 30 认为 DISCHARGING
// current_mA < -30 认为 CHARGING
// 如果实际方向相反，改 CURRENT_DIRECTION 为 -1.0f
// ======================================================
const char* getPowerState(float current_mA) {
    if (current_mA > CURRENT_DEAD_ZONE_MA) {
        return "DISCHARGING";
    }

    if (current_mA < -CURRENT_DEAD_ZONE_MA) {
        return "CHARGING";
    }

    return "IDLE";
}

// ======================================================
// 打印一次寄存器，用于调试
// ======================================================
void printDebugRegisters() {
    uint16_t config = 0;
    uint16_t calib = 0;

    if (ina219ReadRegisterSafe(INA219_REG_CONFIG, &config) == 0) {
        printf("Config Register   : 0x%04X\n", config);
    } else {
        printf("Config Register   : READ FAILED\n");
    }

    if (ina219ReadRegisterSafe(INA219_REG_CALIBRATION, &calib) == 0) {
        printf("Calibration Reg   : 0x%04X\n", calib);
    } else {
        printf("Calibration Reg   : READ FAILED\n");
    }
}

// ======================================================
// 主函数
// ======================================================
int main() {
    printf("Orange Pi 5 Ultra + INA219 Battery Monitor\n");
    printf("I2C Device : %s\n", I2C_DEV);
    printf("I2C Address: 0x%02X\n", INA219_ADDR);
    printf("Battery    : 2S 18650, max 8.4V\n");
    printf("Mode       : R100 calibration, shunt = %.3f ohm\n", SHUNT_OHMS);
    printf("------------------------------------------------\n");

    wiringPiSetup();

    if (ina219Open() < 0) {
        printf("ERROR: Failed to open INA219.\n");
        printf("Check whether %s exists and whether I2C2 is enabled.\n", I2C_DEV);
        return 1;
    }

    if (!ina219Detect()) {
        printf("ERROR: INA219 not detected on %s address 0x%02X.\n", I2C_DEV, INA219_ADDR);
        printf("Please check:\n");
        printf("1. VCC -> 3.3V\n");
        printf("2. GND -> GND\n");
        printf("3. SDA -> I2C2_SDA_M0\n");
        printf("4. SCL -> I2C2_SCL_M0\n");
        printf("5. Run: sudo i2cdetect -y 2\n");
        printf("Expected address: 0x40\n");
        return 1;
    }

    printf("INA219 detected successfully.\n");

    if (ina219Init() < 0) {
        printf("ERROR: INA219 init failed.\n");
        return 1;
    }

    printDebugRegisters();

    while (1) {
        float busVoltage_V = 0.0f;
        float shuntVoltage_mV = 0.0f;
        float batteryVoltage_V = 0.0f;
        float current_mA = 0.0f;
        float power_mW = 0.0f;
        float currentByShunt_mA = 0.0f;

        int ok = 1;

        if (ina219GetBusVoltage_V(&busVoltage_V) < 0) {
            ok = 0;
        }

        if (ina219GetShuntVoltage_mV(&shuntVoltage_mV) < 0) {
            ok = 0;
        }

        if (ina219GetCurrent_mA(&current_mA) < 0) {
            ok = 0;
        }

        if (ina219GetPower_mW(&power_mW) < 0) {
            ok = 0;
        }

        if (!ok) {
            printf("------------------------------------------------\n");
            printf("ERROR: INA219 read failed.\n");
            printf("Possible reasons:\n");
            printf("1. INA219 is not connected\n");
            printf("2. Wrong I2C bus, current bus is %s\n", I2C_DEV);
            printf("3. Wrong address, current address is 0x%02X\n", INA219_ADDR);
            printf("4. SDA/SCL reversed\n");
            printf("5. No common GND\n");
            printf("6. Kernel driver conflict, check i2cdetect output for UU\n");
            sleep(1);
            continue;
        }

        busVoltage_V = busVoltage_V * VOLTAGE_GAIN;

        // INA219 的 bus voltage 是 IN- 到 GND 的电压。
        // batteryVoltage = busVoltage + shuntVoltage
        batteryVoltage_V = busVoltage_V + (shuntVoltage_mV / 1000.0f);

        currentByShunt_mA = calcCurrentFromShunt_mA(shuntVoltage_mV);

        int soc = estimateBatteryPercent_2S(batteryVoltage_V);
        const char* state = getPowerState(current_mA);

        printf("------------------------------------------------\n");
        printf("Bus Voltage      : %.3f V\n", busVoltage_V);
        printf("Battery Voltage  : %.3f V\n", batteryVoltage_V);
        printf("Shunt Voltage    : %.3f mV\n", shuntVoltage_mV);
        printf("Current Register : %.2f mA\n", current_mA);
        printf("Current By Shunt : %.2f mA\n", currentByShunt_mA);
        printf("Power            : %.2f mW\n", power_mW);
        printf("Battery SOC      : %d %%\n", soc);
        printf("Power State      : %s\n", state);

        if (batteryVoltage_V <= BATTERY_LOW_VOLTAGE) {
            printf("WARNING: Battery low! Suggested stop discharge.\n");
        }

        sleep(1);
    }

    return 0;
}