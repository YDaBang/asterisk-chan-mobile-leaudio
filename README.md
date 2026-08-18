# chan_mobile with a Bluetooth LE Audio transport

Asterisk's `chan_mobile` bridges a phone over classic Bluetooth HFP, so the audio
is capped at CVSD 8 kHz or, where the headset profile negotiates it, mSBC 16 kHz.
This adds a second transport: the phone's LE Audio unicast stream — LC3, 32 kHz,
10 ms frames, 80 octets — reaches Asterisk as `slin32`, and call control moves
from HFP AT commands to GTBS.

*한국어: [README.ko.md](README.ko.md)*

Against Asterisk 22.9.0.  The classic paths are untouched and still selected by
default; the LE transport is opt-in per device.

The other half of this work is [bluez-leaudio-server-fixes][bluez], four patches
for BlueZ in the acceptor role.  Running this module is what exposed them, and at
least one of them — a Unicast Server that never leaves `Releasing` — has to be
applied for a second call to work at all.  If you are reading this to build the
module, read that one too.

    classic CVSD   8 kHz    what chan_mobile gives you today
    classic mSBC  16 kHz
    LE LC3        32 kHz    what this adds

## The split that decides the design

`bluetoothd` owns the LE link, the ASE state machine, and the CIS.  Asterisk does
not, and cannot without reimplementing BAP.  So the module does not open the
stream itself: a separate call-control process negotiates the stream and hands
the ISO socket over a unix socket, and `chan_mobile_leaudio.c` is the media side
of that handoff.  Everything awkward about this code follows from that boundary —
the module has to survive its peer restarting, the socket being replaced under
it, and frames arriving before the codec has any history to conceal from.

## What is here

    patches/0001-...patch    diff against 22.9.0: addons/chan_mobile.c and
                             addons/Makefile
    addons/                  new files to drop into an Asterisk tree
      chan_mobile_leaudio.c  LC3 media helper: ISO socket, decode/encode,
      chan_mobile_leaudio.h  lifecycle, and the handoff contract
      chan_mobile_lecall.c   call-control client (GTBS answer/hangup/incoming)
      chan_mobile_lecall.h
      chan_mobile_msbc.c     classic mSBC path, kept as the fallback
      chan_mobile_msbc.h
    tests/                   standalone unit tests for the media lifecycle
    build/build-chan-mobile.sh

## Building

From the top of an Asterisk 22.9.0 tree, with `$REPO` pointing at a checkout of
this repository:

    patch -p1 < "$REPO"/patches/0001-chan_mobile-add-an-LE-Audio-unicast-transport.patch
    cp "$REPO"/addons/*.[ch] addons/

The patch touches `addons/Makefile` as well as `chan_mobile.c` — without that
hunk the new source files are never compiled and you get a `chan_mobile.so` with
no LE Audio in it and no error to explain why.

You need BlueZ development headers, libsbc, and [liblc3][lc3].  `build/` holds
the recipe we use; it links SBC and LC3 statically so the module carries no
runtime dependency beyond `libbluetooth.so.3` and libc.

[lc3]: https://github.com/google/liblc3

## Configuring

Both transports are per device in `chan_mobile.conf`, and both default to
`classic`, so an existing configuration keeps behaving exactly as before:

    [phone]
    address=AA:BB:CC:DD:EE:FF
    port=0
    audiotransport=le-canary      ; or classic (default)
    callcontrol=le-gtbs           ; or classic (default)
    widebandspeech=yes            ; classic mSBC, independent of the above
    leaudiosocket=/run/asterisk-leaudio/leaudio.sock
    lecallsocket=/run/asterisk-leaudio/lecall.sock

`le-canary` is what the transport was called while it was the experiment we ran
in production, and the name stayed; read it as "the LE Audio transport".  Setting
only one of the two lines is the mistake to avoid — LE media with classic call
control, or the reverse, gives you a device that answers but carries no audio.

Reloading the module also needs the adapter's voice setting applied first
(`hciconfig <dev> voice 0x0060`), otherwise `module load chan_mobile` fails.

## The handoff reconnect is bounded

A peer that accepts the media handoff socket and closes it immediately turns the
handoff loop into a full-speed spin: connect, log, `POLLHUP`, repeat.  The
`sleep(1)` in that loop only ever guarded a *failed* connect, so a connect that
succeeded and then died instantly cost nothing and went straight round again.
One such loop wrote 2.1 million identical NOTICE lines into the first 200 MB of
a 2.9 GB log.

A connection that does not survive `MBL_LE_HANDOFF_MIN_LIFETIME` is treated as
churn and delays the next attempt, doubling to `MBL_LE_HANDOFF_BACKOFF_MAX`; one
that survives resets the delay, so a single bad patch cannot leave a healthy
link waiting.  The delay sleeps in one-second steps and rechecks the unload
flag, so it cannot hold module unload for its whole length.

The connect notice is rate limited to `MBL_LE_HANDOFF_LOG_INTERVAL` and carries
`reconnects_since_last_notice=N`.  Suppressing silently would hide exactly the
reconnect storm worth knowing about.

## Dialling during a call: the core makes the tone

GTBS has no DTMF opcode.  Accept, Terminate, Hold, Retrieve, Originate and Join
are the entire vocabulary, so on the LE path there is no way to ask the phone to
press a key.  Classic HFP has `AT+VTS` and does not need one.  The result was
that keypad presses did nothing on LE, which makes any IVR unreachable.

Asterisk already solves this, and the driver has to ask:

    if (!tech->send_digit_begin)          return 0;   /* nothing happens */
    if (!tech->send_digit_begin(chan, d)) return 0;   /* driver handled it */
    ast_playtones_start(chan, 0, dtmf_tones[...], 0); /* non-zero: core does it */

