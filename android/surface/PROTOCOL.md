# ioswine-surface protocol, version 1

Wire protocol between the iOS-side renderer (client) and the Android
presenter (server). Implemented in `src/common/protocol.{h,c}`, shared
verbatim by both sides. The host unit test (`tests/host/test_protocol.c`)
is the executable form of this spec: if this document and the test
disagree, the test wins and this document is wrong: fix it.

## Transport

| Item | Value |
|---|---|
| Socket | `AF_UNIX`, `SOCK_STREAM`, **abstract** namespace |
| Name | `@ioswine-surface` (sun_path[0] = 0, then `"ioswine-surface"`) |
| Byte order | Little-endian everywhere, including `float` (IEEE-754) |
| Max payload | 65535 bytes (header `length` field is u32 but values above the cap are rejected) |
| Max clients | 1 at a time; a second connect gets `ERROR` + close |

The socket is abstract (no filesystem entry), so no path permissions
apply; any process on the device that can speak the protocol can
connect. That is intentional for the prototype: the iOS side and the
presenter are both ours. If this ever runs on a non-rooted multi-user
device, gate the socket with an `SO_PEERCRED` uid check (not
implemented in v1: see §9).

## Framing

Every message is a 12-byte header followed by `length` payload bytes:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `magic` | `0x49535753` (`"ISWS"`, little-endian) |
| 4 | 2 | `type` | see §3 |
| 6 | 2 | `flags` | 0 in v1 for all messages |
| 8 | 4 | `length` | payload bytes after the header |

The receiver must validate `magic` and `length <= 65535` before
reading the payload, and must validate the payload length against the
message type before parsing it. Unknown types are a protocol error:
the server replies `ERROR` (`ISWS_ERR_BAD_MESSAGE`) and closes the
connection; the client logs and disconnects.

All multi-byte integers are written with explicit little-endian
helpers (`isws_wr_u32le` etc.): never by casting a struct onto the
wire, so host struct padding can never leak into the protocol.

## Message types

| Type | Value | Direction | Payload size |
|---|---|---|---|
| `HELLO` | 1 | C → S | 24 |
| `PRESENT` | 2 | C → S | 16 + 16 × damage_count |
| `GOODBYE` | 3 | C → S | 0 |
| `READY` | 0x81 | S → C | 12 |
| `FRAME_DONE` | 0x82 | S → C | 24 |
| `TOUCH` | 0x83 | S → C | 28 |
| `KEY` | 0x84 | S → C | 16 |
| `INFO` | 0x85 | S → C | 32 |
| `FOCUS` | 0x86 | S → C | 8 |
| `ERROR` | 0x87 | S → C | 4 + message bytes |

### HELLO (C → S, 24 bytes): plus one fd via SCM_RIGHTS

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `version` | 1 |
| 4 | 4 | `width` | client frame width, px (1..8192) |
| 8 | 4 | `height` | client frame height, px (1..8192) |
| 12 | 4 | `stride` | bytes per row of one buffer; multiple of 4, `>= width*4`, `<= 32768` |
| 16 | 4 | `format` | 1 = `BGRA8888_PREMUL` (only format in v1) |
| 20 | 4 | `flags` | 0 (reserved) |

The HELLO is sent as a single `sendmsg` whose ancillary data carries
exactly one fd: a **memfd** holding both frame buffers back to back.
Buffer 0 starts at offset 0, buffer 1 at offset `stride*height`.
Total memfd size:

```
memfd_size = stride * height * 2
```

The server `fstat`s the fd and rejects it if it is smaller than
`memfd_size`. The fd must arrive with the HELLO; a HELLO without an
fd gets `ERROR` (`ISWS_ERR_NO_FD`) and the connection is closed.
Any fd arriving on a non-HELLO message is closed and treated as a
protocol error.

Pixel format: **BGRA8888 premultiplied**, 4 bytes per pixel in memory
order B, G, R, A. Alpha is expected to be 0xFF for opaque frames; the
server does not un-premultiply (it copies the channels as-is).

### PRESENT (C → S, 16 + 16 × damage_count bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | `frame_id` | client-chosen, monotonically increasing (starts at 0) |
| 8 | 4 | `buffer_index` | 0 or 1 |
| 12 | 4 | `damage_count` | 0..16; **0 means the whole frame** |
| 16 + 16×i | 16 | `damage[i]` | rect: `x` i32, `y` i32, `w` u32, `h` u32 (client px) |

Damage rects may extend outside the frame or be empty; the server
clips each to `[0,0,width,height)` and drops empties
(`isws_expand_damage`). A PRESENT with `damage_count > 16`, a payload
length that does not match `16 + 16*damage_count`, or a
`buffer_index >= 2` is a protocol error.

