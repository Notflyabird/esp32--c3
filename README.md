# ESP32-S3 ESP-SR Dou Dizhu Scorekeeper

Pure ESP-IDF 5.3.5 project for an ESP32-S3 N16R8 board with a single INMP441 microphone.

## Hardware

- INMP441 BCLK: GPIO4
- INMP441 WS: GPIO5
- INMP441 SD: GPIO6
- INMP441 L/R: GND

No speaker, TTS, NVS storage, display, or LED output is used. All score state is printed through serial logs.

## Voice Commands

Say `Hi ESP`, then use one command within six seconds.

Scoring command format:

```text
<player> di zhu <result> <points> fen
```

Players:

- `yi hao`
- `er hao`
- `san hao`

Results:

- `ying`
- `shu`

Supported point phrases:

- `liang fen`
- `si fen`
- `liu fen`
- `ba fen`
- `yi shi fen`
- `yi shi er fen`
- `yi shi si fen`
- `yi shi liu fen`
- `yi shi ba fen`
- `er shi fen`

Examples:

- `yi hao di zhu ying liang fen`
- `er hao di zhu shu ba fen`
- `san hao di zhu ying er shi fen`

Other commands:

- `cha xun fen shu`
- `chong zhi suo you fen shu`

## Scoring

If player X landlord wins N points:

- Player X: `+N`
- Other two players: `-N/2` each

If player X landlord loses N points:

- Player X: `-N`
- Other two players: `+N/2` each

Supported N values are 2, 4, 6, 8, 10, 12, 14, 16, 18, and 20.

## Build And Flash

Open an ESP-IDF 5.3.5 terminal and run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```
