# RFID Attendance System - Reader Prototype

Advanced Arduino sketch for **ICCT Smart Attendance System** using MFRC522 RFID reader, MQTT communication, I2C LCD, and RTC clock synchronization.

## **Key Features**
- **Dual Mode**: Attendance tracking + Admin card registration mode
- **MQTT Integration**: Real-time communication with web application
- **NTP Sync**: Automatic time sync from internet
- **Smart Feedback**: LCD shows "Recorded!", "Card Denied", "Not on schedule", etc.
- **Error Handling**: 5-second lockout after errors + debounce protection
- **LED Indicators**: WiFi (pin 8), RFID (pin 7), Registration (pin 6)

## **Hardware Connections**

| Component | Pin | Arduino Pin |
|-----------|-----|-------------|
| MFRC522 RFID | RST | 9 |
| MFRC522 RFID | SS | 10 |
| I2C LCD | SDA | A4 |
| I2C LCD | SCL | A5 |
| LCD Address | - | `0x27` |
| WiFi LED | - | 8 |
| RFID LED | - | 7 |
| Reg. LED | - | 6 |
| Buzzer | - | 4 |

## **Required Libraries**
Install via **Arduino IDE > Library Manager**:

