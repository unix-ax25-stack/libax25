# The protocol between libax25 and an AX.25 node — for developers

Part two.  The sysop's part beside this file says what the arrangement does;
this one says how, in enough detail that a second node could be written to it.

That is the point of writing it down.  WAMPES is the node today, but nothing
here is particular to it: the meeting point is a **socket and a grammar in
lines**, and any program with an AX.25 stack — TheNetNode, an XNET, something
of your own — can offer the same one.  Every `libax25` program then reaches it
without a line changed.

---

## The service socket

A unix stream socket, `/tcp/sockets/ax25` in a default WAMPES installation,
named for the client side in `wampes.conf(5)`.  Mode `0660` and a group is the
whole access control; there is no login in the protocol.  The same service may
be offered over TCP on the loopback, which trades that control away.

**A stream, not a datagram socket.**  That single fact shapes everything
below: nothing on it has a boundary of its own, so every message either ends
at a newline or says its own length.  It is also the reason for two of the
faults found while building this, both described at the end.

One connection carries one thing.  A client that wants an outgoing call, a
listener and a datagram sender opens three.

---

## The commands

Each is a line.  A node answers only when it has something to say; success is
usually silence, and exactly one line begins with `***`.

### `binary` and `ascii`

```
-> binary
```

Whether the node converts line endings on this connection.  `binary` means it
does not, which is what a program wants: an AX.25 socket is what the kernel
used to give, and the kernel converted nothing.  `ascii` is for a human on a
terminal.

The default follows the *kind* of entry a session belongs to, not the
connection: a client is binary, a program started from a path or a session
dialled over TCP is ascii.  `datagram` switches its connection to binary by
itself, because a counted frame is bytes.

### `handover`

```
-> handover
```

Asks the node to answer the next `connect` with a **descriptor** rather than
letting this connection become the session.  It is sent before `connect`, and
a node that does not know the word answers nothing to it — then the connection
is the session, as it was before.  No negotiation, no version, no round trip.

**Why an outgoing call wants this at all** is not obvious, and it is not about
the direction.  The service connection was made by the *client*, so its type
is the client's: a stream.  Kernel AX.25 was `SOCK_SEQPACKET` — one `write()`
is one frame — and protocols that ride on a connection rely on it; FBB's
compressed forwarding reads the end of an uncompressed block off the frame
boundary.  A node that makes the pair itself can choose:

```c
if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0) return 1;
if (socketpair(AF_UNIX, SOCK_STREAM,    0, sv) == 0) return 0;
```

SEQPACKET where the system has it, a stream where it does not — macOS does
not offer it on a unix socket.  There the boundaries are lost, which costs
that one kind of service and nothing else.

### `connect`

```
-> connect [<iface>:]<dest>[ via <digi>[,<digi>...]][ < <src>][ --pid 0x<nn>]
<- link setup (hfb)...                     progress, may be ignored
<- *** connected to <dest>                 with a descriptor beside it
```

The interface prefix is the part after the colon in an `axports(5)` entry and
is the same one an operator types at an RMNC.  `< src` is the source callsign
from `bind()`; the node does not check it against anything.  `--pid` is the
third argument of `socket()`, which kernel AX.25 ignored and every program
passes as 0.

Every line that does not begin with `***` is progress.  Exactly one does, and
it is the answer.  End of file before it means the link never came up.

The refusals carry a reason, which is the whole point of them:

| line | errno |
|---|---|
| `*** link failure with X - no route` | `EHOSTUNREACH` |
| `*** link failure with X - busy` | `EADDRINUSE` |
| `*** link failure with X - nomem` | `ENOBUFS` |
| `*** link failure - invalid call "…"` | `EINVAL` |
| `*** no interface "X"` | `ENODEV` |
| end of file, no verdict | `ETIMEDOUT` |

### `listen`

```
-> listen [ui] [pid=<n>] <call>
<- *** listening on <call> pid 0x<nn>
```

Claims a callsign.  The sysop's configuration is the permission — a callsign
with no `client` entry cannot be claimed — and this is the claim against it.

| refusal | errno |
|---|---|
| `*** X is already taken` | `EADDRINUSE` |
| `*** X is not open for clients` | `EACCES` |
| `*** X belongs to a port` | `EADDRNOTAVAIL` |

