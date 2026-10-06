# OBS Audio to WebSocket Plugin

[![Build Status](https://github.com/omnyist/obs-audio-to-websocket/actions/workflows/push.yaml/badge.svg)](https://github.com/omnyist/obs-audio-to-websocket/actions/workflows/push.yaml)

An OBS Studio plugin that streams the audio of one OBS source to a WebSocket server, live. It is built for speech-to-text: the audio is sent as 16 kHz, mono, 16-bit PCM, which is what most speech recognizers want.

The plugin taps the source **after its filters** (noise suppression, compressor, limiter and so on), so the server hears what you set up in OBS, not the raw device.

## At a glance

If you're wiring this into a bot or script, or an assistant is helping you do it, these are the facts that matter:

- **The plugin is the WebSocket client.** Your program runs a WebSocket server on the URL you set in the plugin (default `ws://localhost:8889/audio`). OBS connects to it, not the other way round.
- **Only `ws://` works.** There is no TLS, so `wss://` URLs are rejected.
- **The payload is raw audio.** Every message is binary: 16 kHz, mono, signed 16-bit little-endian PCM. There is no header, and there are no JSON or text messages.
- **It's the processed audio.** The plugin taps the source after its OBS filters, so what your server receives is what you hear after your noise suppression, compressor and so on. Nothing is sent while the source is muted.
- **Google Speech-to-Text can't take a WebSocket directly.** Your server forwards the audio to Google's streaming API as `LINEAR16`, 16000 Hz, 1 channel. See [Using it with Google Speech-to-Text](#using-it-with-google-speech-to-text).
- **Gain defaults to 1x.** Leave it there for audio that already has a limiter or compressor; more gain clips.
- **Auto-connect follows your OBS stream.** It connects when you start streaming and disconnects when you stop. It does not start when OBS launches. Use Start Streaming in the dialog if you want audio flowing while you're offline.

## Features

- Streams any OBS audio source, such as your microphone, to a WebSocket server
- Audio is sent after the source's OBS filters
- 16 kHz mono 16-bit PCM, no header, ready for speech recognizers
- Adjustable gain on the outgoing audio (does not change your stream or recording)
- Reconnects automatically with exponential backoff
- Optional auto-connect whenever you start streaming in OBS
- Live level meter, connection status and data rate in the OBS Tools menu
- Settings dialog in English and Russian (follows the OBS language)

## System Requirements

- OBS Studio 31.0 or newer
- Windows 10/11, macOS 11+ (Apple Silicon and Intel), or Linux

## Installation

Download the build for your platform from the [Releases page](https://github.com/omnyist/obs-audio-to-websocket/releases), then close OBS and install it.

**Windows**
- Run the installer (`...-windows-x64.exe`). This is the easy way.
- Or extract `...-windows-x64-Portable.zip` into your OBS folder (usually `C:\Program Files\obs-studio\`) so that its `obs-plugins` and `data` folders merge with OBS's own. The `locale` files must end up in `data\obs-plugins\obs-audio-to-websocket\`, or the settings window shows raw label names instead of text.

**macOS**
- Run the .pkg, or extract the .tar.xz and copy the `.plugin` bundle to `~/Library/Application Support/obs-studio/plugins/`.
- Releases are not signed or notarized yet. If macOS refuses to open the .pkg, right-click it and choose Open, or allow it under System Settings → Privacy & Security.

**Linux**
- Install the .deb if the release has one, or build from source (below).

Start OBS again. The settings live under **Tools → Audio to WebSocket Settings**.

## Quick Start

1. Start your WebSocket server (see [Writing a server](#writing-a-server)).
2. In OBS, open Tools → Audio to WebSocket Settings.
3. Set the URL, for example `ws://localhost:8889/audio`. Only `ws://` is supported, not `wss://`.
4. Pick your audio source. Microphones are listed first.
5. Leave **Gain** at `1.0x` unless the server needs a louder signal (see [Gain](#gain)).
6. Click Start Streaming. The status line shows when the connection is up.

Auto-connect: with "Auto-connect when streaming starts" checked, the plugin connects when you start streaming in OBS and disconnects when you stop. It does not start on its own when OBS launches. Use the button if you want audio flowing while you are not live.

## Using it with Google Speech-to-Text

Google's streaming API takes a gRPC stream, not a WebSocket, so a small server of your own sits in between: the plugin connects to it, and it forwards the audio to Google. That is your Twitch bot, or a small process beside it.

What the plugin sends matches what Google asks for:

| Google setting | Value |
|---|---|
| Encoding | `LINEAR16` |
| Sample rate | `16000` Hz |
| Channels | 1 (mono) |

Things to know from Google's docs:

- A streaming request is limited to about 5 minutes of audio, so the bot has to open a fresh recognition stream before then and keep feeding it.
- Each message sent to Google can carry at most 25 KB of audio. The plugin's WebSocket messages are far smaller than that (a few hundred bytes to about a kilobyte).
- Audio must arrive at roughly real-time speed, which it does.

See Google's [streaming recognition guide](https://docs.cloud.google.com/speech-to-text/docs/streaming-recognize) for client code in your language.

## Writing a server

The plugin is the WebSocket **client**, so your program has to listen. Each binary message is a slice of raw audio, with no header; there are no text messages to parse.

```javascript
import { WebSocketServer } from 'ws';

const wss = new WebSocketServer({ port: 8889 });

wss.on('connection', (socket) => {
  console.log('OBS connected');

  socket.on('message', (data, isBinary) => {
    if (!isBinary) return;
    // data is a Buffer of 16 kHz, mono, signed 16-bit little-endian PCM.
    // Write it to your speech recognizer's input stream here.
    console.log(`got ${data.length} bytes`);
  });

  socket.on('close', () => console.log('OBS disconnected'));
});
```

The default URL in the plugin is `ws://localhost:8889/audio`. This server accepts any path, so that works as is.

## Gain

The Gain setting multiplies the audio before it is sent. It only affects what goes over the WebSocket, not your stream, recording or monitoring.

- `1.0x` sends the source exactly as it comes out of its filters. Use this when the source already goes through a compressor or limiter, or the level looks healthy in the OBS mixer.
- Raise it only if the recognizer struggles with a quiet source. Anything pushed past full scale is clipped, which makes recognition worse, not better.

## Audio format

What is sent for every audio message:

- 16-bit signed PCM, little-endian
- 16 kHz sample rate
- Mono (all OBS channels are averaged together)
- No header and no control messages, just audio bytes

If the OBS audio sample rate is 16 kHz or lower, the audio is sent at that rate without resampling.

While the source is muted in OBS, nothing is sent.

## Troubleshooting

### Connection
- Make sure the server is running before you click Start Streaming
- Check the URL starts with `ws://` and the port matches your server
- Check firewall settings if the server is on another machine
- The plugin retries with a growing delay, up to 10 attempts, then stops and says so

### Audio
- Check the source is not muted in OBS
- Check the level meter in the settings dialog moves when you talk
- The OBS log shows a warning after about 10 seconds of silence

### Logs
Messages from the plugin start with `[Audio to WebSocket]` in OBS's log (Help → Log Files).

## Building

This plugin uses the official OBS plugin template build system.

Requirements: CMake 3.28+, Qt6 and a C++17 compiler (Visual Studio 2022 on Windows, Xcode 14+ on macOS, GCC 11+ on Linux). On macOS and Linux you also need nlohmann-json (`brew install nlohmann-json` or `sudo apt-get install nlohmann-json3-dev`). WebSocket++ and Asio are downloaded automatically.

```bash
git clone https://github.com/omnyist/obs-audio-to-websocket.git
cd obs-audio-to-websocket

cmake --preset macos            # or windows-x64, ubuntu-x86_64
cmake --build --preset macos
```

GitHub Actions builds every push to `main`, and builds the release packages for tagged versions.

## Contributing

Contributions are welcome. Issues and pull requests are open.

## License

GPL-2.0, the same as OBS Studio.
