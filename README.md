# Red Viter

Virtual Boy emulator for the PS Vita, based on Red Viper.

## Build

Requirements: VitaSDK, vitaGL, vitaShaRK, mathneon, zlib.

    mkdir build && cd build
    cmake ..
    make

## Setup

- Install the VPK.
- Put ROMs (`.vb` or `.zip`) in `ux0:/vb/`.
- Data (config, saves, states) lives in `ux0:data/red-viter/`. Shader or audio errors are written to `ux0:data/red-viter/log.txt`.
