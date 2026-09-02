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

It was not only about branches, either.  The two backends should **look and
behave alike**, and the work of making them do so is mostly done: every entry
point asks both the same way, the AGWPE half has its own file and a named
surface, both carry the protocol id, both answer the same error number for the
same refusal, both say the same thing about an option they cannot honour, and
a datagram socket can receive on either.  What is left of the asymmetry is
this item: one backend is still chosen by a probe once per process and the
other by a file per port.

The reason for caring was never tidiness.  An asymmetry like that reads as if
AGWPE were an abandoned corner, which it is not — it is the way to a
`direwolf` or to AGWPE clients on the LAN — and a corner nobody looks at is
where the faults of the last few days had been sitting, some since the first
commit.

The acceptance step that went with it — **the direwolf tests want repeating** —
is done.  Against a real direwolf, fed audio over UDP so a client can be
connected before anything arrives, all four frame kinds leave correctly:

  connect            'C'   DL9SAU-9 > DB0AAA
  connect via        'v'   ndigis 2, DB0BBB and DB0CCC
  connect with pid   'c'   what direwolf calls a non-standard connection
  unproto via        'V'   pid 0xf0, two digipeaters, payload intact

and a UI frame is received through the monitor stream, which is the half that
did not exist when those tests were last run.

What has to be answered first — descriptors that are never bound:

* `axctl(8)` and `axkill(8)` open an `AF_AX25` socket and issue
  `ioctl(SIOCAX25CTLCON)` immediately, with no `bind()` in between
* a program may `connect()` without binding, and then no port is named at all

So the rule cannot be "decide at `bind()`".  What settles it is cheaper than a
new rule: **the placeholder is the default backend's real socket**, not an
empty one.  `socket()` then behaves exactly as it does today, an `ioctl` before
any `bind()` finds the same thing it finds now, and `bind()` is where a
descriptor can still change hands.  The question does not have to be answered,
it disappears.

Decided while planning this, so that the next reading does not reopen it:

* **`agwpe.conf` becomes the register, the way `wampes.conf` already is.**  A
  port belongs to AGWPE when its name is in that file.  The `agwpe-` prefix
  stays a naming convention — it is decoration today too, stripped at one
  place before the name is matched against the upstream, and it never decided
  anything.
* **`AXSOCK_BACKEND` goes.**  It exists to override a per-process choice that
  is sometimes wrong; once the files decide per port there is nothing left to
  override but "force everything to X", which is a test switch.

  What that costs is already known, because `wampes_enabled()` answers the
  variable and nothing else: delete it and the function is constantly false,
  and its one remaining caller — the WAMPES arm of the chooser in `socket()` —
  is dead code.  That is the right outcome and not a loss.  `socket()` builds
  the default backend, `bind()` hands the descriptor over when the port turns
  out to belong to a node, and that path is the one every program takes today
  anyway.  The arm only ever ran under the variable.

  It had a second caller until the monitor socket was fixed, and that one was
  a real fault: the quiet `SOCK_PACKET` socket was unreachable without the
  variable set, so `listen -a` died on a node-only machine.  Worth
  remembering as the shape of the thing — an environment override standing in
  for a question about the configuration answers correctly only in the test.
* **The kernel stays "not ours".**  Making it a third backend of the same
  shape would read better and would put every call of every AX.25 program on a
  kernel machine through our lookup — the configuration most people run and
  the one we can test least.  The symmetry is worth having between the two
  backends we actually serve.  If the kernel stack disappears for good, that
  is the moment to make it the third one.

And the acceptance it has to pass, because the last rebuild found its fault
only at the end: **three cases at run time** — kernel only, both, userspace
only — **and both build variants**, with and without
`--enable-userspace-ax25`.

One of them is done: after the file split, Linux builds and carries a
connection in each direction.  That is the branch this desk cannot exercise —
there the interception is strong symbols rather than an interpose table, a
different mechanism through the same new boundary — and it is where the split
had left two pointer definitions cut in half, invisible to every build here.
The rest of the matrix is still owed.

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

**But the service protocol has to be able to say it**, and today it cannot.
`connect hfb:DL1AAA via DB0BBB,DB0CCC < DL1TST-1` and the counted form of a
datagram both carry a path and no marks, so even once the library knows which
hops are done there is no room on the wire to pass that on.  The mark belongs
where an operator already writes it - `DB0BBB*` in the path - which is the
notation `setcall()` refuses today and would then have to read.  Both
directions want it: what we send, and what the node reports on an incoming
frame.  So this is three pieces, not two - the library, the grammar on
`sockets/ax25`, and the node's own handling - and the grammar is the one that
has to be decided first, because the other two write against it.

That gives back something the old world had: a station can emit a connect as
though the first hops had already happened — which is what a node does when it
inserts itself into a path.  Worth saying out loud that on a shared channel
such a frame is indistinguishable from a real digipeat, so it is a tool and a
footgun in the same hand.

