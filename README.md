# ESP32 Wireless Thermal Tire Temperature System

A DIY motorsport tire temperature monitoring system using ESP32 microcontrollers, MLX90640 thermal cameras, and ESP-NOW wireless telemetry.

This project provides real-time multi-zone tread temperature analysis for track cars, race cars, and high-performance driving applications without requiring contact probes or expensive commercial equipment.

Full how-to walkthrough: https://youtu.be/BBLoeDo5oM8

---

# Features

* Real-time thermal tire monitoring
* MLX90640 thermal camera integration
* Wireless ESP-NOW telemetry
* Battery-powered remote sensor nodes
* Live browser-based calibration UI
* Automatic tire region detection
* Ellipse tire masking
* 5-zone tread temperature analysis
* Right-side tire zone mirroring
* EMA smoothing/filtering
* Brake hotspot rejection
* Low-latency communication
* Lightweight ESP32 firmware
* Designed for motorsport environments

---

# System Overview

Each wheel module contains:

* ESP32
* MLX90640 thermal camera
* Battery power
* ESP-NOW transmitter

The node continuously:

1. Captures thermal images
2. Detects tire surface temperatures
3. Splits the tread into zones
4. Filters noisy pixels
5. Sends processed temperatures wirelessly

A separate ESP32 receiver/display unit can then:

* Show live tire temperatures
* Display trends
* Trigger alerts
* Log telemetry

---

# Hardware

## Main Components

| Component               | Qty         | Notes                    |
| ----------------------- | ----------- | ------------------------ |
| ESP32 Dev Board         | 1 per wheel | ESP32 lite w/ jst recommended   |
| MLX90640 Thermal Camera | 1 per wheel | 55° or 110° FOV          |
| LiPo Battery            | 1 per wheel          | 2000mah recommended      |
| Power Switch            | 2 per wheel           |                          |
| Enclosure               | 1 per wheel          | Heat resistant preferred PETG or ASA |
| Mounting Hardware       | Optional    | Depends on vehicle       |

---

# Recommended Parts
Disclaimer: Some of the links below are affiliate links, which means I may earn a commission at no extra cost to you.

As an Amazon Associate I earn from qualifying purchases.

1. Esp32 Lites 4 pack  https://www.amazon.com/dp/B0BZP3GXT7?th=1&linkCode=ll2&tag=traviscea05-20&linkId=61597949ec335a3ab2f504bd427caaa9&language=en_US&ref_=as_li_ss_tl
2. 55° Thermal Camera(MLX90640) x4 needed https://www.amazon.com/dp/B07ZKK8QWY?&linkCode=ll2&tag=traviscea05-20&linkId=89015596541410dd8700740aabaff986&language=en_US&ref_=as_li_ss_tl <br>
Other 55° Thermal Camera link if first is sold out https://www.amazon.com/dp/B0F312VWW3?&linkCode=ll2&tag=traviscea05-20&linkId=9bcd7ac8df73fbbeacc3a000f99c4359&language=en_US&ref_=as_li_ss_tl
3. 2000mah battery 4 pack https://www.amazon.com/dp/B0FR9GH966?th=1&linkCode=ll2&tag=traviscea05-20&linkId=ce733239115802f7ae2e96530c1f4ace&language=en_US&ref_=as_li_ss_tl
4. Usb extension cable x4 needed https://www.amazon.com/dp/B08BYRH9DR?th=1&linkCode=ll2&tag=traviscea05-20&linkId=64e65b45900524723d50228c386b2803&language=en_US&ref_=as_li_ss_tl
6. Rocker switches 10pk https://www.amazon.com/dp/B07S2QJKTX?&linkCode=ll2&tag=traviscea05-20&linkId=7190de449726856cee2a4f0f23969745&language=en_US&ref_=as_li_ss_tl

---

# 3d print files

3d print file for mount
https://drive.google.com/file/d/1aso5gM7PM5rLHeJS4tMDzWUVYjjKrLWI/view?usp=drive_link

---

# Wiring

## MLX90640 → ESP32

| MLX90640 | ESP32   |
| -------- | ------- |
| VIN      | 3.3V    |
| GND      | GND     |
| SDA      | GPIO 23 |
| SCL      | GPIO 19 |

---

## Calibration Mode Jumper

