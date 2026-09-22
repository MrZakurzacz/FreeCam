# Building and testing the Windows receiver

## Requirements

- Windows 10 22H2 or Windows 11
- Visual Studio 2022 with the **Desktop development with C++** workload
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

## Run

Start:

```powershell
.\build\Debug\FreeCamReceiver.exe
```

The receiver listens on:

- TCP 47820: control and handshake
- UDP 47821: H.264 video packets

Windows Firewall can ask for permission the first time the receiver listens on the network. Allow it on your private/home network.

If you configure firewall rules manually, the receiver needs inbound access only to those two ports. FreeCam does not require an internet connection.

## Find the PC IPv4 address

On the Windows PC:

```powershell
ipconfig
```

Use the IPv4 address of the adapter that is on the same local network as the iPhone. A typical home address looks like `192.168.x.x` or `10.x.x.x`.

Enter that address in FreeCamIP on the iPhone.

## Expected M0 output

After **Connect** on the iPhone, the Windows console should show a control connection and the protocol hello message.

After **Start Camera**, the receiver should begin reporting completed H.264 access units. Output is limited to the first few frames and then approximately once per second.

Example shape:

```text
FreeCam Receiver
Listening for video on UDP port 47821
Listening on TCP port 47820
Client connected from 192.168.1.50:...
CONTROL <= {"type":"hello","protocol":0,"deviceName":"iPhone"}
CONTROL => {"type":"hello_ack","protocol":0,"videoPort":47821}
VIDEO frame=0 bytes=... fragments=... keyframe complete=1 dropped=0 invalid=0
VIDEO frame=1 bytes=... fragments=... complete=2 dropped=0 invalid=0
```

At this milestone the Windows app verifies that complete H.264 frames reach the laptop. It does **not** decode or display the video yet. Preview decoding is the next milestone.

## Troubleshooting

If the iPhone cannot connect:

1. Confirm both devices are on the same LAN.
2. Confirm the entered IPv4 address belongs to the Windows PC.
3. Confirm Windows Firewall allows `FreeCamReceiver.exe`, TCP 47820, and UDP 47821 on the private network.
4. Avoid guest Wi-Fi networks that isolate wireless clients from each other.