**The key is (callsign, pid, connected-or-UI), and one client holds it.**  A
Rose daemon and a text application can therefore share a callsign; two
programs wanting the same pid cannot.  The claim lives exactly as long as the
connection it was made on, so a client that dies leaves nothing behind.

`pid=` takes names as well as numbers — `pid=netrom` and `pid=0xcf` are the
same thing — while the answer keeps the number, because a client may be
reading it.

### `datagram`

```
-> datagram [<iface>:] [--pid 0x<nn>] [--silent]
```

Puts the connection into datagram mode; the node answers only on a parse
error.  Without an interface the frames go out of every AX.25 interface,
which is what a beacon on a node-wide entry asks for.

Given a destination on the same line the header is fixed for the whole
session and every line is payload.  Without one — the shape `libax25` uses —
every frame brings its own header in **the counted form**.

---

## The counted form

Both directions, and the only shape that survives arbitrary bytes:

```
[<n>]<src>><dest>[,<digi>[*]...]:<exactly n bytes>
```

`[n]` stands at the very front, so the first byte decides and no payload can
be read as a length.  The header ends at the colon; the payload starts
straight after it and is exactly n bytes.  **No line ending closes it** — CR,
LF and NUL are ordinary content, which is the whole point.

A `*` marks an element that has already repeated the frame.  Incoming it
carries the has-been-repeated bit into the digipeater's SSID byte, where it
belongs.  Outgoing `libax25` does not write it, which is a gap and not a
decision.

Sending, one frame:

```
-> datagram hfb:
-> [17]DL9SAU-2>DL1ABC,DB0AAA:dies ist ein test
```

Receiving, after `listen ui`:

```
<- [5]DL1ABC>DB0FHN-13,DB0AAA*:hallo
```

Note what is *not* here: no length field in a header of ours, no escaping, no
framing layer.  The count is the frame boundary a stream does not have.

---

## How an incoming call becomes `accept()`

This is the part worth reading twice, because it is where the descriptor
changes hands.

1. The application calls `listen()`.  The shim opens a service connection,
   sends the claim, and — this is the trick — **that connection becomes the
   listening descriptor**.  `poll()` and `select()` on it therefore work with
   nothing of ours involved: it is readable exactly when a call is waiting.

2. Per incoming call the node makes a `socketpair`, gives one end to the same
   machinery that serves a spawned program, and sends the other with
   `SCM_RIGHTS` — together with a trace line, in **one** `sendmsg()`:

   ```
   hfa DL1TST-1,DB0BBB > DL9SAU-13
   <port> <caller>[,<path>...] > <the callsign they reached>
   ```

   Together deliberately.  A descriptor arriving on its own would have to be
   matched against a line arriving separately, and there is no key to match
   them with.

3. `accept()` is that one `recvmsg()` — or rather, it reads to the end of the
   line and collects the descriptor from whichever byte carried it.  The
   caller goes into the address the application gets; the callsign that was
   reached is answered later by `getsockname()`, which is how `ax25d` picks
   its stanza.

The node sends with `MSG_DONTWAIT`: a client that has stopped calling
`accept()` must not be able to stop a cooperatively scheduled node.

**A NET/ROM session arrives the same way**, announced in the same shape, with
the node the user sits on where a digipeater would be:

```
netrom DL1ABC-7,DL1TST-1 > DL9SAU-8
```

The shim reads two callsigns out of it and never asks which protocol carried
the call.

---

## What the shim does with a descriptor

`socket()` cannot know the port yet, and the application wants a number now.
So:

```
socket()    a placeholder — an unbound unix socket, and a real descriptor
bind()      the first moment the port is known, and therefore the moment the
            backend is chosen: kernel, AGWPE or a node, per port
connect()   the conversation above, then dup2() of the session onto the
            number the application already holds
listen()    the claim, and the connection takes the descriptor's place
```

From `connect()` onwards **nothing of ours is on the data path**.  `read()`,
`write()`, `poll()` and `close()` reach the kernel directly; that is the whole
reason for handing a descriptor over rather than relaying bytes.

A datagram socket claims its callsign at `bind()` rather than at the first
`recvfrom()`, so that `select()` is readable when a frame arrives rather than
never.  A refusal there is not an error — a beacon binds too, usually a port's
own callsign, which no client may claim — so the reason is kept and handed to
whoever calls `recvfrom()`.

### Which calls are intercepted

