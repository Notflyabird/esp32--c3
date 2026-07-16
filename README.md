# ESP32-S3 Multilingual Voice RGB Light

Pure ESP-IDF 5.3.5 project for an ESP32-S3 N16R8 board with an INMP441 microphone.

## Hardware

- INMP441 BCLK: GPIO4
- INMP441 WS: GPIO5
- INMP441 SD: GPIO6
- Onboard addressable RGB LED: GPIO48

## Voice Commands

Say `Hi ESP`, then use one of these commands within six seconds:

- `da kai dian deng` / turn on the light
- `guan bi dian deng` / turn off the light

## Build And Flash

Open an ESP-IDF 5.3.5 terminal and run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

The project uses the ESP-IDF Component Manager only to resolve ESP-SR. PlatformIO
is not required.
