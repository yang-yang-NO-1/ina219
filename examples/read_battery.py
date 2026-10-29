#!/usr/bin/env python3
"""Orange Pi 5 Python/Qt/UI example. Standard library only; 127.0.0.1 API."""
import json
from urllib.request import urlopen

URL = "http://127.0.0.1:8765/api/v1/battery"
with urlopen(URL, timeout=2) as response:
    data = json.load(response)

if not data["valid"]:
    print("INA219 read error:", data["error"])
else:
    print("Battery:", data["battery_voltage_v"], "V")
    print("Current:", data["current_a"], "A")
    print("Power:", data["pack_power_w"], "W")
    print("State:", data["state"])

# SOC may be null until a capacity AND a trustworthy initial SOC are configured.
soc = data["soc"]
if soc["available"]:
    print("SOC:", round(soc["soc_percent"], 1), "%")
    print("Remaining:", round(soc["remaining_capacity_mah"]), "mAh")
    if soc["uncertain"] or soc["stale"]:
        print("NOTE: SOC is approximate / may be stale")
else:
    print("SOC: awaiting initial calibration (source:", soc["source"], ")")

alarm = data["alarm"]
if alarm["sensor_fault"]:
    print("WARNING: INA219 measurement unavailable")
elif alarm["active"]:
    print("BATTERY ALARM:", alarm["level"])
