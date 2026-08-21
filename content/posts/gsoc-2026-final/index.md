+++
title = "GSoC 2026 final report"
description = "Bringing USB redirection to qemu-rdp: implementing MS-RDPEUSB in IronRDP and bridging it to QEMU's usbredir."
date = 2026-08-21
draft = false

[taxonomies]
categories = ["Learning"]
tags = ["rust", "programming", "gsoc-2026", "qemu", "rdp"]
+++

## Overview

[original idea](https://wiki.qemu.org/Internships/ProjectIdeas/RDPUSB)

[My project](https://summerofcode.withgoogle.com/programs/2026/projects/ssoc2JtG)
this summer is to implement USB redirection for [qemu-rdp]. qemu-rdp is an [RDP]
server for the `org.qemu.Display1` D-Bus interface that [QEMU] exposes with
`-display dbus`. It runs as its own process, outside of QEMU, and turns a
virtual machine into a remote desktop that speaks a protocol every platform
already has a client for. It could already carry display, input, audio and
clipboard, but not a USB device.

And my work aims to close that gap. USB redirection feels like plugging a local
USB device into a remote QEMU machine over the network.

RDP carries everything beyond the core desktop over _dynamic virtual channels_,
multiplexed by a static `DRDYNVC` channel ([MS-RDPEDYC]). [MS-RDPEUSB] defines
one named `URBDRC` for USB. What it carries is deliberately low level: **URBs**,
raw USB Request Blocks, so a device works without any code written for its
class.

QEMU, meanwhile, already speaks [usbredir], the protocol SPICE has used for USB
pass-through for years. `-device usb-redir` gives the guest an emulated port
and exposes the other end as a chardev, which QEMU offers over the same
`org.qemu.Display1` D-Bus interface. That chardev is qemu-rdp's way in.

So the project is a bridge. qemu-rdp plays the usbredir host, except it owns no
device: each request from QEMU becomes a URB sent over `URBDRC` to the RDP
client, and completes when the answer comes back.

```
[physical USB] → RDP client → URBDRC over DRDYNVC → qemu-rdp → usbredir → QEMU → [guest]
```

## My Work

All my work can be found in these repos:

> my info: @uchouT <i@uchout.moe>

- [qemu-rdp] - the RDP server that exports a QEMU virtual machine. I wrote the
  USB redirection backend here, bridging `URBDRC` to usbredir.
- [IronRDP] - the Rust RDP stack qemu-rdp is built on. The MS-RDPEUSB
  implementation lives here, along with the dynamic virtual channel work it
  needed.
- [FreeRDP] - the widely used C implementation of RDP, and the client I tested
  against. A few `urbdrc` bug fixes.
- [QEMU][qemu-repo] - just a documentation fix for the D-Bus chardev.

I'll pick some highlights from my GSoC project below.

[demo video](https://youtu.be/0Pqclo0T26Q). (Bad oral English warnings!)

### Decoupling `qemu-display`'s usbredir manager

qemu-display could already redirect a USB device to QEMU, but only a local
physical one. Its `usbredir` module did not merely use `usbredirhost` — it
_was_ the usbredirhost session: a `DeviceHandler` implementation with its own
`Drop` and raw file-descriptor polling, keyed by USB bus and device address.
`qemu_display::Error` carried `Rusb` and `Usbredir` variants, so every consumer
of the crate depended on libusb, and `Display::usbredir()` handed back a
manager with nothing to substitute.

qemu-rdp has no local device to give it. Its device lives behind the RDP
connection, and is driven by the client rather than by usbredirhost. But the
part it does need was already written: finding a free chardev, creating the
socket pair, registering one end over D-Bus, and tracking the live sessions and
the free-channel count.

So I separated them. A `UsbRedirBackend` trait and a generic `UsbRedir<B>` keep
the chardev orchestration in qemu-display; the usbredirhost session moved out
to qemu-rdw, where it belongs; the rusb error variants left `qemu_display::
Error`; and `Display::usbredir()` became `usbredir_chardevs()` plus explicit
backend injection.

- [qemu-display!8] - decouple the usbredir channel manager from the host
  backend

### Refactoring `ironrdp-dvc`

URBDRC asks more of the DVC layer than any channel before it. Channels have to
be created and closed at runtime, and every instance shares one name — a
control channel plus one channel per redirected device, told apart only by the
ID `DRDYNVC` assigns them. `ironrdp-dvc` could do neither: channels were
registered once at build time, and the registry was keyed by name, so a second
`URBDRC` would have overwritten the first. It took several PRs to get there.

- [IronRDP#1142] - `DvcChannelListener`, so one name can produce many
  processors
- [IronRDP#1302] - closing a channel from either side
- [IronRDP#1368], [IronRDP#1377] - typed accessors, so a caller recovers a
  processor together with its channel ID and the wrapper types stay private
- [IronRDP#1416] - reserve the ID before building the processor, so the
  processor can use it while initializing

The first of those came out of a design argument I am glad I made.
[IronRDP#1135] arrived with
a plan already sketched, and the plan was to let the DVC layer do the work: a
protocol processor would queue up the channels it wanted, and the layer beneath
would notice and open them on its next pass. It would have worked for USB
redirection. I argued against it anyway.

Two things were wrong with it. That layer only wakes up when the client sends
something, so channels could only ever be opened in reaction to the client —
while RDPEDYC is clear that the server is the side that opens them. And it asked
a layer whose whole job is to speak RDPEDYC to also understand _why_ a channel
was wanted, a question only the protocol above it can answer. Convenient for USB
redirection, useless to anything else, and paid for out of the layering.

What I proposed instead was that the DVC layer simply offer a way to create a
channel and know nothing about who asked or why, leaving that decision to the
layer that actually has the context.

### `ironrdp-rdpeusb` PDU codec

[IronRDP#1165] had left a PDU
skeleton in place, and I finished the codec on top of it. The idea running
through these PRs is to let the type system carry that context, so an invalid
state is not merely rejected at runtime but impossible to build.

- [IronRDP#1294] - lift `SHARED_MSG_HEADER` out of every PDU struct, so callers
  cannot assemble a mismatched header
- [IronRDP#1321] - split the PDU and `TS_URB` enum wrappers
- [IronRDP#1403] - separate raw and validated `InternalIoControl`
- [IronRDP#1456] - type the URB completion payloads

### URBDRC state machine

Sans-I/O protocol processors for both the server and the client side. This is
one of the most interesting and challenging part of my GSoC journey :) The
traits are the seam between the protocol and whatever backend sits behind it,
so designing them meant working out the whole data flow first.

Setting up a new device channel is the trickiest part of that flow. When a
device is plugged in on the client, the client asks the server to open a
channel for it, and the server does, but what comes back carries nothing except
a channel ID. To the DVC layer it is just another `URBDRC` channel,
indistinguishable from the control channel or from any other device. The client
still has to announce the device on it with `ADD_DEVICE`, and `ADD_DEVICE`
needs that device's information.

Nothing in the protocol correlates the two, and the processor cannot do it
either — it has no way of knowing which device is its own. Only the backend
knows, because it is the side that asked for the channel in the first place. So
that became part of the trait: when a new channel appears, the backend is asked
to hand over the device it has been waiting to redirect, and the device
information is fetched from it again later, at the moment `ADD_DEVICE` is
actually assembled.

- [IronRDP#1365] - the client processors
- [IronRDP#1394] - the server processors, plus the backend-facing `io` model
  shared by both sides

### Getting it working first

The protocol library was finished, and completely untested against anything
real. So before polishing it any further I went for an end-to-end prototype:
wire usbredir into qemu-rdp, redirect one device, and find out whether the
state machine I had written actually held up.

qemu-rdp builds on `ironrdp-server`, so the wiring had to start there. I opened
[IronRDP#1417] as a draft to
hold the place, then moved over to qemu-rdp and put usbredir in as fast as I
could. It worked in the end: FreeRDP connected, and a USB flash drive showed up
inside the guest. LLMs helped a lot during that push.

The result was buggy and not something I would want to maintain, but that was
never the point. The point was to find out what I did not know yet, and it
delivered:

- a couple of bugs in FreeRDP's own urbdrc client
- a set of edge cases in the Linux USB API that I only cornered by stracing the
  kernel to see what was actually being submitted — an interesting journey in
  itself :)
- USB state management and USB-to-RDPEUSB translation turned out to be far more
  work than I expected, and meanwhile I ran into [macrdp], another RDP server
  built on `ironrdp-server` that is solving exactly the same problem.

By the end of that detour I had enough context to see how the work should be
actuall landed.

### USB model

The duplicated part was never really application work. Driving a redirected
device through the protocol processors alone meant assembling `TS_URB`
structures and tracking USB device state by hand, and doing it again in every
application that wanted to redirect a device. So I split it into three layers,
each one allowed to speak only its own vocabulary:

- `ironrdp-usb` — USB and nothing else: protocol-independent, `no_std`,
  sans-I/O, no dependencies. It describes USB operations without executing them,
  and parses only the byte layouts USB itself defines.
- `ironrdp-rdpeusb::usb` — translation. An `ironrdp-usb` request goes in, a
  complete RDPEUSB packet comes out, so the `TS_URB` payload, URB function,
  transfer envelope, flags and buffer shape cannot disagree with each other.
- `ironrdp-server` — the facade, and the request lifetime that comes with it:
  request IDs, completion routing, and RAII pending requests that cancel
  themselves on the wire when dropped. A caller never names a `TS_URB`.

Above those sits the application. qemu-rdp only has to deal with what usbredir
itself imposes, and macrdp, or anything else built on IronRDP, gets the same
three layers for free. The full rationale is in
[IronRDP#1516].

- [IronRDP#1682] - the new `ironrdp-usb` crate
- [IronRDP#1683] - the RDPEUSB translation module
- [IronRDP#1417] - the server integration and the device facade

### Rebuilding the qemu-rdp bridge

With the three layers in place I went back to qemu-rdp and rewrote the USB side
on top of them.

The rewrite splits along the line the layering suggested. `session_task` is the
transport half: it frames the QEMU chardev, drives [usbredir-proto]'s sans-I/O
parser, and holds no USB or usbredir state of its own. `bridge` is everything
else — capability negotiation, device state, packet translation, and
correlating each usbredir request with the RDP completion that eventually
answers it.

Correlation turned out to be the subtle one. A usbredir request and an RDPEUSB
request are not the same object and do not end at the same moment, so the piece
that tracks them in flight tracks only their lifetime; what a request is _for_
rides along with it and is never interpreted there. Lifetime and meaning stay
in different places.

- [gsoc-2026-usbredir](https://gitlab.com/marcandre.lureau/qemu-display/-/commits/gsoc-2026-usbredir)

## Future Work

- Land the rest of the IronRDP work. The new crates need a release before
  anything downstream can depend on them.
- Interrupt IN and isochronous transfers in qemu-rdp. Only what a mass-storage
  device needs is wired up today.
- Test against mstsc. FreeRDP has been my only client so far, and the Windows
  one decides what the spec means in practice.
- URBDRC support in `ironrdp-client`. IronRDP can serve a redirected device,
  but cannot yet redirect one.
- Rebuild rdw-rdp on `ironrdp-client`, now that it is a library rather than a
  binary — and pick up URBDRC along with it.

## Thoughts

Actually, I have a lot to talk about, but to not make this post tedious, I just
list tl;dr here. More about them may be published in the future, tagged with
[#gsoc-2026](/tags/gsoc-2026).

### Data matters a lot

Data comes first, and code is shaped to fit it. Looking back, most of the time
I thought I was designing behaviour I was really deciding something else: what
to store, where it should live, and who is allowed to know it. The hard
question was never _what should this function do_, but _who holds this piece of
information, and when_.

With the wrong data, code turns ugly and fills up with hacks. With the right
data, it comes out simple and elegant.

### Deciding what not to do

In [The one ring problem: abstraction and our quest for power] (I found this
blog series in IronRDP repo's `ARCHITECTURE.md`), Ted Kaminski describes how
programmers are tempted to reach for ever more power in their abstractions. The
reach is tempting because power arrives as convenience and bills you later.

Most of the decisions I am happy with this summer were refusals. Again and
again I was tempted to let a layer learn one small thing about the layer above
it, because it would have made the case in front of me much easier. Leaving it
out was never a limitation I ran into; it was a capability I declined to take,
and it always felt like the long way round.

What you get back is a boundary the next person can reason about without
reading your code. The less a layer exposes, the less there is to get wrong.

### Communication in open source

In open source, people meet each other through the work itself. IronRDP's
maintainer [@CBenoit](https://github.com/CBenoit) reached out on Matrix to say
he liked my sense of architecture, and later asked for my help on the project's
LLM harness — something well outside what I had signed up for. None of that
came from introducing myself well.

There is a real freedom in that. No pleasantries to perform, no self-packaging
(I feel really uncomfortable with it!!), no reading the room. You say the
thing, you show the code, and the work carries exactly the weight it earns.

## Thanks

- [Marc-André Lureau](https://github.com/elmarco) - my mentor, who helped
  enormously throughout, needless to say
- [IronRDP] community - for the reviews, and I really learned a lot from it
- [glamberson](https://github.com/glamberson) - we had a nice
  [discussion](https://github.com/Devolutions/IronRDP/issues/1077#issuecomment-4528328787)
  about where the spec is ambiguous
- [clintcan](https://github.com/clintcan) - [macrdp] maintainer, helps me with
  the test, and offers valuable mstsc cases
- [playbahn](https://github.com/playbahn) - who did much of the early work on
  the PDU codec
- [QEMU] community - for taking me on as a GSoC org, and for `-display dbus`
  and usbredir, the two things this whole project stands on
- [Google](https://summerofcode.withgoogle.com/) - for running Summer of Code,
  which is how all of this started

[qemu-rdp]: https://gitlab.com/marcandre.lureau/qemu-display/-/tree/master/qemu-rdp
[IronRDP]: https://github.com/Devolutions/IronRDP
[MS-RDPEUSB]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeusb/a1004d0e-99e9-4968-894b-0b924ef2f125
[MS-RDPEDYC]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/3bd53020-9b64-4c9a-97fc-90a79e7e1e06
[usbredir]: https://www.spice-space.org/usbredir.html
[RDP]: https://en.wikipedia.org/wiki/Remote_Desktop_Protocol
[QEMU]: https://qemu.org
[usbredir-proto]: https://github.com/elmarco/usbredir-proto
[macrdp]: https://github.com/clintcan/macrdp

[FreeRDP]: https://github.com/FreeRDP/FreeRDP
[qemu-repo]: https://gitlab.com/qemu-project/qemu
[qemu-display!8]: https://gitlab.com/marcandre.lureau/qemu-display/-/merge_requests/8
[IronRDP#1142]: https://github.com/Devolutions/IronRDP/pull/1142
[IronRDP#1302]: https://github.com/Devolutions/IronRDP/pull/1302
[IronRDP#1368]: https://github.com/Devolutions/IronRDP/pull/1368
[IronRDP#1377]: https://github.com/Devolutions/IronRDP/pull/1377
[IronRDP#1416]: https://github.com/Devolutions/IronRDP/pull/1416
[IronRDP#1135]: https://github.com/Devolutions/IronRDP/issues/1135
[IronRDP#1165]: https://github.com/Devolutions/IronRDP/pull/1165
[IronRDP#1294]: https://github.com/Devolutions/IronRDP/pull/1294
[IronRDP#1321]: https://github.com/Devolutions/IronRDP/pull/1321
[IronRDP#1403]: https://github.com/Devolutions/IronRDP/pull/1403
[IronRDP#1456]: https://github.com/Devolutions/IronRDP/pull/1456
[IronRDP#1365]: https://github.com/Devolutions/IronRDP/pull/1365
[IronRDP#1394]: https://github.com/Devolutions/IronRDP/pull/1394
[IronRDP#1417]: https://github.com/Devolutions/IronRDP/pull/1417
[IronRDP#1516]: https://github.com/Devolutions/IronRDP/discussions/1516
[IronRDP#1682]: https://github.com/Devolutions/IronRDP/pull/1682
[IronRDP#1683]: https://github.com/Devolutions/IronRDP/pull/1683
[IronRDP#1711]: https://github.com/Devolutions/IronRDP/pull/1711

[The one ring problem: abstraction and our quest for power]: https://www.tedinski.com/2018/01/30/the-one-ring-problem-abstraction-and-power.html
