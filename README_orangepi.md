# Orange Pi 5 + INA219 Battery Monitor

## 项目简介
本项目旨在使用 Orange Pi 5 和 INA219 模块实现电池监控功能。程序通过 I2C 通信读取电池的电压、电流和功率，并估算电池的剩余电量（SOC）。

## 硬件要求
1. Orange Pi 5
2. INA219 电流/电压传感器模块
3. 2S 锂电池（如 18650 电池组）
4. 连接线（杜邦线）

## 软件要求
1. Orange Pi 5 的操作系统（如 Ubuntu 或 Debian）
2. `wiringPi` 库

## 硬件连接
| Orange Pi 5 引脚 | INA219 引脚 |
|------------------|-------------|
| 3.3V 或 5V       | VCC         |
| GND              | GND         |
| SDA（默认引脚 3）| SDA         |
| SCL（默认引脚 5）| SCL         |

## 软件安装
1. **安装 `wiringPi` 库**：
   ```bash
   sudo apt update
   sudo apt install wiringpi
   ```

2. **验证 `wiringPi` 是否安装成功**：
   ```bash
   gpio -v
   ```
   如果安装成功，会显示 `wiringPi` 的版本信息。

3. **编译程序**：
   将 `main_orangepi.cpp` 文件保存到项目目录下，然后运行以下命令：
   ```bash
   g++ -o ina219_monitor main_orangepi.cpp -lwiringPi -lm
   ```

4. **运行程序**：
   ```bash
   sudo ./ina219_monitor
   ```

## 程序功能
1. **实时监控**：
   - 总线电压（Bus Voltage）
   - 电池电压（Battery Voltage）
   - 分流电压（Shunt Voltage）
   - 电流（Current）
   - 功率（Power）
   - 电池剩余电量（SOC）
   - 电池状态（充电/放电/待机）

2. **低电压警告**：
   当电池电压低于 6.4V 时，程序会提示用户停止放电。

## 注意事项
1. 确保 INA219 模块的 I2C 地址为 `0x40`，如果地址不同，请修改代码中的 `INA219_ADDR`。
2. 如果程序无法检测到 INA219，请检查接线是否正确，特别是 SDA 和 SCL 引脚。
3. 电池电压估算仅供参考，带负载时电压会有所下降。

## 许可证
本项目遵循 MIT 许可证，详情请参阅 LICENSE 文件。