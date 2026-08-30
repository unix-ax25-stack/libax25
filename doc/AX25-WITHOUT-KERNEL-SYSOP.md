# AX.25 without a kernel stack — for the sysop

The AX.25 stack leaves the Linux kernel in 7.1.  Everything written against
`libax25` — `call`, `ax25d`, `axspawn`, mailboxes, DX clusters, convers
servers — expects `socket(AF_AX25, …)` to work, and on a machine without the
kernel stack nothing answers it.

This is one way out: **`libax25` answers the socket calls itself and hands the
work to a program that has a complete AX.25 stack in user space.**  Today that
program is WAMPES.  The applications do not know the difference; nothing is
recompiled.

It is a **proof of concept**, and it is not a paper one: it carries real
traffic on **DB0FHN-10**.  Come and look, connect to it, break it.  Feedback
and bug reports are explicitly wanted — several of the faults fixed so far
were found exactly that way, by somebody logging in and something behaving
oddly.

It is also meant as a **reference implementation** rather than a private
arrangement.  What sits between `libax25` and WAMPES is a small protocol on a
unix socket, written down in AX25-WITHOUT-KERNEL-DEVELOPER.md beside this
file.  Any other
AX.25-capable program can learn it — TheNetNode, an XNET, something of your
own — and every `libax25` program then reaches it without a line changed.  The
kernel stack was the meeting point for thirty years; this is a proposal for
the next one.

AGWPE, which `libax25` also speaks, is a different subject and is not
described here: it is the way to a `direwolf` or to AGWPE clients on the LAN,
and its configuration files have no bearing on what follows.

---

## What it looks like

```
   call, ax25d/axspawn, conversd, a mailbox
                  |
                  |  socket(AF_AX25, …), bind, connect, listen, accept
                  v
              libax25                      one unix socket
                  |  ------------------------------------------->  WAMPES
                  |  <-------------------------------------------
                  |     a descriptor per session, handed over
                  v
        the session is the descriptor — libax25 is out of the way
```

Two things follow from the picture, and they are the whole reason for it:

* **After the connection stands, nothing of ours is in the path.**  The
  descriptor the application holds *is* the session.  `read()`, `write()`,
  `poll()` and `close()` go to the kernel and nowhere else.
* **The node does the AX.25.**  Timers, retries, digipeating, routing, the
  channel — all of it stays where it already worked.

---

## What you configure

**Outgoing connections need nothing beyond two files.**  A program that dials
out — `call`, a forwarding mailbox, a cluster fetching a feed — works as soon
as `axports` and `wampes.conf` agree.  Being *called* is the third step, and
it is optional: only a station that answers needs it.

### 1. `axports`: which interface a program leaves by

```
# name        callsign   baud  paclen  window  description
wampes        DB0FHN-10  9600  256     2       WAMPES node
wampes:xnet   DB0FHN-2   9600  256     2       node wampes, interface xnet
```

The part before the colon says **which node**; it is resolved on the Linux
side and never travels.  The part after it names **one of the node's own AX.25
interfaces**, and travels as the prefix WAMPES already understands
(`connect xnet:DB0AAA`) — the same one an operator types at an RMNC or XNET.

Both shapes may stand side by side, as above: `axports` refuses duplicate
names and duplicate callsigns, and these differ in both.  An entry without a
suffix leaves the choice of interface to the node, which routes the call; one
with a suffix pins it.

One entry per interface rather than per node, because every interface has a
callsign of its own anyway.  That callsign is what an outgoing connection uses
as its source unless the program binds one itself — `call -s` does.

### 2. `wampes.conf`: where the node listens

```
# name    address                  description
wampes    /tcp/sockets/ax25        the node on this machine
```

That is also what makes an entry a WAMPES entry: a port belongs to a node when
the name before the colon appears here.  Nothing is guessed from spelling, no
name is reserved, and a node reachable over TCP is written `host:port`
instead.

**With those two, outgoing works.**  There is no daemon of ours to start, no
port to open, no state to keep in step.

### 3. Optional: being called

To let a `libax25` program answer for a callsign, the node has to be told that
this callsign may be handed out.  In WAMPES' `net.rc`, one line each:

```
listen ax25 add db0fhn-9  client        # ax25d/axspawn: the login
listen ax25 add db0fhn-11 client        # conversd
```

`client` is the word that matters: it means *hand the session to whoever asks
for this callsign over the service socket*, as opposed to running a program or
dialling a TCP port.  A callsign with no such line cannot be claimed at all —
that is the access rule in one sentence.

These lines are about **connected sessions**.  UI frames are a listener of
their own, and a program that wants to receive them needs one:

```
listen ax25 add ui db0fhn-13 client     # UI frames for that callsign
```

The two may stand side by side for the same callsign — one entry for
connections, one for datagrams.

**An entry is a callsign, a pid and connected-or-not, and one program holds
it.**  That is the whole rule, and two things follow from it.  A second
program asking for a callsign somebody already holds is refused with
`*** DB0FHN-13 is already taken`; it does not quietly get a second copy of
the traffic.  And because the pid is part of the key, entries that differ in
it are different entries:

```
listen ax25 add ui pid=text db0fhn-13 client    # one program
listen ax25 add ui pid=rose db0fhn-13 client    # another, at the same time
```

Each frame goes to the entry whose pid it carries.  So a Rose daemon and a
text application can share a callsign without knowing about each other, while
two programs wanting the *same* pid cannot.

Writing `add` again with the same callsign, pid and kind does not create a
second entry; it replaces the first.

**This is stricter than the kernel was**, and a program that relied on the old
behaviour will notice.  Measured on a machine with the kernel stack: three
processes bound the same callsign with the same pid, one after another, and
none of them was refused — kernel AX.25 treats an AX.25 datagram socket the
way it treats a raw IP protocol, not the way it treats a UDP port.  Whether
all of them would then have received a copy of an incoming frame is untested,
and it is the question that decides whether this difference matters in
practice — it needs a frame from another station, since the kernel does not
hand a machine's own transmissions to its own datagram sockets, not even the
digipeated repeat.  Here, one program holds a callsign and gets the frames; a second is
told so rather than being left to wonder.

---

## Beside the kernel stack, not instead of it

Nothing here asks a station to switch over.  **The choice is made per port**,
and a machine that still has a working kernel AX.25 setup can add one node
port and leave everything else exactly as it is.

It works out that way because of when the decision can be taken.  `socket()`
has to hand back a descriptor before anybody knows which port is meant — the
callsign, and with it the port, arrives at `bind()`.  So that is where the
question is asked: the callsign is looked up in `axports(5)`, and if the entry
belongs to a node named in `wampes.conf(5)`, the socket changes hands there
and then.  Anything else stays with the kernel.

For one program that means:

```
call -r kernelport DL1ABC        goes to the kernel stack
call -r wampes:xnet DL1ABC       goes to the node
```

in the same process, one after the other, with nothing set and nothing
switched.  A mailbox can be moved one callsign at a time, and moved back the
same way, which is a comfortable position to test from — there is no flag day
and no rollback plan to write.

One limit, said plainly: **AGWPE is not yet part of this.**  Whether a process
uses the kernel or an AGWPE server is still decided once, at its first AX.25
socket, and applies to all of them.  Only node ports are chosen per port.  If
you serve both, keep them in separate programs for now.

---

## Who may use it

**The file system is the whole access control.**  There is no login on the
service socket, no password, no per-user configuration — whoever can open
`/tcp/sockets/ax25` may use the node.

That is deliberate, and it is why the socket is where it is: a unix socket
carries its rights in the file system, which is a tool you already know.

**Out of the box the gate is the DIRECTORY, not the socket:**

```
drwxr-x---  2 root staff  /tcp/sockets          0750, and this is the gate
srw-rw-rw-  1 root staff  /tcp/sockets/ax25     0666, deliberately open
```

That looks backwards until you see which one decides.  The directory carries
the group, so one `chgrp hams /tcp/sockets` moves the whole service from one
group to another and no socket mode has to be reasoned about.  (The socket is
`0666` and not `0777` because `connect()` on a unix socket asks for **write**
and never for execute — measured, and Linux says so in `af_unix.c`.)

If you would rather state the terms on the socket itself, `net.rc` runs after
it exists:

```
axsock group hams
axsock mode 0660          →  srw-rw----  1 root hams  /tcp/sockets/ax25
```

Then members of the group may use the transmitter and nobody else can even
connect.  Finer grain is the file system's business, not ours — a group per
daemon, an ACL for one account, whatever your machine already does.

**Say what that means before you widen it.**  Somebody who can open the socket
can:

* claim any callsign the sysop configured with `client`, if no program is
  holding it at that moment.  First come, first served — so if `ax25d` is not
  running, another program can take `DB0FHN-9` and answer the logins itself.
* connect out under **any** source callsign.  `< SRC` is not checked against
  anything; the socket's mode is what says who may transmit.

What that person **cannot** do:

* take a callsign somebody already holds — the node answers
  `*** DB0FHN-9 is already taken` and hands out nothing.
* claim a callsign nobody configured (`is not open for clients`) or one that
  belongs to a port (`belongs to a port`).
* see or interfere with a session already handed to somebody else.  Each
  session is a socket pair of its own, one end passed to the one client that
  claimed the callsign.  There is no way to ask for a copy, and no monitor
  stream to listen on.

In short: **the boundary is a file system one, and it is real — but everything
inside it is trusted.**  Treat membership in that group as you would treat the
right to use the transmitter, because that is what it is.

The node can also offer the same service over TCP (`axsock tcp-listen on`,
127.0.0.1 and ::1 only; `axsock` alone shows whether it is on).  That switch
removes the file system from the picture: every account on the machine can
then reach it, and a TCP connection carries no credentials to grade it by.
Turn it on when that is what you mean.  It is off unless `net.rc` says
otherwise.

---

## The switches on a listener

`listen ax25 add` takes options, and the defaults are chosen so that the
common cases need none.  `listen ?` at the node prints the full list.