### GOODBYE (C → S, 0 bytes)

Clean disconnect. The server releases the session (unmaps the memfd,
closes the fd). The client may also just close the socket; the server
treats EOF the same way.

### READY (S → C, 12 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `version` | server protocol version (1) |
| 4 | 4 | `flags` | 0 (reserved) |
| 8 | 4 | `max_in_flight` | 2 (== number of buffers) |

Sent once, immediately after a HELLO is accepted. The client must
wait for READY before presenting (the test client waits up to 5 s).

### FRAME_DONE (S → C, 24 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | `frame_id` | echoes the PRESENT's `frame_id` |
| 8 | 8 | `vsync_ns` | `CLOCK_MONOTONIC` ns of the vsync the frame was posted on (see §6) |
| 16 | 4 | `status` | 0 = presented, 1 = dropped |
| 20 | 4 | `reserved` | 0 |

One FRAME_DONE is sent for **every** PRESENT, including dropped ones
and ones presented while there is no window yet. The client must never
wait forever for an ack: if no ack arrives within one frame period it
may present on the other buffer anyway (see §6).

### TOUCH (S → C, 28 bytes)

One message per pointer per motion event (a MOVE with N pointers down
produces N messages with the same timestamp).

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `action` | 0 = down, 1 = move, 2 = up, 3 = cancel |
| 4 | 4 | `pointer_id` | stable per contact, 0..9 |
| 8 | 4 | `pointer_count` | pointers currently down, including this one (1..10) |
| 12 | 4 | `x` | float, **client pixels** (after scale/letterbox mapping) |
| 16 | 4 | `y` | float, client pixels |
| 20 | 8 | `timestamp_ns` | `CLOCK_MONOTONIC` |

Coordinates are `float` (not fixed-point): the mapping involves a
non-integer scale, and float precision (~0.001 px at these magnitudes)
is far beyond what a touch needs. Points that fall in the letterbox
bars are handled by phase, not by clamping everything: a DOWN in a bar
is dropped (no contact begins there, so a tap on a black bar never
becomes a tap on the app edge); a MOVE or UP for a finger that is
already down is clamped to the nearest client pixel, because the finger
must still be able to lift. The client cannot tell from the message
alone whether a touch was clamped; it does not need to, because a
contact never begins in a bar.

`pointer_id` is a slot (0..9), not the Android pointer id: the
presenter maps Android ids to slots first come, first served, and
drops a DOWN when no slot is free rather than aliasing it onto another
contact. `pointer_count` counts the pointers currently down including
the finger in the message (for UP, the count before it is removed).
CANCEL applies to every pointer that is down, so the presenter sends
one CANCEL per active pointer. There is no pressure field in this
payload; the client synthesizes 1.0 while a finger is down and 0.0 on
UP/CANCEL. A MOVE with N pointers down produces N messages with the
same timestamp, preceded by that event's historical samples, oldest
first.

### KEY (S → C, 16 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `keycode` | 4 = back (only key in v1) |
| 4 | 4 | `action` | 0 = down, 1 = up |
| 8 | 8 | `timestamp_ns` | `CLOCK_MONOTONIC` |

### INFO (S → C, 32 bytes): RESIZE / ROTATION / mapping

Sent after READY (once a window exists) and again on every
rotation/resize/config change. The client must use the latest INFO
for touch mapping; the server already maps touches into client pixels
using this same mapping, so INFO is mostly for the client's own
layout: but it is the authoritative statement of the mapping.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `screen_w` | window width, physical px |
| 4 | 4 | `screen_h` | window height, physical px |
| 8 | 4 | `client_w` | client frame width, px |
| 12 | 4 | `client_h` | client frame height, px |
| 16 | 4 | `scale` | float, aspect-fit scale (> 0) |
| 20 | 4 | `offset_x` | float, screen px of the client frame's top-left corner |
| 24 | 4 | `offset_y` | float, same for y |
| 28 | 4 | `rotation` | 0, 90, 180 or 270 (see note below) |

Mapping (aspect-fit letterbox, computed by `isws_compute_mapping`):

```
scale    = min(screen_w / client_w, screen_h / client_h)
offset_x = (screen_w - client_w * scale) / 2
offset_y = (screen_h - client_h * scale) / 2
screen   = client * scale + offset
client   = (screen - offset) / scale   (clamped to the client frame)
```

Rotation note: `AConfiguration` only exposes portrait/landscape, not
exact degrees, so the server reports 0 for portrait and 90 for
landscape. The exact value is informational only: the mapping is
fully determined by `screen_w`/`screen_h`/`scale`/`offset_*`, which
are always correct. Do not use `rotation` for touch mapping.

