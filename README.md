# ESP32-S3 ESP-SR Dou Dizhu Scorekeeper

Pure ESP-IDF 5.3.5 project for an ESP32-S3 N16R8 board with a single INMP441 microphone.

## Hardware

- INMP441 BCLK: GPIO4
- INMP441 WS: GPIO5
- INMP441 SD: GPIO6
- INMP441 L/R: GND

No speaker, TTS, NVS storage, display, or LED output is used. All score state is printed through serial logs.

## Voice Commands

Say `Hi ESP`, then use one command within six seconds:

- `yi hao di zhu ying liang fen` / 一号地主赢两分
- `er hao di zhu ying liang fen` / 二号地主赢两分
- `san hao di zhu ying liang fen` / 三号地主赢两分
- `yi hao di zhu shu liang fen` / 一号地主输两分
- `er hao di zhu shu liang fen` / 二号地主输两分
- `san hao di zhu shu liang fen` / 三号地主输两分
- `cha xun fen shu` / 查询分数
- `chong zhi suo you fen shu` / 重置所有分数

## Build And Flash

Open an ESP-IDF 5.3.5 terminal and run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```
