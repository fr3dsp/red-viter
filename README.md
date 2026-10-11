# Red Viter

Virtual Boy emulator for the PS Vita, based on Red Viper.

> [!CAUTION]
> This project is being made with AI assistance.

## Build

Requirements: VitaSDK, vitaGL, vitaShaRK, mathneon, zlib.

    mkdir build && cd build
    cmake ..
    make

## Setup

- Install the VPK.
- Put ROMs (`.vb` or `.zip`) in `ux0:data/redviter/roms/`.
- Data (config, saves, states) lives in `ux0:data/redviter/`. Shader or audio errors are written to `ux0:data/redviter/log.txt`.

## Credits
* FreddySP
* Hiro/dotfla_
