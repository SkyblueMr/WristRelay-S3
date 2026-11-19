# Voice gateway protocol

The firmware needs a gateway supplied by the builder. This repository does not
include a gateway, account service, or hosted endpoint.

## Connection

Configure a `wss://` URL for the gateway's voice route. The watch verifies the
certificate chain and hostname against the ESP-IDF CA bundle. Wi-Fi and SNTP
must be ready before opening a call. Use a certificate accepted by that bundle,
or provision your trust anchor via your build's certificate bundle. Use a plain
route path without URL userinfo, a query string, or a fragment. When a token is
configured, the WebSocket HTTP upgrade includes
`Authorization: Bearer <token>`; with no token, no authorization header is sent.
Only use a no-token route in an environment with equivalent access controls.
Leave ESP-IDF's per-tag log control enabled for token-authenticated calls; the
firmware disables WebSocket transport request logging before sending a token.

One successful WebSocket upgrade starts one call. There is no JSON start or ready
exchange, no WebSocket subprotocol, and no proprietary authentication or device
ID. The gateway must finish admission before accepting the upgrade. Any response
other than HTTP `101`, including a redirect, fails the connection. The gateway can
begin reading or sending audio immediately after upgrading.

## Audio messages

Each binary WebSocket message in either direction contains exactly one raw Opus
packet (1 to 1,275 bytes), with exactly 60 ms of mono audio. Do not add a length,
timestamp, sequence number, header, or container. There is no Ogg or RTP framing.
The watch's uplink Opus encoder operates on 16 kHz mono audio (960 samples per
packet). A gateway may decode at 16 kHz or another rate supported by its Opus
decoder. For downlink, encode 60 ms per packet at an Opus-supported rate; the watch
decodes to 24 kHz mono (1,440 samples at the speaker). Use normal Opus audio
packets, not zero-byte messages or decoder packet-loss markers.

The watch accepts WebSocket fragmentation and TCP reads that split a frame.
Standard WebSocket ping/pong and close are handled as control frames, not audio.
Do not send text, metadata, or a batch of raw packets as one message. Pace downlink
at the playback rate; the watch buffers at most four queued packets, plus one
being played. Unsupported messages, queue overflow, invalid Opus, transport
loss, and audio I/O failure end the call with an error. There is no retry or
audio replay. This public path does not provide acoustic echo cancellation.

## Ending a call

Send WebSocket close code `1000` to end normally after the last audio packet.
The watch also recognizes a close frame with no code as normal and drains audio
already enqueued. Other close status codes and abrupt disconnects are treated as
failures. A local stop discards queued downlink audio and starts a normal WebSocket
close handshake if connected. Any subsequent call starts a new connection and
codec state.
