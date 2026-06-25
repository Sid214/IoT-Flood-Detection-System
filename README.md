# 🌊 IoT Advanced Flood Detection System

![ESP8266](https://img.shields.io/badge/ESP8266-NodeMCU-blue)
![C++](https://img.shields.io/badge/Language-C++-00599C)
![License](https://img.shields.io/badge/License-MIT-green)

A robust, real-time IoT flood monitoring system built with the ESP8266 NodeMCU and an HC-SR04 ultrasonic sensor. This project features a modern, interactive web dashboard stored entirely in the ESP8266's flash memory, providing live water level simulations, asynchronous hardware alerts, and device-level audio sirens.

## ✨ Features

* **Real-Time Web Dashboard:** A responsive, glass-morphism UI that visualizes water levels dynamically.
* **Zero-Delay Architecture:** Built with `ESP8266WebServer` and `millis()` for non-blocking execution. The web server and hardware sensors run simultaneously without freezing.
* **3-Tier Alert System:** Intelligently categorizes water levels into SAFE, WARNING, and DANGER zones with distinct visual and audio cues.
* **Robust Sensor Logic (Median Filter):** Includes a custom filtering algorithm that takes multiple rapid readings and discards outliers to prevent false-positive alarms.
* **Dual-Audio Alarms:** Triggers a 4000Hz piercing hardware buzzer locally, while also unlocking the browser's audio API to sound a siren on the viewing device (phone/laptop).
* **Flash Memory Storage:** HTML/CSS/JS is served directly from PROGMEM to prevent RAM overflow and crashing.

## 🛠️ Hardware Requirements

* 1x ESP8266 NodeMCU (ESP-12E)
* 1x HC-SR04 Ultrasonic Sensor
* 1x Passive Piezo Buzzer
* 1x 220Ω Resistor
* Breadboard & Jumper Wires (M-M, M-F)
* Micro-USB Cable (with data transfer capability)

## 🔌 Circuit & Wiring

| HC-SR04 / Component | ESP8266 Pin | Notes |
| :--- | :--- | :--- |
| **VCC** (Sensor) | **Vin / VU** | Powers the sensor with 5V from USB |
| **GND** (Sensor) | **GND** | Ground connection |
| **TRIG** (Sensor) | **D5** | Trigger Pin |
| **ECHO** (Sensor) | **D6** | Echo Pin |
| **Buzzer (+) Long Leg** | **D7** | Connected *through* the 220Ω Resistor |
| **Buzzer (-) Short Leg**| **GND** | Ground connection |

> **Note:** Always use a 220Ω resistor in series with the buzzer to protect the ESP8266 GPIO pins from drawing too much current.

*(Optional: Add a picture of your circuit diagram here by linking to `assets/circuit_diagram.png`)*

## 🚀 Installation & Setup

1. **Install the Arduino IDE** and add the ESP8266 board manager URL to your preferences.
2. **Clone this repository** to your local machine.
3. Open `src/IoT_Flood_Monitor.ino` in the Arduino IDE.
4. **Update Network Credentials:** Change the Wi-Fi settings to match your local network:
   ```cpp
   const char* ssid = "YOUR_WIFI_NAME";
   const char* password = "YOUR_WIFI_PASSWORD";
