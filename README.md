# FreeCam

FreeCam is an open-source project to use an iPhone as a reliable webcam on Windows.

## Goals

- Support Windows 10 and Windows 11.
- Use the iPhone camera over the local network first, with USB support later.
- Provide 720p and 1080p video without watermarks, time limits, accounts, ads, or paid feature gates.
- Keep the connection stable and recover automatically after temporary network loss.
- Expose the stream as a normal webcam to applications such as Discord, Teams, browsers, and OBS.

## First milestone

Version 0.1 proves the core video path:

```text
iPhone camera
    |
AVFoundation
    |
H.264 encode
    |
local network
    |
Windows receiver
    |
H.264 decode
    |
live preview
```

The virtual camera layer comes after the video path is stable.

## Platform plan

### iPhone

- Swift
- AVFoundation for camera capture
- VideoToolbox for hardware H.264 encoding

### Windows

- Windows 10 22H2 x64
- Windows 11 x64
- Native receiver application
- Shared decoding and frame-buffer pipeline
- DirectShow virtual-camera backend for Windows 10 and Windows 11
- Optional Media Foundation virtual-camera backend for Windows 11 later

## Project status

Early development.

See [docs/architecture.md](docs/architecture.md) and [protocol/README.md](protocol/README.md) for the current design.

## License

GPL-3.0. See [LICENSE](LICENSE).