| | what it does | default for `client` |
|---|---|---|
| `--binary` / `--ascii` | whether the node converts line endings | **binary**, and that is what you want |
| `--silent` / `--verbose` | whether the node announces the call into the session | **silent, and forced** |
| `--wait` | do not start the service when the link comes up; wait until the caller sends something | off |

`--wait` has more use than it looks.  Without it the service starts the moment
the link stands, which is what a login prompt wants — but not always.  An
incoming SABM may be meant for a protocol with a different pid (a userspace
Rose daemon, say), and a plain text login can be one where the caller decides
when to be logged in, by sending an empty line.  In both cases the greeting
would arrive before anybody asked for it.

Two of those deserve a sentence, because guessing wrong is quiet:

**Binary is the default for `client` entries and should stay.**  The far end is
a `libax25` program, which speaks the packet radio convention itself — a
station's CR is a CR all the way through.  Converting for it corrupts without
saying so.  The other kinds of entry default the other way: a program started
from `/path/program` or a session dialled to `tcp:host:port` gets ascii,
because an ordinary unix program wants LF.

**`--verbose` has no effect on a `client` entry.**  The node forces silence
there, and for a good reason: the client is told about the call in the *same*
message that carries the descriptor, so an announcement sent into the session
as well would be the first thing the far end read as data.  A program that
wants to know who called reads it from the handover line — it gets the calling
station and the callsign that was reached — and it learns that the session
ended from end of file on its descriptor.  So a DX cluster does get both
events; it just does not get them as text in the stream.

---

## Programs that were never linked against libax25

A program that opens `AF_AX25` itself, without ever calling a `libax25`
function, can still be served: load the library before the C library and it
answers the socket calls.  Nothing is recompiled.

```
LD_PRELOAD=/usr/lib/libax25.so.0 program
```

`README-linux-systemd.txt` has the worked example — `conversd` from
[conversd-saupp](https://github.com/dl9sau/conversd-saupp), with its unit file
and the two things that are easy to get wrong.  `axsock(7)` has the full
story, including what preloading cannot reach.

---

## Datagrams

UI frames go both ways.  `beacon(8)` and anything else that sends them works:
the frame is passed to the node with its path intact and leaves the interface
as if the node had sent it there itself — a bridge, not a router.  A program
that binds a callsign the sysop opened with a `ui` entry receives them, and
`recvfrom()` fills in who sent the frame and by what path.

What ends a frame is a count, not a line ending, so CR, LF and NUL inside the
payload are content and arrive as they were sent.

Receiving needs the `ui` entry above, and the pid decides which program gets
which frame.  Sending needs no entry at all: any program that may open the
socket may send, under whatever callsign it binds.

---

## What is not there

Say these out loud, because each of them is quiet rather than loud:

* **No monitor.**  `listen(1)`, `mheardd(8)` and anything else that watches raw
  frames see nothing through a WAMPES port.  The service protocol has no
  monitor stream — nothing sends a copy of every frame back the way AGWPE
  does.  The socket is handed out and stays quiet, with one line on standard
  error saying so.  The node traces on its own console; that is where to look.
* **The has-been-repeated bit travels one way only.**  A frame that arrives
  brings its path with the `*` intact — the mark becomes the bit in the
  digipeater's SSID byte, as it is on the air.  Going out it is dropped: the
  path is written from the address as it stands, and nothing carries the mark.
  There are uses for it — forwarding a frame, or recording that the first hop
  has already happened — it is simply not done yet.
* **End to end, no digipeating of our own.**  What `libax25` offers is a
  session between two stations.  It is not a path to a TNC hanging off the
  side, and it does not repeat for anybody.

---

## When it does not work

In this order, because each step rules out the one before:

```
AXSOCK_DEBUG=1 <program>        which backend answered, and the whole
                                conversation with the node

ax25d -l                        connection log to syslog.  Without it, a call
                                that matches no entry in ax25d.conf is closed
                                without a word

the node's own trace            what it thought of the call
```

The first is the one to reach for.  "The library is not loaded", "the port is
not a WAMPES port", "the node refused us" and "the node is not running" look
identical from the outside and completely different in that output.

Two traps that have cost real evenings:

* **`--enable-userspace-ax25`.**  On Linux the interception is *off* by
  default.  A library built without it contains none of this, and preloading
  it does nothing at all.  See `README-hints-for-non-kernel-AX25-hosts.txt`.
* **Re-typing the `configure` line.**  `config.status` remembers the arguments
  and `make` re-runs it by itself; typing the line again from a README is how
  the wrong prefix — or a missing `--enable-userspace-ax25` — gets in.

---

## Status, and what would help

Proof of concept, in real service on DB0FHN-10, developed in the open.  The
faults found so far were found by using it: a login that hung up without a
word, a beacon that reported success and went nowhere, a service that spun at
100% CPU after the user left.  All three were old faults that the kernel stack
had covered up, and all three were found by somebody noticing that something
was odd.

So: connect, log in, run something real against it, and say what happened.
The configuration running on DB0FHN-10 is the one described above — there is
nothing else behind the curtain.
