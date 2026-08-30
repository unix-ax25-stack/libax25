# TODO

What is known to be missing, known to be undecided, or deliberately left out.
Kept here because the three source trees are separate today and this is where
the rest of the documentation lives; when they become one repository it moves
up a level and changes nothing else.

---

## One decision point instead of three

**The structural item, and the one that would remove a whole class of faults.**

Today the backend is chosen twice, in two places, by two mechanisms:

* kernel-or-AGWPE at `socket()`, once per process, by a probe, cached
* node-or-not at `bind()`, per port, by looking the callsign up in
  `axports(5)` and `wampes.conf(5)`

The first has to happen at `socket()` only because something must be *built*
there — a kernel socket or an AGWPE-backed socketpair — and at that moment no
callsign and therefore no port is known.  The second was added later and is
built as an undoing: whatever `socket()` made is discarded and replaced behind
the same descriptor number.

There is no reason for the asymmetry beyond the order in which the two were
written.  The replacement is already described in `axsock.c`:

> socket caching: `socket()` hands back a placeholder fd, `bind()` resolves the
> callsign against axports … and `dup2()`s the real fd over the placeholder,
> keeping the visible fd number stable.

What it buys:

* `AXSOCK_BACKEND` and the probe disappear, and with them a per-process choice
  that has no business being per-process
* kernel and AGWPE ports coexist in one program, which is the only combination
  that does not work today
* the hooks in `axsock.c` that let `wampes_bind()` run *before* the table
  lookup are no longer special; one table, one dispatch
* three faults found in one day become impossible by construction: the socket
  type lost across a handover, a port nameable only through the digipeater
  slot, `sendto()` falling through to a socketpair end and answering `EISCONN`.
  All three were consequences of "socket() built one thing and bind() wanted
  another"

It is not only about branches, either.  The two backends should **look and
behave alike**, and today they do not: one is chosen by a probe and the other
by a file, one steals descriptors and the other cannot.  An asymmetry like
that reads as if AGWPE were an abandoned corner, which it is not — it was
tested and working, and it is still the way to a `direwolf` or to AGWPE
clients on the LAN.  Making the two paths the same shape is as much about not
letting one of them rot as it is about edge cases.

Which brings an acceptance step with it: **the direwolf tests want repeating
once the paths are unified.**  They last passed some weeks ago, against code
that has moved underneath them since.

What has to be answered first — descriptors that are never bound:

* `axctl(8)` and `axkill(8)` open an `AF_AX25` socket and issue
  `ioctl(SIOCAX25CTLCON)` immediately, with no `bind()` in between
* a program may `connect()` without binding, and then no port is named at all

So the rule cannot be "decide at `bind()`"; it has to be **decide at the first
call that determines it, and fall back to the process default for the calls
that cannot**.  That is a short list — `ioctl`, `connect` without a bound
callsign, `setsockopt(SOL_AX25)` — and it should be written down before it is
written in code.

---

## Open

**The has-been-repeated bit on anything we send — UI frames and I frames
alike.**  A frame received through a node carries the `*` into the
digipeater's SSID byte; a frame sent out drops it, in both directions and for
the same reason: the header is written from the address as it stands, and
nothing looks at the bit.  There are
uses — forwarding a frame, recording that the first hop already happened.

The library side is small: write the `*` into the header it already builds.
The node side is where the work is, and it is not only "parse it too".
`setcall()` treats the character inconsistently today:

* `DB0AAA*` — no SSID, so the callsign field is seven characters and it
  answers `-1`, "invalid call"
* `DB0AAA-1*` — `atoi("1*")` is 1, so the call is accepted and **the `*` is
  dropped without a word**

Both parsers refuse the mark now — `setcall()` in the node, and
`ax25_aton_entry()` here, which had arrived at the same asymmetry by itself.
So nothing is dropped in silence any more, and the day the bit is carried the
caller takes the mark off and sets it itself.

**And it need not stop at datagrams.**  A connection can express the same
thing, and the node is most of the way there already: `nextdigi` is the index
of the first digipeater that has not repeated yet, `ax25hdr.c` sets the bit
for everything before it when it writes the header, and it addresses the frame
to `digis[nextdigi]`.  What is missing is that `ax25_parse_target()` never
counts a mark, and that the connect path in `ax25user.c` sets `nextdigi = 0`
outright — it would have to keep what the caller asked for.

Nothing changes on the library side: an application marks a digipeater by
setting the has-been-repeated bit in its SSID byte, which is exactly what the
receiving path already produces, and the shim writes the `*` into the header
it builds anyway.

That gives back something the old world had: a station can emit a connect as
though the first hops had already happened — which is what a node does when it
inserts itself into a path.  Worth saying out loud that on a shared channel
such a frame is indistinguishable from a real digipeat, so it is a tool and a
footgun in the same hand.

**No monitor through a node.**  `listen(1)` and `mheardd(8)` see nothing: the
service protocol has no stream that carries a copy of every frame, the way the
AGWPE `K` record does.  Adding one is a protocol question, not a library one.

**The silent disconnects during a login.**  Sessions are sometimes dropped
without a word while `axspawn` is asking for a password.  The libax25 side of
it was found and fixed — a trace line split across two writes left the caller
without an address — and it did not stop the reports.  The remaining suspicion
is on the node's side of the same channel: `axserver.c` checks `sendmsg()` only
for `< 0` and takes a partial write for success, and a send error there drops
the whole client with everything it holds.  Belongs in the WAMPES tree, noted
here because the symptom shows up on this one.

**`AXSOCK_BACKEND` and `WAMPES_SOCKET` should go.**  They are overrides that
nothing needs any more: the files decide.  Useful for trying something out and
harmless while this is young, but an override that outlives its reason turns
into a way of configuring things twice, and then into a bug report about the
file being ignored.

---

## Measurements outstanding

**Does kernel AX.25 deliver a copy to every bound socket?**  Three processes
bound the same callsign with the same pid on a machine with the kernel stack
and none was refused, so the kernel is looser than we are.  Whether all of them
would then have *received* an incoming frame is untested, and it decides
whether the difference matters: an APRS listener beside an operating program on
one callsign works there and does not here.

It needs a frame from another station — a machine's own transmissions do not
reach its own datagram sockets, not even the digipeated repeat, although
`listen(1)` shows both.

---

## Deliberately not built

Written down so that the next person knows these were considered.

**Self-registering listeners.**  A listener is administrative: the sysop opens
a callsign, a program claims it.  A protocol where `bind()` and `listen()`
register it instead is imaginable; the questions it would have to answer are in
`AX25-WITHOUT-KERNEL-DEVELOPER.md`.

**Asking who is speaking.**  The service socket carries no identity, and a unix
socket could be asked — `SO_PEERCRED`, `getpeereid`, `LOCAL_PEERCRED`.  With
that, what a program may do could be graded rather than being all-or-nothing at
the socket's mode.  Same file, next section.

**A non-blocking `connect()`.**  Not offered rather than missing.  Answering
`EINPROGRESS` honestly would mean making the descriptor writable exactly when
the link comes up, which would mean intercepting `poll()` and `select()` —
putting the library back on the data path, which is the one thing this design
exists to avoid.  Every program in the suite that sets `O_NONBLOCK` sets it
after connecting, for its I/O loop, and that works today.
