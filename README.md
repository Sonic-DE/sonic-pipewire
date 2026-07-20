# KPipeWire

A set of convenient classes to use PipeWire (https://pipewire.org/) in Qt projects, developed in C++ and mainly targeted for use in QML components.

This SonicDE build targets X11 through Qt's `xcb` platform plugin. Screen capture is acquired through the standard XDG Desktop Portal ScreenCast API; the companion `xdg-desktop-portal-sonicde` backend is responsible for XCB capture and PipeWire producer nodes, while this repository consumes, previews, and encodes the returned PipeWire streams.

## Features

At the moment KPipeWire offers two main components:
* **KPipeWire:** connect to and render a PipeWire video stream into your app.
* **KPipeWireRecord:** using FFmpeg, records a PipeWire video stream into a file.

## Runtime Requirements

For SonicDE screen capture and preview, the runtime environment must provide:

* Qt with the `xcb` platform plugin available and selected before application startup, for example `QT_QPA_PLATFORM=xcb`.
* Qt Quick Controls 2 for the `xdp-recordme` preview utility.
* An X11 session.
* PipeWire.
* `xdg-desktop-portal`.
* `xdg-desktop-portal-sonicde`, selected for the SonicDE desktop portal implementation.

## Portal Capture Model

Applications request streams from XDG Desktop Portal ScreenCast. The portal returns a restricted PipeWire remote file descriptor and one or more stream node IDs. KPipeWire then opens those nodes with `PipeWireSourceStream`, renders them with `PipeWireSourceItem`, or encodes them with `PipeWireEncodedStream`.

The current SonicDE portal backend supplies shared-memory `SPA_DATA_MemPtr` frames. KPipeWire still contains separate support for `SPA_DATA_MemFd` and DMA-BUF-capable producers, but SonicDE's XCB capture path does not exercise DMA-BUF. DMA-BUF import is available only when a producer advertises it and the Qt/xcb session exposes a valid EGL display with usable modifiers; otherwise shared memory is the expected path.

SonicDE advertises cursor mode `Hidden`, and conditionally `Embedded` when XFixes support is available. It does not provide cursor metadata, so clients should not request Metadata cursor mode from SonicDE unless a future backend advertises it.

## Examples

Preview a portal ScreenCast stream with the Qt Quick test utility:

```sh
QT_QPA_PLATFORM=xcb xdp-recordme --duration 5000
```

Run the portal-only headless utility and encode the returned stream:

```sh
QT_QPA_PLATFORM=xcb kpipewireheadlesstest --xdp-screencast --encoded --duration 5000
```

The headless utility also supports the XDG RemoteDesktop acquisition path when input/session testing is required:

```sh
QT_QPA_PLATFORM=xcb kpipewireheadlesstest --xdp-remotedesktop --encoded --duration 5000
```

## Licence

This project is licenced under LGPL v2.1+. You can find all the information under `LICENSES/`.