`socket`, `bind`, `connect`, `listen`, `accept`, `send`, `sendto`, `write`,
`recv`, `recvfrom`, `shutdown`, `close`, `setsockopt`, `getsockopt`, `ioctl`,
`getsockname`, `getpeername`.  Everything that is not AX.25 falls through to
the real call, guarded by a counter that is zero until the first AX.25 socket
exists.

`read()` is deliberately **not** among them: the descriptor is readable by
itself.  See `axsock(7)` for how the library gets in front of a program on
each platform, which differs — strong symbols on ELF, an interposing table on
macOS.

### What a child inherits

`ax25d` hands a service the connection on descriptor 0, and the child asks
`getpeername(0)` who is calling.  Over a socketpair the kernel answers
`AF_UNIX`, so the parent says who is there in one variable:

```
AXSOCK_INHERIT=<fd> <local callsign> <peer callsign>
```

The library reads it when it loads and answers for that descriptor.  No child
has to be changed.  It is removed from the environment once read.

---

## Tracing it

Four vantage points, and the trick is knowing which one answers which
question.

```
AXSOCK_DEBUG=1 <program>      every decision the library takes, and the whole
                              conversation with the node.  Start here: "the
                              library is not loaded", "this port is not a node
                              port", "the node refused us" and "the node is not
                              running" look identical from outside and
                              completely different here

strace / dtruss               whether the call even reached the library.  A
                              socket(AF_AX25, ...) that fails with
                              EAFNOSUPPORT and no unix socket afterwards means
                              nothing intercepted it

svc.py <socket> <command>     speak the service socket by hand.  The quickest
                              way to find out whether a refusal is ours or the
                              node's

fakewampes.py, fakedgram.py,  a node that is not there.  Every fault below was
fakeuirx.py                   found or confirmed with one of these
```

The fakes are worth the twenty lines they cost.  A node one can make *behave
badly on purpose* — split a line, hand over a descriptor and never finish the
line, answer a claim and then push a datagram — turns "it sometimes drops a
session" into a test that fails every time.

---

## Traps, all of them found the hard way

Written down because each one cost an evening, and because a second
implementation will meet the same ones.

**A stream does not deliver messages.**  `sendmsg()` with `MSG_DONTWAIT`
accepts *part* of a message when the buffer is nearly full and reports how
much.  A node that treats that as success has handed over a descriptor and
never finishes the line describing it; a client that assumes one `recvmsg()`
is one line hands up a half-parsed call and leaves the rest for the next
`accept()`, which then finds a line with no descriptor.  Read to the end of
the line; write all of what you started.

**End of file is not an error, and it is not nothing.**  `read()` returning 0
is how a closed session arrives when it is not a kernel socket — a socketpair
has no exception condition to report.  Code that only tests for `< 0` treats
it as a short read, writes zero bytes, and spins at 100% CPU for as long as
the process lives, because end of file stays readable.

**A count is not a line.**  Anything that carries payload needs the counted
form.  A frame with a CR in it is not exotic; it is what an ordinary AX.25
station sends.

**The socket type is part of the identity.**  When one backend hands a
descriptor to another at `bind()`, the type decided at `socket()` has to go
with it, or a datagram socket arrives looking like a connection and the send
path falls through to the descriptor itself.

**Not every program names the port the same way.**  `bind()` carries the port
in the first digipeater slot, and `call(1)` always puts it there — `beacon(8)`
only when its `-c` differs from the port's own callsign.  Look the source
callsign up as well.

**macOS has no `SOCK_SEQPACKET` for unix sockets**, and no `sa_len` room in a
Linux-shaped `sockaddr`.  Ask the system rather than assuming; take a stream
when the answer is no, and say what it costs.

---

## Not implemented: self-registering listeners

A listener is administrative here.  The sysop writes `listen ax25 add <call>
client` and a program claims it; the line is the permission and the claim is
made against it.  That was a decision and not an oversight — one file says
which callsigns may leave the node's own control, and it is a file the sysop
reads anyway.

A protocol where the program's `bind()` and `listen()` register the callsign
by themselves — the node learning what is being listened for, rather than
being told in advance — is imaginable and may well be wanted (the comparison
with UPnP is not entirely a compliment, and not entirely unfair either).  If
somebody builds it, these are the questions it has to answer:

* **Who may register, and what.**  Without the sysop's line, the socket's
  permissions are the only gate left.  Either that is the answer — whoever may
  open the socket may register anything — or the node has to ask who is
  speaking; see the next section.