Leaving `send_digit_begin` NULL takes the first branch, so no tone is ever
produced.  `mbl_digit_begin` returns non-zero on a GTBS call and zero on
classic, where `AT+VTS` still does the work.  `ast_senddigit_end` stops the tone
when `send_digit_end` returns non-zero, which the GTBS branch already did.

The tone is generated at 8 kHz and the core translates it up, so nothing here
needs to know the LE sample rate.  It also does not strand the call at 8 kHz:
`playtones_alloc` saves the write format and `playtones_release` restores it, so
only the moment a key is held is narrowband, and what is narrowband then is a
697-1633 Hz tone rather than speech.  Generating the tone by hand is the obvious
alternative and it is the one that loses that restore.

## Caller ID over the call-control socket

The `lecall` packet is a fixed 128 bytes and carries the caller identity as an
optional payload on a call-state message:

    [0]      uri length   0..CM_LE_CALL_URI_MAX  (32)
    [1..n]   uri
    [1+n]    name length  0..CM_LE_CALL_NAME_MAX (44)
    [2+n..]  name

`CM_LE_CALL_VERSION` is 2 and the check is strict — a packet with any other
version is rejected with `-EPROTO` rather than parsed leniently, so both ends of
the socket must be replaced together.  There is no negotiation and none is
wanted: the two processes ship as a pair.

The parser refuses a URI containing anything outside `0-9 + * # -`, and a name
containing any non-printable byte.  A caller ID reaches a SIP header, so the
peer is not trusted to have filtered it.

Both lengths may be zero.  Our own deployment sends the number and leaves the
name empty, because the softphone syncs the same address book and resolves the
name from the number itself; the field is kept so that a deployment which does
want it needs no protocol change.  When the number is withheld by the network or
the originating server, the module renders `Anonymous`.

The module logs whether a caller ID was present, never its value.

## Tests

The media lifecycle tests need only liblc3 — no Asterisk tree, no Bluetooth
hardware, no phone:

    cc -O2 -Wall -Iaddons -I/path/to/lc3/include \
        tests/test_media_lifecycle.c addons/chan_mobile_leaudio.c \
        /path/to/liblc3.a -lm -o test_media_lifecycle
    ./test_media_lifecycle

Nine cases, covering the failures that actually bit us: empty SDUs at startup
being mistaken for a dead stream, `POLLHUP`/`POLLERR`/`POLLNVAL` classification,
file descriptors being reused across calls, and concealment running before the
decoder has any history.  That last one is worth spelling out, because the first
fix for it was silently wrong: packet loss concealment extrapolates from the
previously decoded frame, so before the first real frame there is nothing to
extrapolate from and it emits noise.  Gating on "has this stream made progress"
does not work either — a successful *send* also counts as progress, and TX always
lands first.  The gate has to be RX-only.

## Reproducing our exact binary

`build/build-chan-mobile.sh` asserts the ABI checksum and the dependency set of
the module we run, so it will refuse to finish anywhere else.  If you only want a
working module, drop the `AST_BUILDOPT_SUM` and `needed=` assertions.  If you
want the same bytes, the tree has to be configured with `--without-pjproject` and
`--with-jansson-bundled`, zlib must *not* be staged, and the checkout has to sit
at the same absolute path — only the Asterisk tree is passed through
`-ffile-prefix-map`, so the dependency roots leak into debug info.

## Limitations

This is a working bridge, not a general driver.  It was developed against one
Android phone and one adapter, and the LE transport assumes the 32 kHz / 10 ms /
80-octet configuration that phone offers.  Other configurations are not
negotiated.

The first ~500 ms of a call can be choppy.  LE receive and RTP send both measure
clean across that window, so the remaining suspect is what the phone sends; it is
not diagnosed.

The BlueZ side is not optional, as noted at the top: a Unicast Server that still
holds a transport reference never leaves `Releasing`, and the phone's LE Audio
watchdog tears the group down about 3.5 seconds later.  Every call after the
first one then fails, silently.  That fix and three others are in
[bluez-leaudio-server-fixes][bluez].

[bluez]: https://github.com/YDaBang/bluez-leaudio-server-fixes

## License

GPL-2.0-only, matching Asterisk.  See `LICENSE`.
