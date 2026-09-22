# M0 end-to-end test

This test checks the first complete FreeCam path:

```text
iPhone camera
-> H.264
-> Wi-Fi
-> Windows receiver
-> H.264 decode
-> preview window
```

## 1. Build the Windows receiver

Requirements:

- Windows 10 22H2 x64 or Windows 11 x64
- Visual Studio 2022
- Desktop development with C++ workload
- CMake 3.20 or newer

From a Developer PowerShell:

```powershell
cd windows
cmake -S . -B build -A x64
cmake --build build --config Release
```

Run:

```powershell
.\build\Release\FreeCamReceiver.exe
```

Expected startup output:

```text
Listening for video on UDP port 47821
FreeCam Receiver
Control: TCP 47820
Video:   UDP 47821
Preview window is ready.
```

A black **FreeCam Preview** window should open.

## 2. Find the PC IPv4 address

In PowerShell:

```powershell
ipconfig
```

Use the IPv4 address for the network that the iPhone is also connected to.

Example only:

```text
192.168.1.123
```

Do not use the example address unless it is actually your PC address.

## 3. Start FreeCamIP on the iPhone

The iPhone and PC must be on the same local network.

In FreeCamIP:

1. Allow camera access.
2. Allow local network access.
3. Enter the PC IPv4 address.
4. Tap **Connect**.
5. Confirm the status changes to **Connected**.
6. Tap **Start Camera**.

## 4. Expected Windows output

The console should show a control handshake:

```text
Client connected from ...
CONTROL <= {"type":"hello",...}
CONTROL => {"type":"hello_ack","protocol":0,"videoPort":47821}
```

After video starts, it should periodically show received frames:

```text
VIDEO <= frame=... bytes=... keyframe=... total=...
```

The first decodable keyframe should cause the camera image to appear in the preview window.

## Firewall

On the first run, Windows Defender Firewall can ask whether FreeCam Receiver may accept network traffic.

Allow it on **Private networks**.

FreeCam currently uses:

- TCP 47820 for control
- UDP 47821 for video

## If connection works but video does not appear

Record:

- the Windows console output
- the FreeCamIP status text
- Sent frames
- Sent datagrams
- Send errors

These values are enough to separate a transport problem from a decode/render problem.

## Current limitations

This is an M0 development build.

- manual PC IP entry
- Wi-Fi only
- 720p30
- rear wide camera
- no audio
- no virtual webcam yet
- CPU NV12-to-BGRA conversion for preview