* **Which callsigns.**  A program's own with any SSID, a pattern, a range, or
  anything at all.  The node's own interface callsigns must stay out of reach
  either way, as they are today.
* **The settings a static line carries.**  `pid` and connected-or-UI the
  protocol already sends, but an entry also holds an interface list,
  binary-or-ascii, silent, and wait.  A registration either carries them or
  accepts defaults, and the defaults are not the same for every kind of entry.
* **Precedence.**  A configured entry must win.  A registration that could
  shadow or replace one would turn a configuration file into a suggestion.
* **The take-over hole, said out loud.**  A service that is momentarily not
  running leaves its callsign free.  With static entries the sysop has at
  least named the set of callsigns that can happen to; with free registration
  the set is everything.  Either that is acceptable or the identity question
  has to be answered first.
* **Lifetime.**  The claim already dies with the connection, which is the
  right shape; what re-registration means — refuse, replace, or share — is not
  yet a question anybody has had to answer.
* **Refusals stay distinguishable.**  They become errnos on the other side,
  and a client that cannot tell "taken" from "not allowed" cannot report
  anything useful.

---

## Not implemented: asking who is speaking

The service socket carries no identity.  The node never asks who is at the
other end, and the file system's permissions are the whole answer — which is
simple, effective, and slightly blunt: a user who may not open the socket may
not connect out either.

A unix socket can be asked, and cheaply:

```
Linux   getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)   struct ucred: pid, uid, gid
macOS   getpeereid(fd, &uid, &gid)
        getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, ...) struct xucred: uid and the
                                                       full group list — measured,
                                                       16 groups came back
```

On Linux only the primary group arrives, so "is this user in group hams"
needs `getpwuid()` and `getgrouplist()` behind it.  The credentials are fixed
when the connection is made, so a process cannot change them afterwards.  Over
TCP there are none at all — that path's only protection is that it binds to
the loopback.

With that, what a program may do could be graded, which is a different
question from who may register a listener:

| | connect out as | listen for |
|---|---|---|
| a daemon's own uid (conversd) | its callsign | its callsign |
| group `hams` | own callsign, any SSID | perhaps nothing |
| group `hamsoft` | anything | anything |
| anybody else | nothing | nothing |

Worth writing down because today both halves are ungraded, and in opposite
directions: **any** source callsign may be used on an outgoing call — only its
spelling is checked — while a listener may only have the callsigns the sysop
opened.

Kernel AX.25 offers the mirror image of the first half, and it is worth being
exact about it, because it is *offered* rather than *done*.  `ax25_bind()`:

```c
user = ax25_findbyuid(current_euid());
if (user) call = user->call;              /* REPLACES what he asked for */
else if (ax25_uid_policy && !capable(CAP_NET_ADMIN))
        return -EACCES;
else call = addr->fsa_ax25.sax25_call;    /* he may call himself anything */
```

So the association bites only for users who **have** an entry, and
`ax25_uid_policy` is **0** by default (`AX25_NOUID_DEFAULT`).  Without
`axparms --assoc policy deny`, any local user binds any callsign — the kernel
does not grade outgoing calls, it ships a switch for grading them and leaves
it off.  Where it does bite it is one uid to exactly one callsign: no SSID
range, no groups, no difference between connecting out and listening.  And it
**replaces silently** rather than refusing, which is the one part not worth
copying: a program that believes it is DL9SAU-7 and is quietly made into
something else has no way to notice.

Listening needed nobody's permission there at all.  So on that half we are
already stricter than the kernel's default, and that is an argument for the
administrative model above rather than against it.  Neither shape is wrong;
ours simply grew rather than being chosen, and a node that asked for
credentials could choose.

---

## What a second node would have to implement

Shorter than it looks:

* a unix stream socket, and its permissions
* `binary`, `handover`, `connect`, `listen`, `datagram` as above — five words
* one `***` line per command, with the reasons spelled out, because that is
  what becomes an errno on the other side
* `SCM_RIGHTS` with the line and the descriptor in one `sendmsg()`
* the counted form for datagrams, both ways
* a table of claims keyed by callsign, pid and connected-or-UI

Everything else — timers, retries, routing, the channel — is the node's own
business and none of ours.  That is the division of labour this whole
arrangement exists to draw.
