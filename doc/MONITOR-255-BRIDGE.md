# The WAMPES mirror onto the AGWPE monitor channel

How frames a libax25 program sends and receives over the **wampes** backend
find their way into the AGWPE **monitor channel 255**, where a `listen`
program can show them with the has-been-repeated mark (`*`) on the digipeaters
that have already repeated.  AGWPE, direwolf and ax25netd are not touched.

## What it is

Not the WAMPES *node* feeds the monitor — the **libax25 client side** of the
wampes bridge does.  A program connected to the radio through **wampes** holds,
beside its session channel, an AGWPE client on ax25netd's loop port 255 and
**mirrors** every wampes UI frame — inbound and outbound — as a raw `'K'`
record.  The session data flow is not disturbed.

The mirror carries **only wampes traffic**.  What a program sends and receives
to and from AGWPE (direwolf) is not mirrored back: there ax25netd fills the
monitor all by itself, and a second copy would be a doubling.

Why the split: ax25netd joins every AGWPE upstream (direwolf) as an AGWPE
client and hands each raw inbound frame to its monitor clients unchanged,
repeated bits intact.  A WAMPES node is no AGWPE upstream and gives ax25netd
nothing to tap, so the libax25 side of the wampes bridge lays those frames on
the loop port itself.

Where the mirror connects is the same place the AGWPE backend looks:
`AXSOCK_HOST`/`AXSOCK_PORT` (default 127.0.0.1:8100; a leading `/` is a unix
socket; `AXSOCK_USER`/`AXSOCK_PASSWORD` log in where the server asks).  A link
that has dropped is thrown away and re-established at the next frame.  An
unreachable server fails silently — the wampes bridge itself keeps working
without ax25netd.

Wampes text frames know no protocol id; the mirror stamps them with 0xf0
(AX.25) where the socket has not set a pid of its own.

## The has-been-repeated bit

Two frame kinds can sit on the monitor.  Raw `'K'` records carry the repeated
bit as the `0x80` bit of the SSID byte of every digipeater — 1:1.  Decoded
records (`'I'`/`'S'`/`'U'`/`'T'`) show it as a `*` after the name, which
direwolf often leaves off.  libax25 and `listen` use only the raw records, so
the `*` arrives as the real bit, exactly as a digipeater left it.

On the way in, the node reports the path with `*` on the hops that already
repeated; the bridge turns that into the has-been-repeated bit and normalises
the gaps the node leaves in a path (it marks only the last one).  On the way
out, an application sets the bit in the SSID byte of `sendto(2)`'s sockaddr;
the header line to the node carries the `*` again and the node emits the
on-air bit.  Because an on-air path is contiguous, both directions fill every
digipeater up to the last repeated one, so the mirror and the air show the
same thing.

## What cannot be seen

The mirror hangs off the datagram path (`sendto`/`recvfrom`) and covers
**only UI frames**.  A connected AX.25 session (connect) flows completely past
it: the library hands the application a plain descriptor, and afterwards only
the I-frame payload crosses the bridge — never the control frames
(SABM/SABME/UA/DISC/REJ/FRMR), never the N(S)/N(R) sequence, never the address
headers, which live in the node's session state.  A listen-capable KISS frame
cannot be built from what the library side sees.

So for UI and datagram traffic the monitor works; for connected sessions it
needs a source inside the wampes kernel — a monitor export at its LAPB/AX.25
layer, something like a wampes-internal `listen`.  A front-end on the
libax25↔wampes session can never deliver the picture of a connected link.

## What is deliberately left alone

- AGWPE, direwolf and ax25netd stay as they are.  Their frames are mirrored
  only once — by ax25netd itself.
- `listen(1)` needs no change: it already prints `*` from the has-been-repeated
  bit.
- `setcall()` keeps refusing an unset `*` in a callsign.  The repeated bit is
  an address property, set in the sockaddr, not something typed into a name.

## Open points

- **Protocol id**: wampes text frames carry no pid; the mirror stamps 0xf0
  where the socket set nothing.  A lost pid is a known limit of the idea.
- **Silent when unreachable**: a missing ax25netd or loop port raises no error
  at the application.  The channel is purely observational.
- **Distribution to listeners**: before a new name is invented, look whether
  the existing `ax25mond(8)` (ax25-apps, a monitor-socket distributor) can be
  fed with the mirrored frames and pass them to listen/mheardd.
- **Configuration**: using the mirror (and the wampes path at all) today needs
  `AXSOCK_BACKEND=wampes`, `WAMPES_SOCKET=...` and, until the new library is
  installed, `LD_PRELOAD=...` at the invocation.  That is too much to
  remember; the values want to settle in one place — `axports`/`wampes.conf`,
  and no preload once `make install` has run.