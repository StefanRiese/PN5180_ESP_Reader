[![CC BY-NC-SA 4.0][cc-by-nc-sa-shield]][cc-by-nc-sa]

This work is licensed under a
[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License][cc-by-nc-sa].

[![CC BY-NC-SA 4.0][cc-by-nc-sa-image]][cc-by-nc-sa]

[cc-by-nc-sa]: http://creativecommons.org/licenses/by-nc-sa/4.0/
[cc-by-nc-sa-image]: https://licensebuttons.net/l/by-nc-sa/4.0/88x31.png
[cc-by-nc-sa-shield]: https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg


## Donations / Spenden
If somebody wants to support me for upcoming projects :)  
- PayPal:  [![donate](https://www.paypalobjects.com/de_DE/DE/i/btn/btn_donate_LG.gif)](https://www.paypal.com/donate/?hosted_button_id=T25NKW8BXJ7J8)
- Amazon Giftcard: https://www.amazon.de/Amazon-Gutschein-per-E-Mail-Amazon/dp/B0054PDOV8 - stefan.riese@me.com

# PN5180 ESP reader
This reader uses a PN5180 RFID reader and a Wemos D1 mini to read RFID tags like ISO 15693.

<img src="./Images/IMG_4033.JPEG" width="50%" height="50%">
<img src="./Images/IMG_4035.JPEG" width="50%" height="50%">
<img src="./Images/IMG_4034.JPEG" width="50%" height="50%">

# Used hardware
- 1 x PN5180 reader (https://www.amazon.de/dp/B0BFGWS7MZ)
- 1 x Wemos D1 mini (https://www.amazon.de/AZDelivery-NodeMCU-ESP8266EX-kompatibel-inklusive/dp/B08BTYHJM1)
- 1 x 10x3mm magnet (https://www.amazon.de/Temporaryt-Neodym-Magnete-Aufbewahrungs-K%C3%BChlschrank/dp/B07DNBXBWQ)
- 4 x M3x6 screw

# Wiring (Wemos D1 mini / ESP8266)
| PN5180 pin | Wemos D1 mini pin | GPIO |
|---|---|---|
| NSS  | D2 | GPIO4  |
| BUSY | D0 | GPIO16 |
| RST  | D1 | GPIO5  |
| MOSI | D7 | GPIO13 |
| MISO | D6 | GPIO12 |
| SCK  | D5 | GPIO14 |
| VCC  | 3V3/5V (per module spec) | - |
| GND  | GND | - |

A WS2812B (NeoPixel) status LED is wired to **D8** (GPIO15) and gives feedback:
green = success, red = failure, white = boot.

# Setup instructions

1. **Install the Arduino IDE** (1.8.x or 2.x) from https://www.arduino.cc/en/software.
2. **Add ESP8266 board support**: In the Arduino IDE, go to *File > Preferences* and add
   `http://arduino.esp8266.com/stable/package_esp8266com_index.json` to
   *Additional Boards Manager URLs*. Then open *Tools > Board > Boards Manager*,
   search for `esp8266` and install it.
3. **Select the board**: *Tools > Board > ESP8266 Boards > LOLIN(WEMOS) D1 R2 & mini*.
4. **Install required libraries** via *Sketch > Include Library > Manage Libraries*:
   - `PN5180-Library` (by Andreas Trappmann) — provides `PN5180ISO15693.h`
   - `Adafruit NeoPixel`
5. **Wire the hardware** according to the table above.
6. **Open the sketch**: `Firmware/Pn5180Esp/Pn5180Esp.ino`.
7. **Select the correct serial port** under *Tools > Port*, then click **Upload**.
8. **Open the Serial Monitor** (baud rate `115200`) to see boot logs and interact
   with the reader using the following single-character commands:
   - `v` — print firmware/sketch version
   - `i` — read the inventory (UID) of a nearby ISO 15693 tag
   - `u` — attempt to disable privacy mode (unlock) on a nearby tag

On boot, the sketch resets the PN5180, prints its product/firmware/EEPROM
versions, and enables the RF field. If it reports `Initialization failed!?`,
double-check the wiring (especially NSS/BUSY/RST and power) and press the
reset button.