**`AX25_IAMDIGI`, to come back to.**  It makes a socket repeat what it hears —
the socket becomes a digipeater — and `rsdwnlnk(8)` sets it.  It is refused
with `ENOPROTOOPT` today rather than accepted and ignored, which stops a
program that needs it instead of letting it run wrong, but that is a holding
answer and not a decision.

What it would take is not obviously large.  On the node side a repeated frame
is what `nextdigi` and the has-been-repeated bit already describe, so this and
the `*` item further up are the same piece of work seen from two ends.  On the
AGWPE side a frame to repeat is one the server heard and we send back out with
one more hop marked — the monitor stream has the frames and `sendto()` can
carry a path, so the parts exist.  What wants deciding first is whether a
userland socket should be able to digipeat at all without the sysop saying so,
which is the access-control question further down.

**What is left of the protocol id on the AGWPE side.**  Most of it is done —
`socket()` carries the pid through, frames go out with it, the inbound match
prefers a listener that claims it, `listen()` refuses a second one on the same
callsign and pid, and a connect spelled `c` is recognised.  Two things remain,
and both are outside this file.

**Two processes cannot divide one callsign by pid.**  AGWPE registers a
callsign with `X`, and `X` carries no pid, so the server hands its one owner
everything addressed to it.  Sorting therefore happens in the client, which
can only sort what reaches it.  The node backend has no such limit: its
`listen ax25` entries are per pid and the node splits the link into one stream
per pid before anything reaches us — which is the better division of labour
and the reason not to build a multiplexer here.  Giving AGWPE the same would
mean a pid on the registration, which is a change to `ax25netd(8)` and to what
it can claim to be.

**And a question for the node, not for here:** whether `listen ax25 <call>
client pid=any` would be worth having.  It would save a sysop an entry per pid
and let one program take everything on a callsign.  What speaks against it is
that the session is handed over as a raw descriptor: with several pids on one
stream the client cannot tell which frame was which, because the pid is not in
the bytes.  It would want the counted form, the way the datagram path already
works — so it is not one line in `axlisten_find()` but a different kind of
client.  Worth deciding in the WAMPES tree before it is built here.

**`getsockopt(SOL_AX25)` still answers two different things.**  `setsockopt()`
is aligned — both backends accept a channel parameter, both say once per option
that it went nowhere, and both refuse the two that change what the bytes mean.
Reading one back was not touched: the AGWPE path answers success with a zeroed
value and the node path falls through to the real call and fails.  Nothing in
the suite reads one back, which is why it has not bitten, and also why fixing
it is cheap: pick one answer.  Zeroes-and-success is the friendlier lie and
`ENOPROTOOPT` is the true one; a value kept from the `setsockopt` would be
better than either, and is the most work.

The parameters themselves stay where they are.  Neither backend carries them —
the AGWPE connect frames have no field and the node's `connect` line has no
room — and the far side would ignore them anyway, taking its window and timers
from its own interface configuration.  There is a mechanism on the AGWPE side
that looks like the answer and is not: `AGWPE_CTL_PARAM_WINDOW` reaches
`ax25netd`, which stores it in `s->window`, never reads it again and does not
pass it upstream.

**UI reception off the monitor stream: built and measured.**
It works through `ax25netd(8)` both ways now — the direct route on its loop
port, and the monitor stream on every other port, which is the only way a real
AGWPE server has and is what a monitor channel is for.  Measured on a radio
port across two processes, byte-identical for the same fourteen payloads that
torture the loop path.

**Measured against direwolf, and the answer settles the echo filter.**
direwolf does **not** mirror a client's own transmission into the raw monitor
stream - zero `K` frames come back for a UI frame sent through it, where
`ax25netd` mirrors one at once.  So the two behave oppositely, and the
has-been-repeated condition is not a belt-and-braces on the AGWPE path but the
thing that carries it: with no mirror the echo entry stays unused, and the
first frame to match it is the digipeat.

Driven end to end, over real audio, on the two cases that differ only in the
mark:

  same payload, no mark   suppressed as our own echo
  same payload, DB0XYZ*   delivered, sender named

which is what it is for.  Without the condition the second would have gone in
the bin - and it is the frame that proves the hop happened.

The rig is two things direwolf already ships plus one script: `gen_packets`
turns TNC2 lines into audio and keeps the `*`, and `ADEVICE udp:7355` lets the
audio in whenever we like rather than only at startup, where no client is
connected yet.  `dwaudio.py` in the test tools is the feeder.  No patch, and
the frame stays a received one - through modulator and demodulator - rather
than something handed in at the side.

**What must not be filtered out.**  The monitor copy of our own transmission
has to be suppressed for a datagram socket — the kernel does not deliver a
station its own frames — but only that, and the temptation is to cast the net
too wide.  Two cases that must come through:

* **A digipeated repeat.**  Send `A>APRS,WIDE1-1`, a repeater sends it on as
  `A>APRS,DB0XYZ*`: same source callsign, different frame, and the one the
  operator most wants to see - it is the proof the digipeat happened.
  Filtering by source callsign would swallow it.
* **The same frame heard on another port.**  Transmitted on port 1 and heard
  back on port 2, for whatever reason, is real information about the network
  and not an echo.

So the test is all three at once: the identical frame, with no
has-been-repeated bit set, on the port it was sent on.  Anything else is
somebody's traffic, including when it started as ours.

**`bind()` waits on the node without a bound.**  A datagram `bind()` claims
the callsign for incoming UI frames, and the wait for the node's answer has no
deadline — none of the service conversations do, except the descriptor
handover.  Against a running node this is invisible: it is a unix socket on
the same machine and the answer comes back at once, refusal included.  Against
a node that has stopped answering, a program that only ever wanted to *send*
hangs in `bind()`.  The remedy is the one the handover already uses, a
deadline and a recorded error, with sending left working; what wants deciding
first is whether a claim that timed out should be retried later or stay
failed for the life of the socket.

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

**A radio link on the desk, for the tests that need two stations.**  Two
direwolf instances on the same virtual audio device - BlackHole on this
machine - hear each other, which gives a real modulated 1200 baud channel
without a radio.  `/tmp/dwA.conf` and `/tmp/dwB.conf`, `AGWPORT` 8000 and 8010,
both `ADEVICE "BlackHole 2ch"`.

What it has already shown: a complete AX.25 connection through the library
over the air, v2.2 with XID negotiation, data in both directions and a clean
DISC/UA.  That had never been done - every earlier direwolf test checked which
frames went *out*, because there was nobody to answer them.

Two things to know before believing a result from it.  The two instances share
one channel, so they collide with each other exactly as two stations on one
frequency do; a frame that does not arrive may be a collision rather than a
fault.  And a test must be patient - `axprobe -w` exists for that - because a
listener that gives up unregisters its callsign, and the call then arrives at
a station that is no longer there.

Still to run on it: two listeners on one callsign with different pids, which
needs an incoming connection and so could not be done against a single
direwolf; and the outbound flow control below.

**Outbound flow control on the AGWPE path.**  The node side of this was found
and fixed today by thinking it through: a `cat` of a large file fills faster
than 1200 baud drains, the far side says stop, and what the application must
get is `EAGAIN` - not a torn-down session.  The AGWPE path has not been put
through it.  The rig above is what it wants: a real link that is slow enough
for the buffers to fill in seconds rather than hours.

---

## Measurements outstanding

**Does kernel AX.25 deliver a copy to every bound datagram socket?**  Three
processes bound the same callsign with the same pid on a machine with the
kernel stack and none was refused, so the kernel is looser than we are — we
hold a callsign for one program.  Whether all three would then have *received*
an incoming UI frame is the open half, and it decides whether the difference
matters: an APRS listener beside an operating program on one callsign works
there and does not here.

The attempt to answer it stopped early, and honestly so: with a frame sent
from the same machine **none** of the three saw anything, not even one.  The
likely reason is that a machine's own transmissions do not reach its own
datagram sockets, not even the digipeated repeat, although `listen(1)` shows
both — but that was not chased down either.  It wants a frame from a second
station, addressed to the bound callsign rather than passing through it as a
digipeater.

---

## Decided against

**A non-blocking `connect()`.**  Not offered rather than missing.  Answering
`EINPROGRESS` honestly would mean making the descriptor writable exactly when
the link comes up, which would mean intercepting `poll()` and `select()` —
putting the library back on the data path, which is the one thing this design
exists to avoid.  Every program in the suite that sets `O_NONBLOCK` sets it
after connecting, for its I/O loop, and that works today.

---

## Not built yet, and in this order

Not decided against — the ideas arrived later, or the priorities were
elsewhere.  The order matters: **the identity question first, then
self-registration.**  Built the other way round, a registration protocol would
have to be reopened to let credentials into it.

**Access control from the unix side: which unix identity may use which AX.25
resource.**  Today the socket's mode is a single gate, and past it everything
is allowed — any source callsign, any configured callsign to listen for, any
pid.  A graded model would decide per resource: which callsigns a user may
call out under, which they may listen for, which pids they may speak, and
whether they may mark a hop as repeated.

Asking who is at the other end is only the means — `SO_PEERCRED` on Linux,
`getpeereid` or `LOCAL_PEERCRED` on macOS, the latter returning the whole
group list.  The policy is the work, and the sketch of one is in
`AX25-WITHOUT-KERNEL-DEVELOPER.md`.  A callsign filter is one shape it could
take; a table of unix groups against capabilities is another.

**Self-registering listeners.**  A listener is administrative: the sysop opens
a callsign, a program claims it.  A protocol where `bind()` and `listen()`
register it instead is imaginable; the questions it would have to answer are in
`AX25-WITHOUT-KERNEL-DEVELOPER.md`.

