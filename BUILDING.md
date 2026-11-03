# Building HarissaDC

HarissaDC is built as an Xbox 360 executable (`.xex`).

## What you need

- Windows with **Visual Studio 2010** and the **Xbox 360 SDK** (XDK 2.0.21076 or close)
  installed, its Visual Studio integration included.
- **Python 3**, only if you change the interface texts.

## Build

1. Open `src/platform/xbox360/harissadc.sln` in Visual Studio 2010.
2. Choose the **Release** configuration and the **Xbox 360** platform.
3. Build the solution. The result is `src/platform/xbox360/Release/harissadc.xex`.

From a command prompt, the same build:

```
MSBuild.exe src\platform\xbox360\harissadc.sln /p:Configuration=Release "/p:Platform=Xbox 360"
```

Rename `harissadc.xex` to `default.xex` and put it in the `HarissaDC` folder on the console
(see the README for the folder layout).

## Interface texts

The menu texts of every language are in `src/platform/xbox360/lang/*.txt`. After a change,
generate `src/platform/xbox360/xb_lang_table.h` again:

```
python src/platform/xbox360/tools/gen_lang.py
```

## Third-party code

The libraries in `src/third_party/` (BearSSL, zlib, LZMA SDK, SoundTouch) and the fonts in
`third_party/` keep their own licenses: see `THIRD_PARTY_NOTICES.txt` and the license files
next to them.