| ESP32   | Connection             |
| ------- | ---------------------- |
| GPIO 13 | GND = Calibration Mode |

---

## Battery Monitor

| Circuit                | ESP32   |
| ---------------------- | ------- |
| Voltage Divider Output | GPIO 34 |

---

# Software Features

## Calibration Web Interface

When GPIO13 is grounded during boot:

* ESP32 launches WiFi AP
* Browser UI becomes available
* Live thermal image displayed
* Tire region can be adjusted
* Zone dividers can be dragged
* Calibration saved to onboard flash

### Default WiFi

SSID:

```text
TIRE-CAL-1
```

Password:

```text
12345678
```

---

# Temperature Processing

The firmware performs:

* Ambient temperature detection
* Tire isolation
* Elliptical masking
* Zone segmentation
* Noise filtering
* EMA smoothing
* Hotspot rejection

---

# Tire Zones

The tread is divided into 5 regions:

```text
[ OUTER ] [ MID-OUTER ] [ CENTER ] [ MID-INNER ] [ INNER ]
```

Right-side tires are automatically mirrored.

---

# ESP-NOW Communication

The node transmits:

* Tire ID
* Battery %
* 5 tread temperatures
* Hottest detected temperature

Low latency makes this suitable for:

* Real-time dashboards
* Pit telemetry
* Driver displays

---

# Receiver MAC Address

Update this section in the firmware:

```cpp
uint8_t receiverAddress[] = {
    0x11,
    0x53,
    0xCB,
    0x8F,
    0x89,
    0x49
};
```

Replace with your receiver ESP32 MAC.

---

# Tire IDs

Configure each node:

```cpp
#define TIRE_ID 0
```

| ID | Wheel       |
| -- | ----------- |
| 0  | Front Left  |
| 1  | Front Right |
| 2  | Rear Left   |
| 3  | Rear Right  |

---

# Arduino IDE Setup

## Required Libraries

Install:

* Adafruit MLX90640
* ESPAsyncWebServer
* AsyncTCP

---

# Build Settings

Recommended:

| Setting          | Value   |
| ---------------- | ------- |
| CPU Frequency    | 80MHz   |
| Flash Mode       | QIO     |
| Partition Scheme | Default |
| Upload Speed     | 921600  |

---

# Power Consumption

Optimizations include:

* 80MHz CPU operation
* ESP-NOW instead of WiFi runtime
* Minimal display overhead
* Frame filtering

Typical runtime depends on:

* Battery size
* Refresh rate
* ESP32 variant

---

# Mounting Recommendations

## Best Results

Mount the sensor:

* Facing tread surface
* Slightly downward angled
* Away from direct brake rotor view
* Protected from debris/water
* Rigidly mounted

---

# Suggested Placement

Typical mounting locations:

* Fender liner
* Suspension upright
* Chassis-mounted bracket
* Wheel well liner

---

# Thermal Considerations

Avoid:

* Rotor visibility
* Exhaust heat
* Direct sunlight exposure into lens
* Vibration-induced movement

---

# Known Limitations

* Surface temperature only
* Weather affects readings
* Dirt/water can impact accuracy
* Extreme brake glow may influence data

---

# Future Improvements

Planned ideas:

* SD card logging
* CAN bus integration
* OLED display support
* MQTT/WiFi telemetry
* Lap correlation
* Tire pressure estimation
* Multi-camera fusion
* Cloud telemetry dashboard

---

# Example Applications

* Track day analysis
* Tire pressure tuning
* Camber tuning
* Suspension setup
* Drift tire monitoring
* Endurance racing telemetry
* Autocross optimization

---

# Safety Notice

This project is experimental motorsport hardware.

Do NOT rely on it for:

* Safety-critical systems
* Autonomous control
* Road safety decisions

Always secure wiring and batteries properly in vehicle environments.

---

# License

MIT License

Feel free to modify, improve, and contribute, but never use to sell and licensed as such.

---

# Credits

Built using:

* ESP32
* MLX90640
* ESP-NOW
* ESPAsyncWebServer
* Adafruit MLX90640 Library

# Example Output

```text
FL:
185F 192F 198F 201F 195F

FR:
181F 188F 194F 196F 191F
```

---

# Author

Travis Way

DIY motorsport telemetry and embedded systems development.