### FOCUS (S → C, 8 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `focused` | 0 = lost focus, 1 = gained focus |
| 4 | 4 | `reserved` | 0 |

### ERROR (S → C, 4 + N bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `code` | see below |
| 4 | N | `message` | raw bytes, not NUL-terminated |

| Code | Value | Meaning |
|---|---|---|
| `ISWS_ERR_NONE` | 0 | (not sent) |
| `ISWS_ERR_VERSION` | 1 | unsupported protocol version |
| `ISWS_ERR_BAD_HELLO` | 2 | invalid width/height/stride/format, or memfd too small |
| `ISWS_ERR_BAD_MESSAGE` | 3 | unknown type / bad magic / bad length / HELLO not first |
| `ISWS_ERR_BAD_PRESENT` | 4 | bad buffer index / damage / PRESENT before HELLO |
| `ISWS_ERR_NO_FD` | 5 | HELLO arrived without the memfd |
| `ISWS_ERR_INTERNAL` | 6 | server-side failure (mmap failed, surface busy, …) |

After sending ERROR the server closes the connection, except for the
"surface busy" case where the newcomer gets the error and the existing
session is untouched.

## State machine

```
Client                          Server
  │  connect @ioswine-surface     │
  ├─ HELLO + memfd (SCM_RIGHTS) ─►│ validate, fstat, mmap
  │                               │ (reject: ERROR + close)
  │◄─ READY ──────────────────────┤
  │◄─ INFO ───────────────────────┤ (if a window exists; otherwise
  │                               │  on first APP_CMD_INIT_WINDOW)
  │                               │
  ├─ PRESENT(i, rects, id) ──────►│ copy damage rects to window,
  │                               │ ack FRAME_DONE(id, vsync, status)
  │◄─ FRAME_DONE ─────────────────┤
  │                               │
  │◄─ TOUCH / KEY / INFO / FOCUS ─┤ (any time, async)
  │                               │
  ├─ GOODBYE ────────────────────►│ release session
  │  close                        │ (EOF is equivalent to GOODBYE)
```

Reconnection: the server survives a client disconnect. A new
connection starts over with a fresh HELLO; the previous session's
memfd is unmapped and its fd closed. If a second client connects
while one is active, the newcomer is rejected with ERROR and the
existing session is untouched.

## Vsync pacing

- The server runs an `AChoreographer` frame callback that records the
  latest vsync timestamp (`CLOCK_MONOTONIC` ns). Every FRAME_DONE
  carries the most recent vsync timestamp at ack time. This is the
  vsync the frame was posted on for practical purposes; it is **not**
  a guarantee the frame hit that exact vsync: the lock/unlockAndPost
  path hands the buffer to SurfaceFlinger, which presents it at the
  next vsync.
- The client may have at most 2 frames in flight (one per buffer).
  It paces by waiting for the FRAME_DONE of the buffer it is about to
  reuse, with a timeout of one frame period; on timeout it presents
  anyway on the other buffer.
- Stale PRESENT policy: if a PRESENT names a buffer that is still
  marked in flight (a previous PRESENT for that buffer has not been
  acked yet: this can only happen if the client ignored the
  max-in-flight rule), the server **drops** it and immediately acks
  `FRAME_DONE(status=1)`. It never blocks the socket thread and never
  presents the same buffer twice. The client may reuse the buffer
  immediately after a dropped ack.

## Limits

| Item | Limit |
|---|---|
| Frame width / height | 1..8192 px |
| Stride | multiple of 4, `width*4 .. 32768` bytes |
| Damage rects per PRESENT | 0..16 (0 = whole frame) |
| Simultaneous pointers | 10 |
| Buffers | 2 (double-buffered) |
| Frames in flight | 2 |
| Payload per message | 65535 bytes |

## Version negotiation

v1 has no negotiation: the client sends `version = 1` and the server
rejects anything else with `ISWS_ERR_VERSION`. If a v2 is ever needed,
the sane path is a new socket name (`@ioswine-surface2`) or a
`HELLO` with a higher version that the server answers with either
READY (if it speaks that version) or ERROR: do not try to negotiate
inside v1's HELLO, it has no room for a version list.

## Security notes (v1, prototype)

- No authentication: any local process can connect and can send
  touches' worth of frames to the screen. Acceptable while both ends
  are ours on a rooted dev phone; add an `SO_PEERCRED` uid check
  before any wider use.
- The server validates every length before reading and every geometry
  before using it; a malformed client cannot crash the server (the
  host unit test covers the malformed-input cases). The memfd is
  mapped read-only (`PROT_READ`) on the server side.
- The server never executes or interprets client pixels: they are
  copied as data only.
