# FreeCam Architecture

## Goal

FreeCam turns an iPhone into a reliable webcam for Windows 10 and Windows 11.

The project is local-first:
- no account
- no cloud dependency
- no watermark
- no artificial resolution or time limits

## Initial architecture

```
iPhone
  AVFoundation camera capture
  -> VideoToolbox H.264 encoder
  -> local network transport
  -> Windows receiver
  -> H.264 decoder
  -> preview
  -> virtual camera backend
```

## Platform plan

### iPhone
- Swift
- AVFoundation for camera capture
- VideoToolbox for hardware H.264 encoding

### Windows
- C++20
- Windows 10 22H2 and Windows 11
- receiver and preview shared across both versions
- DirectShow virtual camera backend for compatibility
- optional Windows 11 Media Foundation backend later

## Development milestones

### M0 - transport proof of concept
Show the live iPhone camera feed in a Windows desktop window.

### M1 - reliable local streaming
Add discovery, reconnect, basic statistics, bitrate control, and stable long-running streaming.

### M2 - virtual camera
Expose FreeCam as a webcam device in common Windows applications.

### M3 - USB transport
Add cable-based transport while keeping Wi-Fi support.

### M4 - audio and controls
Add microphone transport, lens selection, focus, exposure, white balance, and rotation controls.

## Compatibility

Initial supported desktop systems:
- Windows 10 22H2 x64
- Windows 11 x64

Initial mobile target:
- iOS 16 or newer

These targets can change as implementation and testing progress.
