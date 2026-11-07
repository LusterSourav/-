# PowerMesh PINOUT — ESP32-S3 DevKitC-1 (matches sketch.ino + diagram.json)

| Net | ESP32-S3 Pin | Type | Connected To | Notes |
|---|---|---|---|---|
| **OLED SDA** | **8** | I2C | SSD1306 SDA | `Wire.begin(8,9)` |
| **OLED SCL** | **9** | I2C | SSD1306 SCL | `0x3C` default |
| **Battery ADC** | **1** | ADC1_CH0 | Pot SIG → 3S divider 9.0-12.6V | `analogRead(1)` 12-bit |
| **Beacon LED** | **2** | OUT | 3W LED via driver | `LEDC` blink 1Hz |
| **USB 5V LED** | **5** | OUT | Power bank indicator | HIGH = active |
| **RF TX LED** | **6** | OUT | RF activity | pulse 60ms on TX |
| **SOS Button** | **10** | IN_PULLUP | Pushbutton → GND | active LOW |
| **SD MISO** | **12** | SPI | MicroSD DO | `SPI.begin(14,12,13,15)` |
| **SD MOSI** | **13** | SPI | MicroSD DI | |
| **SD SCK** | **14** | SPI | MicroSD SCK | |
| **SD CS** | **15** | SPI | MicroSD CS | 8MHz |
| **RF TX** | **17** | UART TX | SA618 RXD | `Serial1 TX` |
| **RF RX** | **18** | UART RX | SA618 TXD | `Serial1 RX` 115200 8N1 |
| **RF AUX** | **35** | IN | SA618 AUX | `1`=ready |
| **RF M0** | **40** | OUT | SA618 M0 | `L`=normal |
| **RF M1** | **41** | OUT | SA618 M1 | `L`=normal |
| **OLED/SD VCC** | **3V3.1** | PWR | 3.3V rail | XL4015 3.3V |
| **GND** | **GND.1** | PWR | Common GND | BMS + buck |
| **USB/UART** | **TX/RX** | UART0 | `$serialMonitor` | 115200 |
| **Dual Node B** | **45/46** | OUT | Second node M0/M1 | only diagram-dual.json |

- Valid S3 pins: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,35-42,45-48` — all used above are valid per `wokwi-cli lint`.
- Single-node `diagram.json` uses `RF AUX 35 M0 40 M1 41` — dual `diagram-dual.json` mirrors with second node on `45/46` to avoid clash.
- Sketch `sketch.ino` is Wokwi-cloud fix (322 lines, `WebServer`); `sketch-node.ino` adds `-DNODE_ID=1/2` → `PM-01` @ `192.168.4.1` vs `PM-02` @ `192.168.5.1`.
