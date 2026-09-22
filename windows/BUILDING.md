# Building the Windows receiver

## Requirements

- Windows 10 22H2 or Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.20 or newer

## Configure

From a Developer PowerShell:

```powershell
cd windows
cmake -S . -B build -A x64
```

## Build

```powershell
cmake --build build --config Debug
```

The executable is normally created at:

```text
windows\build\Debug\FreeCamReceiver.exe
```

## First protocol test

Start the receiver. It listens on TCP port 47820.

From another local program, send one newline-terminated UTF-8 message:

```json
{"type":"hello","protocol":0,"deviceName":"Test iPhone"}
```

The receiver should reply:

```json
{"type":"hello_ack","protocol":0,"videoPort":47821}
```

This is only the M0 control-channel proof of concept. Video transport is not implemented yet.
