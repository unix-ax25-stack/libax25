# WAMPES-Sessions: in ax25netd sichtbar & killbar — Design-Notiz

Stand: 2026-10-13.  Kanonische Ablage in `documentation/` (wie
`david-netd-mail3-and-mapping.md`).

Bezug: `david-netd-mail3-and-mapping.md`, Abschnitt
„WAMPES-Socket: Sessions in netd sichtbar & killbar", dort die offenen
Design-Fragen.

## 0 Zweck

Der Punkt steht seit Tagen auf der Liste, und die Plan-Fassung sagt:
„Nächster Schritt vor Code: Design-Notiz + wampes-Unix-Socket-Protokoll."
Diese Notiz sammelt, was aus den Quellen folgt, was Thomas entschieden hat,
und was noch offen ist.

## Kurz-Entschieden (2026-10-13/14)

* **Kein AGWPE-Server** in wampes (Thomas: „wampes bekommt nach bisheriger
  Planung keinen agwpe-server").  Option 4b ist damit vom Tisch.
- **Frame-Ebene neu**: Gewählt ist die *Shim-Synthese* am Handover — kein
  Wampes-Eingriff für Frames.  Ehrliche Frame-Statistik (FRMR, N(S)/N(R),
  Segmentierung) bleibt damit offen; siehe 4c/4e und die Zählwert-Warnung.
- **Abbruch statt Thread:** Der saubere Hebel ist, den Node schließen zu
  lassen (Session = `socketpair`, Node-Ende via `axpipe`, App-Ende per EOF).
  Dann braucht es *keinen* Helfer-Thread im Shim.  Das legt eine kleine
  WAMPES-Session-Außenschnittstelle nahe (`sessions` / `close`) — siehe 4d.
- **Kill-Weg (Thomas, 2026-10-14):** `axkill`, `axctl kill` und
  `ax25netctl -k` machen **denselben** `socket(AF_AX25)` + `SIOCAX25CTLCON`,
  und die alten Tools sind genau dafür getestet.  Bedient wird dieser ioctl
  bisher nur vom AGWPE-Backend (`libax25/agwpe_sock.c`:4461 → `'Q'` KILL);
  das WAMPES-Backend sagt ausdrücklich „the node has no equivalent"
  (`agwpe_sock.c`:4392).  Also wird der ioctl **im WAMPES-Backend**
  (`wampes.c`) bedient und auf `sessions` + `close` abgebildet — symmetrisch,
  kein neues Tool, kein Sonderweg in `ax25netctl`.
- **Restart:** Ein reiner Push vom Shim nach netd ist nach einem netd-Restart

  weg (eine idle App registriert sich nicht neu).  Die *Autorität* über die
  Sessions ist WAMPES → 4d.

## 1 Zwei Ebenen — warum das getrennt gehört

- **Session-Ebene** „Wer ist mit wem verbunden": die libax25-Brücke
  (`wampes.c`) fährt `connect`/`listen`/`accept`/`close` selbst und kennt sie.
- **Frame-Ebene** „Was ging über die Luft": nur WAMPES (LAPB).

`libax25/doc/MONITOR-255-BRIDGE.md` sagt das selbst:

> A listen-capable KISS frame cannot be built from what the library side
> sees. […] for connected sessions it needs a source inside the wampes
> kernel — a monitor export at its LAPB/AX.25 layer, something like a
> wampes-internal `listen`.  A front-end on the libax25↔wampes session can
> never deliver the picture of a connected link.

Nach `connect()` ist der Descriptor die Verbindung: `read()`/`write()`/`poll()`
laufen am Shim vorbei, und es kreuzt nur I-Frame-Payload die Brücke.  Die
Frame-Ebene ist shim-seitig nur **näherungsweise** zu haben.

## 2 Ist-Stand (belegt)

### 2.1 libax25 `wampes.c` — die Brücke

* je Socket ein `struct wampes_sock` (`fd`, `local`, `port`, `pid`,
  `connected`/`listening`/`dgram`, `ctl`, `me`/`him`).
* Eintrag **bleibt** nach `connect()`/`listen()` in `Socks`; `close`/
  `setsockopt` werden weiter beantwortet, `read`/`write`/`poll` nicht mehr.
  ⇒ die Brücke verfolgt eine Session bis `close()`.
* `wampes_connect()`: `binary` + `handover` + `connect …`; Fd via
  `SCM_RIGHTS`; `s->connected`/`me`/`him`.
* `wampes_listen()`: `listen <call> pid=0x..`; `s->listening`.
* `wampes_accept()`: Zeile `"<port> <src>[,<digi>…] > <dst>"` — Empfänger
  und **via-Pfad** sind hier schon da (wird für (F) gebraucht).
* Der **Spiegel** ist ein prozessweiter, lazy AGWPE-Client auf netd Port 255
  (`wampes_mirror_*`), der nur `'K'` schreibt; heute nur UI.
* Nach `connect()`/`listen()` hält die Bibliothek den Fd.  Der Node hält das
  andere Ende.

### 2.2 WAMPES-Service-Socket (`remote_net.c`, `service_table`)

Genau: `ascii`, `binary`, `connect`, `datagram`, `handover`, `listen` — **kein
Auflisten, kein Abbruch**.  (`full_table` des Command-Sockets zusätzlich
`command`/`console`, root.)

Aber Session-Verwaltung intern: `struct ax25_cb` (globale Liste `Ax25_cb`),
`del_ax25`, `disc_ax25`, `kick_ax25`; `struct axservice` `close_axservice()`.
Eine übergebene Session hängt an `struct axpipe`: das Paar ist ein
**`socketpair`** (`axserv_handover` → `sv[0]` im Node via `axpipe_new`,
`sv[1]` an den Client).

* Client schließt `sv[1]` → `axpipe_readable()` EOF → `axpipe_hangup()` →
  `close_axservice()`.
* Node schließt die Session/`axpipe` → `sv[1]` EOF beim Client.
* `kick_ax25`/`disc_ax25` existieren fürs Node-Innere.

⇒ Es gibt **beide** Richtungen schon; was fehlt, ist ein *Kommando*, das eine
Session benennt, auflistet und schließt.

### 2.3 ax25netd

* `struct ax25netd_session` (from/to/pid/chan/fd/connecting/id) in
  `ax25netd_upstream` (Radio) bzw. Loop.
* `'Q'`: `KILL`, `PARAM`, `MONMASK`, `UISUB`, `SESSIONS('S')`, `KILLID('I')`.
* `'S'` → Zeilen `id port up chan from to pid STATE owner prog`; seit
  2026-10-15 zusätzlich `from` = Rufender, `to` = Angerufener (nicht mehr
  „lokal zuerst") und ein abschließendes `<path>` (§8.9/§8.10).
* **mheard** (`ax25netd_mheard_frame`) nur für **Radio**-Upstreams; Port 255
  füttert mheard nicht → der WAMPES-Spiegel bringt heute keine Statistik.

### 2.4 `ax25netctl`

`-s` liest netd; Kill per **Name** verweigert WAMPES-Ports schon mit
`AX25_PORT_WAMPES` („no kill on this path"), kennt aber `info.node`.

### 2.5 Die zu killende Session: `struct axpipe`

Was per `listen ax25 add …` bzw. `listen netrom add …` (Ziel `client:`,
Programm oder Socket) an ein Unix-Programm übergeben wird, ist ein
**`struct axpipe`** (axserver.c:1378).  Es trägt **genau eines von zwei**:

* `pp->sp` — ein AX.25-Consumer (`open_axservice`, per PID), oder
* `pp->pc` — eine **NET/ROM-L4-Circuit** (`struct circuit`, netrom.h:78).

Geschlossen wird nach genau dieser Unterscheidung, und zwar schon heute
(axserver.c:1467, session.c:196/202):

```
axpipe_hangup:   pp->sp ? close_axservice(sp) : close_nr(pc)
doclose:         AX25TNC ? disc_ax25(axp)     : NRSESSION ? close_nr(nr)
```

**Wichtig (Thomas):** NET/ROM-**L3** ist der `ax25_cb` zum Nachbarn — das
Routing aller Nachbarn, auf dem **mehrere L4-Circuits** reiten.  Ihn zu killen
wäre „einen Baum absägen wegen einem Apfel".  Gekillt wird die **exakt
gewünschte User-Session**, also die L4-`circuit` (`close_nr`) bzw. der
AX.25-`axservice` — der L3-Link bleibt stehen.

**Auch die `client:`-Weiterleitung ist ein `pp->pc`.**  `axserv_handover()`
(axserver.c:2182) schiebt dem Client nur eine Zeile
`netrom <user>,<node> > <call>` samt einem socketpair-Ende per `SCM_RIGHTS`
zu; das L4-Objekt ist danach `pp->pc`.  Der Client sieht also AX.25
(`pid=text`), das Objekt ist NET/ROM.  `axpipe_close()` (axserver.c:1691) löst
nur ab; **abbrechen** tut `axpipe_hangup()`.  Merkmal der Herkunft ist damit
`pp->pc != NULL` → netrom, sonst `pp->sp` → ax25.

## 3 Anforderungen

* **(A)** Sessions auflisten: id, Port, from/to, STATE, Owner.
* **(B)** `kill` muß wirklich beenden.
* **(C)** Nicht-I-Frames → netd → mheard, ohne WAMPES zu belasten.
* **(D)** Der Konnektor bleibt eigenständig (§G).
* **(E)** Frames nur spiegeln, wenn ein Konsument da ist.
* **(F)** via-Pfad je Session merken und rotieren.

## 4 Optionen

### 4a Shim registriert beim netd (nur für die Anzeige)
Die Brücke, ohnehin Lazy-Port-255-Client, meldet Session-Anfang/-Ende per
neuem `'Q'` an netd; netd zeigt es mit owner/pid.  Billig, aber:
* kein *Abbruch*weg (netd → Shim bräuchte den Thread),
* nach netd-Restart veraltet.

### 4b WAMPES als AGWPE-Server — **verworfen**
War als Weg gedacht (Radio-Upstream = alles geschenkt), WAMPES bekommt aber
keinen AGWPE-Server.  Nicht weiterverfolgen.

### 4c WAMPES-interner LAPB-Monitor (echte Frame-Ebene)
Nur so kommen SABM/SABME/UA/DISC/FRMR/RR/I **echt**.  Kostet WAMPES-Arbeit;
die Notiz hält es als *spätere, optionale* Stufe fest (dann auch voller
`listen`-Inhalt für verbundene Links).

### 4d WAMPES-Session-Außenschnittstelle (`sessions` + `close`)  ← Vorschlag

Zwei neue Kommandos auf dem **Service-Socket** (nicht der root-Command-Socket):

* `sessions` → **eine Zeile je Weiterleitung an ein Programm** (`axpipe`):
  Handle + **Herkunft** (`netrom`/`ax25`) + was das Programm sieht (AX.25
  `local`/`remote` + via; bei NET/ROM die synthetische Zeile
  `<user>,<node> > <call>`) + Zustand + evtl. `&AXCB`/`&NRCB` zum Abgleich mit
  `ax st`/`s`.  Nicht der NET/ROM-L3-`ax25_cb`, nicht der eigene Login.
* `close <handle>` → die Weiterleitung schließen (`axpipe_hangup`):
  `close_nr(pc)` (NET/ROM-L4) bzw. `close_axservice(sp)`.  Die App-Fd wird
  EOF; der L3-Link bleibt stehen.

**Angebunden über den vorhandenen Kill-Weg.**  `axkill`, `axctl kill` und
`ax25netctl -k` gehen alle `socket(AF_AX25)` + `SIOCAX25CTLCON`; die alten
Tools sind genau dafür getestet.  Diesen ioctl bedient bisher nur das
AGWPE-Backend (`libax25/agwpe_sock.c`:4461 → `'Q'` KILL an netd); das
WAMPES-Backend lässt ihn fallen („the node has no equivalent",
`agwpe_sock.c`:4392).  Also wird derselbe ioctl **in `wampes.c`** bedient und
übersetzt: `ctl->port_addr` → Node-Socket, `sessions`, Paar `src`/`dest`
suchen, `close <handle>`.  Das Paar ist bei AX.25 `local`/`remote`, bei einer
NET/ROM-L4-Circuit der User (`cuser`) und das erreichte Ziel-Call.  Genau die
Symmetrie zu `'Q'` KILL.

**Warum das die Fragen auflöst:**
* **Ein Kill-Weg für alle:** kein neues Tool; `ax25netctl -k` verliert seinen
  Sonderweg, der `AX25_PORT_WAMPES`-„no kill"-Zweig wird zum Normalfall.
* **Kein Thread:** der Node schließt; der Client bekommt EOF und beendet
  sich selbst (das ist genau das, was ein `close()` des Fd täte).
* **Restart-fest:** WAMPES ist die Autorität; netd hält keinen
  Session-Zustand für WAMPES.
* **netd bleibt außen vor:** WAMPES-Sessions sind netd nicht bekannt, es gibt
  keinen zweiten Eigentümer.

**Auflisten bleibt getrennt nach Backend:** AGWPE-Ports → netd `'S'` (wie
heute); WAMPES-Ports → `ax25netctl` fragt den Node direkt (`sessions`, on
demand).  Damit braucht es keine netd-Registrierung, und on-demand ist von
Natur aus restart-fest.

**Kosten:** kleine WAMPES-Erweiterung (`sessions`/`close` + Merkmal
„Service-Session"); `wampes.c`-Seite ein `wampes_ioctl()` neben
`agwpe_ioctl()`; in `ax25netctl` der `-s`-Zweig für WAMPES-Ports.

### 4e Frame-Ebene durch den Shim (gewählt)
Der Shim bildet an seinen Ereignissen grobe Linkframes nach und legt sie auf
255: `connect` → SABM/SABME, `accept` → UA, `close` → DISC/DM.  Kein
I-Frame, kein FRMR, keine Zähler.

**Ehrlichkeits-Grenze** (T.): was libax25 an WAMPES schreibt, ist *nicht*
das Paket — ein `write()` mit 600 byte wird von WAMPES je paclen zu 3+
Paketen geschnitten.  Ein erfundenes I-Frame wäre falsch.  Synthese
beschränkt sich daher auf den Verbindungslebenslauf.

**mheard-Anbindung:** `ax25netd_mheard_frame()` hängt an Radio-Upstreams;
der Loop speist mheard heute absichtlich nicht.  Für (C) muß der WAMPES-Weg
mheard **mit**speisen (Thomas: „dann müsste dann wohl auch loop mit
beinhalten") — also `ax25netd_mheard_frame()` mitbenutzen statt am Loop
vorbeiführen.  Der genaue Weg (eigener „WAMPES/255-Radio"-Typ oder zweiter
Aufruf) bleibt offen.

## 5 Empfehlung

* **Session sichtbar + killbar:** 4d (WAMPES `sessions`/`close`), Kill über
  den vorhandenen `SIOCAX25CTLCON`, der im WAMPES-Backend bedient wird.
  Erfüllt (B) ohne Thread und restart-fest.
* **Anzeige in netd selbst** bleibt Option 4a — dann zusätzlich.  Nicht
  nötig für das Ziel.
* **(C) Frames:** 4e (Synthese) wie gewählt; ehrlich wird es erst mit 4c.
  Vor dem Bau überlegen, was mheard wirklich braucht („heard"/Callsign)
  — dafür reicht die Synthese; „link statistics" nur mit 4c.
* **(E)** Der Anlauf ist das Registrieren/Abbild der Session (wenige Byte);
  echtes Traffic-Spiegeln nur bei Konsument — meldbar über ein
  `monitor`-artiges `'Q'` in **eine** Richtung (ds. 4a-Kanal).
* **(F)** via-Pfad aus `connect()`/`accept()`-Sockaddr je `wampes_sock`
  merken; für SABM/SABME (connect) und den Rest den Pfad rotieren.

*(Überholt 2026-10-15: der Spur-Weg oben — Spiegeln und Synthese im Shim —
gilt nicht mehr.  WAMPES liefert seinen AX.25-Verkehr roh; siehe §11.)*

## 6 Offene Entscheidungen

*Stand 2026-10-14: 1–3 sind mit Schritt 1 (siehe §7) entschieden und gebaut;
4–6 bleiben offen.  Zum Datagram-Fall, zum Digi-Pfad und zur Umfangs-Frage
(nur libax25 oder auch WAMPES-eigene Ziele) siehe §8.*

*Stand 2026-10-15: 1–3 sind gebaut (Schritt 1 in §7, Anzeige und Kill in
Schritt 3, §10).  4–6 sind entschieden — WAMPES wird rohe Monitor-Quelle,
siehe §11.*

**Nach §8 nachzuziehen:** Punkt 6 (Ziel `program:`/`socket:`/`login`) ist
nach der Umfangs-Überlegung in §8.2/8.5 vermutlich *kein* Listeneintrag mehr —
Schritt 1 listet noch jede Weiterleitung, nicht nur die zu libax25.

1. **Kommandonamen** auf dem Service-Socket: `sessions` + `close <handle>`
   (nicht `full_table`, kein `command`/`console`).  `close` per **Handle**
   (stabil), nicht per Callsign-Paar; die Paar-Suche macht die Brücke aus
   `ctl->src`/`ctl->dest`.
2. **Herkunft statt zweiter Liste (Empfehlung):** Die Session ist die
   Weiterleitung (`axpipe`); sie wird **einmal** gelistet — mit der Identität,
   die das Programm sieht, **und** ihrer wahren Herkunft (`netrom`/`ax25`).
   Nur WAMPES kennt die Herkunft; der Unix-Seite sieht man sie nicht an, genau
   darum soll `ax25netctl -s` sie zeigen.  Gekillt wird die Weiterleitung
   (`axpipe_hangup`): `close_nr` der L4-Circuit bzw. `close_axservice`.
   **Nicht** der NET/ROM-L3-`ax25_cb` (Routing aller Nachbarn, mehrere L4
   darauf).  Node-eigene Connects/Login bleiben außen vor.
   *Darstellung* (eine Tabelle vs. Gruppierung nach Herkunft wie die
   `s`-Abschnitte) und *Sortierung* (nach src-Call und/oder Protokoll) werden
   **später live** beurteilt; die Daten dafür liefert die Zeile ohnehin.
3. **Handle:** laufende Nummer, die WAMPES pro Weiterleitung vergibt (fd und
   Pointer sind über die Lebenszeit nicht stabil); `&AXCB`/`&NRCB` zusätzlich
   als Spalte, damit man die Zeile mit `ax st`/`s` abgleichen kann.
4. **(C) mheard:** Synthese-„heard" reicht für „wer hat gehört" (Callsign);
   echte Link-Statistik erst mit 4c.  Der Loop muß
   `ax25netd_mheard_frame()` mitbenutzen (Thomas).
5. **(E) I-Frame-Inhalt:** Kopie des Inhalts der Unix↔WAMPES-Sessions nur,
   wenn ein Consumer (`listen`/`ax25mond`) da ist — und nur solange.
   Anmeldung des Consumers über einen `'Q'`-artigen Monitor-Kanal.
6. **(F) via-Pfad:** in der Brücke je `wampes_sock` merken (aus
   `connect()`/`accept()`-Sockaddr); für SABM/SABME (connect) und die
   Folgeframes rotieren.

## 7 Umsetzung Schritt 1 — WAMPES-Seite (2026-10-14/15)

Umgesetzt in `wampes.git/src/axserver.c`, `remote_net.c`, `lapb.h`; voll
gebaut und verlinkt (`net`) mit `CC='gcc -std=gnu23'`, dazu die Wortänderung in
`ax25-apps.git/ax25netd/mux.c` (`SABM` → `CONNECTING`, siehe unten).

Zwei neue Kommandos auf dem **Service-Socket** (nur `service_table`, nicht der
root-Command-Socket):

* `sessions` → **eine Zeile je Weiterleitung an ein Programm** (`axpipe`) plus
  eine Zeile je geclaimtem Rufzeichen (Listener/Datagram), am Ende **eine
  `***`-Abschlusszeile**.  Der Service-Socket hat keinen Prompt und die Antwort
  ist so lang wie die Tabelle; ohne die Schlusszeile wäre eine kurze Antwort
  nicht von einer mittendrin abgeschnittenen zu unterscheiden.  Eine Zeile
  (Sitzung *oder* Claim) beginnt nie mit `*`, die Schlusszeile ist also nie zu
  verwechseln.
* `close <handle>` → löst genau diese Weiterleitung.  Unbekanntes/unpassendes
  Handle: eine `***`-Zeile, die Verbindung bleibt offen.  Der Name steht in
  `service_table` absichtlich **zuletzt**, weil Kommandos über ihre ersten
  Buchstaben gefunden werden und ein blankes `c` schon immer `connect` hieß
  (`cl` erreicht `close`).

### 7.1 Zeilenformat

Felder mit je einem Leerzeichen, keines enthält eines, also per Auge *und* per
`strtok()` lesbar — **dieselben Felder, die `ax25netctl -s` druckt** (ohne
UPSTREAM; das ist der Knotenname und steht beim Aufrufer):

```
<id> <port> <from> <to> <proto> <state> <owner> <prog> <path>
```

* `id` — laufende Nummer, die WAMPES je Weiterleitung vergibt und die für
  deren Lebensdauer gilt (fd und Pointer sind es nicht).  Für eine
  Claim-Zeile (Listener/Datagram) `-`: dort ist nichts zu schließen.
* `port` — der Interface-Name der Session (`axlisten_portname`); bei NET/ROM
  `netrom`; bei einer Claim-Zeile die Port-Liste des Listeners (`!`-Präfix wie
  in der Anzeige von `listen`) oder `-` für „alle".
* `from` — **der Rufende** (wer den Ruf aufgebaut hat), `to` — **der
  Angerufene**.  Damit liest die Zeile **in Richtung des Rufs** — `A > Z via
  B C D` —, und der Pfad darunter paßt zu `from`→`to`.  Wer das ist, hängt
  davon ab, **wer den Link geöffnet hat** (`pp->we_called`): bei einem von
  uns geöffneten Link ist `hdr.source` = wir = Rufender (der Kopf wurde nie
  gedreht, `ax25_adopt_path()` kopiert unseren eigenen), bei einem
  hereinkommenden hat `build_path(reverse)` gedreht, dann ist `hdr.dest` =
  Rufender.  Bei NET/ROM ist der Rufende der User, der den Circuit aufgebaut
  hat (`pc->cuser`, aus dem NR4 CONNECT REQUEST in `circuit_manager`), der
  Angerufene die geclaimte Call.  Claim-Zeile: `from` = geclaimte Call,
  `to` = `*`.
* `path` — Digi-Pfad **in Richtung `from`→`to`** (in der Fahrtrichtung des
  Rufs; komma-getrennt, max 8), `-` wenn keiner.  Bei einem von uns
  geöffneten Link läuft `hdr.digis` von unserem ersten Hop nach außen, also
  vorwärts; bei einem hereinkommenden ist er mitgedreht und wird rückwärts
  gelesen (B, C, D und nicht D, C, B — die Reihe des Draht-Monitors,
  `A->Z v B C D`).  Bei NET/ROM der Knoten (`pc->node`), also „wie kam es
  her".
* `proto` — `lp->pid` des Claims (bei NET/ROM 0), **dezimal**; die Anzeige
  formatiert nach `0x%02X`, wie netd es schickt.
* `state` — das **vereinheitlichte** Wort (siehe 7.2).
* `owner` — PID des Programms am fernen Ende des Claim-/Handover-Descriptors
  (`SO_PEERCRED`/`LOCAL_PEERPID`, zur Connect-Zeit festgehalten), `0` wenn
  keins; die Anzeige macht daraus `-`.
* `prog` — Name dazu aus `/proc/<pid>/comm` (nur Linux; sonst `-`), wie
  `client_owner` im netd.  Leerzeichen im Namen werden zu `_`.

Beispiel:

```
3 hf DL9SAU-3 DB0FHN-8 240 ESTABLISHED 4711 axlisten DB0AAA,DB0BBB
- hf DL9SAU-8 * 240 LISTENING 4711 axlisten -
- - DL9SAU-9 * 240 DATAGRAM 4711 aprsclient -
```

### 7.2 Zustandswörter (vereinheitlicht mit netd)

`CONNECTING` / `ESTABLISHED` / `LISTENING` / `DISC`, dazu `DATAGRAM`:

| WAMPES | Wort |
|---|---|
| `LAPB_SETUP`, `NR4STCPEND` | `CONNECTING` |
| `LAPB_CONNECTED`, `LAPB_RECOVERY`, `NR4STCON` | `ESTABLISHED` |
| `LAPB_DISCPENDING`, `LAPB_DISCONNECTED`, `NR4STDPEND`, `NR4STDISC` | `DISC` |
| `LAPB_LISTEN`, `NR4STLISTEN`, I-Claim | `LISTENING` |
| UI-Claim | `DATAGRAM` |

`LAPB_RECOVERY` zählt als `ESTABLISHED` (der Link steht und wird gepollt; netd
hat kein eigenes Wort dafür).  Damit netd dasselbe Vokabular spricht, wurde
dessen `session_row` von `"SABM"` auf `"CONNECTING"` geändert
(`ax25-apps.git/ax25netd/mux.c`), samt Kommentar und dem Beispiel in
`libax25.git/netax25/agwpe_client.h`.

### 7.3 Umfang: nur libax25

Gelistet wird eine Weiterleitung nur, wenn sie **ein Programm** hat.  Das
merkt sich das `axpipe` in `ownerfd`:

* AX.25 hereinkommend (`LK_CLIENT`) → `lp->clientfd`;
* NET/ROM (`listen netrom add client:<call>`) → `cp->clientfd` des geclaimten
  AX.25-Eintrags;
* ausgehender Handover (`axserv_pipe_attach`, `connect` + `handover`) → `cp->fd`
  des aufrufenden Service-Sockets.

Alles, was der Knoten selbst bedient (`program:`, `socket:`, das eigene Login)
läßt `ownerfd = -1` und erscheint **nicht**: diese Liste ist zum Handeln, und
über sie von außen an die eigenen Dienste zu kommen wäre Fernadministration.
Claim-Zeilen erscheinen für jedes `LK_CLIENT`-Rufzeichen mit `clientfd >= 0`
(I-Listener → `LISTENING`, UI-Claim → `DATAGRAM`), jeweils mit PID/Programm für
den Sysop.

**Kill ist nicht der Link.**  `close <handle>` ruft `axpipe_release()`: bei
einer AX.25-Weiterleitung `close_axservice(sp)`, bei NET/ROM `close_nr(pc)`.
Das App-Fd sieht EOF, der AX.25-L2- bzw. NET/ROM-L3-Block bleibt stehen - es
ist derselbe Übergang, den die Peer-Seite auslöst, wenn der Link zuerst geht
(`axpipe_state_upcall`), nur von hier angestoßen.  `axpipe_release` ersetzt
`axpipe_hangup` an allen vier Aufrufstellen (EOF, Schreibfehler, Connect-
Fehler, jetzt auch `close`).  Ein Kill wird **laut** protokolliert
(`syslog`: `axserv: session %lu closed on request (%s)`).

**Nebenbefund, im selben Zug behoben.**  `axpipe_hangup` gab bei einer
AX.25-Weiterleitung nur `close_axservice(sp)`; der State-Upcall, der das
`axpipe` freigibt, kann danach nicht mehr kommen (das `sp` ist aus
`axp->services` entfernt), also blieben `pp` und sein Fd zurück, wenn das
*Programm* zuerst schloß (EOF) oder ein Schreibfehler auftrat.  Bei NET/ROM
fiel das nicht auf, weil `close_nr()` den Circuit und damit den Upcall am
Leben läßt.  `axpipe_release()` räumt in beiden Fällen selbst auf; nur deshalb
bleibt die `Axpipes`-Liste auch dann ehrlich, wenn das Programm die Session
beendet.

### 7.4 Laufzeit-Beleg (2026-10-15)

Node headless gestartet (`net -g -v -d <dir> <dir>/net.rc`, macOS), mit drei
`listen ... client`-Zeilen; ein Python-Client am Service-Socket
(`<dir>/sockets/ax25`):

```
-> binary
-> listen DL1TST-8
<- *** listening on DL1TST-8 pid 0xf0
-> listen ui DL1TST-9
<- *** listening on DL1TST-9 pid 0xf0
-> listen DL1TST-10
<- *** listening on DL1TST-10 pid 0xf0
-> sessions
<- - hf,aprs DL1TST-10 * 240 LISTENING 25112 - -
<- - - DL1TST-9 * 240 DATAGRAM 25112 - -
<- - - DL1TST-8 * 240 LISTENING 25112 - -
<- *** 3 sessions
-> close -
<- *** no session -
-> close 99
<- *** no session 99
```

Belegt: Claim-Zeilen (I → `LISTENING`, UI → `DATAGRAM`), Port-Spalte aus der
Port-Liste (`hf,aprs`) bzw. `-`, der Abschlussmarker, und daß ein unbekanntes
Handle die Verbindung **nicht** schließt (die nächste Zeile kommt noch).  Eben
so belegt: halb-schließen (`shutdown(SHUT_WR)`) nach `sessions` → die Liste
kommt, danach EOF.

Der `owner` (25112) ist die PID des Python-Clients (`LOCAL_PEERPID`); `prog`
bleibt `-`, weil macOS kein `/proc` hat — dieselbe Antwort wie netd dort.
**Nicht** in diesem Lauf: eine echte Session-Zeile (`sp`/`pc`), die einen
AX.25-Link braucht; die Ausgabe teilt sich aber den Zeilenbau mit den
verifizierten Claim-Zeilen.

**Nicht Teil von Schritt 1:** Brücke (`libax25/wampes.c`, `SIOCAX25CTLCON`),
`ax25netctl -s`, sowie (C)/(E)/(F).

## 8 Recherche 2026-10-14 — Datagram, Digi-Pfad, Umfang

### 8.1 Ein Datagram-Socket sind zwei WAMPES-Objekte — keins davon ein `axpipe`

Ein libax25-Programm mit `SOCK_DGRAM` auf einem Node-Port legt **zwei**
Service-Verbindungen an (`libax25/wampes.c`):

1. **Empfangen (Claim).**  `bind()` → `claim_ui()` (`wampes.c`:1831) öffnet
   eine eigene Service-Verbindung, schickt `listen ui <call> pid=0x..` und
   macht sie per `axsock_replace()` **zum Programm-Fd**.  WAMPES sieht das als
   `axlisten`-Eintrag mit `ui=1`, `kind=LK_CLIENT`, `clientfd>=0`
   (`axlisten_client_claim`, `axserver.c`:749); eingehende UI-Frames gehen
   über `axlisten_ui_deliver()` (`axserver.c`:1119) als TNC2-Zeile an diesen
   Fd.  Also ein Fd — **kein** `ax25_cb`, kein `axpipe`.
2. **Senden.**  Beim ersten `sendto()` (`wampes.c`:1692) öffnet der Shim eine
   **zweite** Service-Verbindung und schickt `datagram <iface>: [--pid 0x..]`;
   danach Counted-Form-Frames.  Das ist in WAMPES ein `struct controlblock`
   mit `dgram=1` (`datagram_command`, `remote_net.c`:802) — **kein**
   `ax25_cb`, **kein** `axpipe`, und in **keiner** Liste (Controlblöcke hängen
   nur an `on_read_arg(fd)`, es gibt kein Register).

Es gibt also keine „Verbindung nach außen" im LAPB-Sinn.  Was es gibt, ist
(a) ein **Claim** auf eine Call (hat einen Namen, keinen `ax25_cb`) und (b)
ein anonymes `datagram`-Controlblock (hat nur Port/PID, keinen Namen).

### 8.2 Zielarten und „Verbindung zu libax25"

Die vier Ziele einer `listen … add`-Zeile (`axserver.c`:628):

| Ziel                | kind        | wer bedient es |
|---------------------|-------------|----------------|
| `builtin:login`     | `LK_LOGIN`  | WAMPES (Login) |
| `tcp:<h>:<p>` / `unix:<path>` | `LK_SOCKET` | WAMPES (wählt selbst) |
| `/path program args`| `LK_PROGRAM`| WAMPES (spawnt selbst) |
| `client[:<call>]`   | `LK_CLIENT` | ein **libax25**-Programm (claimt über den Service-Socket) |

Nur `LK_CLIENT` ist eine Verbindung **zu libax25**; die anderen drei
verwaltet WAMPES selbst für eingehende Verbindungen.  Dazu kommt die
**ausgehende** Verbindung, die ein libax25-Programm selbst wählt (`connect` +
`handover` → `axserv_pipe_attach()`, `remote_net.c`:487) — die hängt am
Dummy-`pipe_lp` (`axserver.c`:2331), nicht an einem `lk`-Eintrag.

Ein `axpipe` entsteht an fünf Stellen:

* eingehend: `LK_SOCKET` (`axserver.c`:2512), `LK_CLIENT` (2540),
  `LK_PROGRAM` (2561); NET/ROM analog in `nrserv_listen_start()`
  (`LK_CLIENT` 2199 → 2207, `LK_PROGRAM` 2161 → 2207);
* ausgehend: `axserv_pipe_attach()` (2338).

`pp->lp->kind` sagt also, ob eine *eingehende* Weiterleitung libax25 gehört;
die ausgehende hängt am Dummy `pipe_lp`.  Für „nur libax25" bräuchte der
`axpipe` ein Merkmal (Bit beim Anlegen, oder Test auf `&pipe_lp`).

### 8.3 Was das für `sessions`/`-s` heißt

* **Nicht** listen: `LK_PROGRAM`, `LK_SOCKET`, `LK_LOGIN` — WAMPES-eigen; das
  wäre die `ax stat`-Sicht, also Remoteverwaltung von WAMPES.
* Listen: `LK_CLIENT` (eingehend, I **und** UI) plus die ausgehenden
  Handover-Sessions.
* **Datagram** je Claim (`ui=1`, `clientfd>=0`) eine Zeile: nicht verbunden,
  **kein** `AXCB`.  Der anonyme `datagram`-Controlblock (Senden ohne Claim,
  z.B. ein Beacon) hätte keinen Namen; ihn zu listen verlangt ein
  Controlblock-Register.
* Der Zustand der bestehenden Tabellen ist ein **Wort**: `ax25netd` schreibt
  `ESTABLISHED`/`SABM`, für „nichts dahinter" eine Zeile mit Id `0`, Remote
  `*`, Zustand `LISTENING` (`mux.c`:1327/1349); unser Format schreibt bisher
  `connected`/`connecting`.

### 8.4 Digi-Pfad

`MAXDIGIS` ist **8** (`ax25.h`:22), `AXBUF` 10.  `axpipe_line()` fasst den
Pfad schon als **ein** Komma-Token in `via[AXBUF * (MAXDIGIS + 1)]`
(`axserver.c`:1801) — 8 Digis sind höchstens ~79 Zeichen, das passt.  In der
Accept-Zeile der Brücke hängt der Pfad dagegen an `src`:
`<port> <src>[,<digi>…] > <dst>` (`wampes.c`, `wampes_accept`).  Zu
entscheiden ist, ob `sessions` den Pfad getrennt hält (eigene `via`-Spalte,
bis 8, wie jetzt) oder „abtrennt" (eigene Zeile / an `src` / ans Ende).

### 8.5 AGWPE-Vergleich und I- vs UI-Listener

**Empfangen kann die AGWPE-Seite sehr wohl.**  Ein Datagram-Socket registriert
sich beim AGWPE-Server und schaltet die UI-Subscription ein
(`axsock_nuisub++`, `agwpe_client_uisub()` — `agwpe_sock.c`:3502-3505); netd
liefert eingehende UI-Frames darauf zugestellt aus: `mux_upstream_ui()`
(`mux.c`:2197) parst den UI-Frame, sucht den Halter der Zielcall
(`client_by_call`) und schickt ihn als `'M'` nur dem, der `cl->uisub` gesetzt
hat.  Der frühere Satz „AGWPE has no per-callsign UI delivery" ist damit
überholt und in `libax25.git/doc/AX25-WITHOUT-KERNEL-DEVELOPER.md` am
2026-10-14 richtiggestellt: die Zustellung läuft, nur das **Listing** fehlt.

**Warum der Datagram-Claim trotzdem nicht in `-s` steht:** der `bind()`
registriert ihn mit `'X'`/`listener=0` (`agwpe_sock.c`:3507), und
`mux_ctl_sessions()` druckt aus den `calls[]`-Tabellen nur Einträge mit
`listener==1` (`mux.c`:1374/1394/1405/1452) — das ist die `'L'`-Registrierung,
die nur der Akzeptor auf dem Loop-Port verwendet (`agwpe_sock.c`:4208).  Die
UI-Subscription wird in netd zwar geführt (`cl->uisub`, `mux_uisub_clients`),
aber pro **Client**, nicht pro Call, und nicht gedruckt.  `'X'` allein reicht
zur Unterscheidung nicht: auch ein verbundener Socket registriert beim
`bind()` mit `'X'` (`agwpe_sock.c`:3568).

**I- vs UI-Listener sind zu trennen.**  Ein Programm, das `bind(DB0FHN-9)`
tut, kann **beides** aufbauen: einen Connect-(I-)Listener und einen
Datagram-(UI-)Listener.  In `-s` müssen die zwei `LISTENING`-Zeilen
unterscheidbar sein, also braucht der UI-Fall ein eigenes Zustandswort
(Vorschlag: `LISTENING` = I wie bei netd, `DATAGRAM` = UI).

**Halter (PID/Programm).**  netd holt die Peer-PID beim `accept()`
(`loop_peer_pid()`, `loop.c`:574) — Linux `SO_PEERCRED`, macOS
`LOCAL_PEERPID` — und liest den Namen aus `/proc/<pid>/comm` (`client_owner()`,
`mux.c`:1270).  Für den WAMPES-UI-Claim ist der Anker der `clientfd`
(der Service-Socket, der per `axsock_replace()` zum Programm-Fd wurde); aus
seinen Peer-Credentials kommt dieselbe PID/Prog-Anzeige.

### 8.6 Offene Fragen (an Thomas, 2026-10-14)

**Entschieden am 2026-10-14:**

1. **Pfad:** bleibt ein eigenes `via`-Feld (ein Komma-Token, bis 8) — „einfach
   ausprobieren, nachbessern kann man immer".
2. **Datagram:** am UI-Claim ankern, mit PID/Programm wie bei netd; I- und
   UI-Listener müssen unterscheidbar sein (§8.5).  Zustandswörter:
   **`LISTENING`** = I, **`DATAGRAM`** = UI.  Die Zeile nennt lokal die
   geclaimte Call (z.B. `DB0FHN`) und remote `*`.
3. **Umfang:** **eng** — nur libax25 (`LK_CLIENT` + ausgehende
   Handover-Sessions).  Die WAMPES-eigenen Ziele (`program:`/`tcp:`/`login`)
   werden *nicht* gelistet; eine Öffnung nach außen via `axsock` wird bewusst
   aufgeschoben (später erweiterbar).
4. **Einheitliches Anzeigeformat:** ja.  Die WAMPES-Antwort liefert dieselben
   Felder, die `ax25netctl -s` schon druckt
   (ID/PORT/FROM/TO/PROTO/STATE/UPSTREAM/PID/PROG), damit **eine** Tabelle
   beide Backends zeigt.
5. **Kill:** nur **verbundene Sessions** (Id ≠ 0).  Listener (`LISTENING`,
   `DATAGRAM`) werden **nicht** gekillt — wie netd's `check_kill_name()` sagt:
   „a listen cannot be killed.  Stop the program instead (pid N, prog)"; die
   PID/Prog-Spalten machen das möglich.
6. **Doku-Fix:** der Satz „AGWPE has no per-callsign UI delivery, only the
   monitor stream" in `libax25.git/doc/AX25-WITHOUT-KERNEL-DEVELOPER.md` ist
   richtiggestellt (netd liefert per UI-Subscription; das haben `agwpe.h` und
   `agwpe_client.h` schon korrekt beschrieben).
7. **AGWPE nachziehen:** ja, gewünscht.  netd muss `'X'`+`uisub` von einem
   verbundenen `bind()` trennen können, damit auch dort ein Datagram-Claim eine
   Zeile bekommt (kleine Regel-/Protokoll-Ergänzung in
   `agwpe_sock.c`/`mux.c`).

8. **Pfad-Darstellung:** der Pfad (bis 8 Digis) bekommt eine **eigene
   Fortsetzungszeile**, eingerückt bis unter `FROM`; gedruckt nur, wenn wirklich
   Digis da sind.  Keine zusätzliche Spalte (die würde bei 8 Digis die FROM→TO-
   Breite sprengen und hinter `PROG` wäre sie unansehnlich):

   ```
   ID   PORT  FROM       TO        PROTO STATE        UPSTREAM  PID   PROG
   1    hf    DL9SAU-3   DB0FHN    0x00  ESTABLISHED  node      1234  axlisten
         DL9SAU-2, DB0AAA, DB0FHN
   ```

   netd-Zeilen hatten damals keine Pfadzeile.  Inzwischen führt netd den
   Pfad für **selbst initiierte** Connects mit (Nachtrag §8.10) — die Pfadzeile
   gibt es also auch dort, wo es einen Pfad gibt.

9. **Zustandswörter der Sessions — vereinheitlicht** auf netd's Vokabular
   (`CONNECTING`/`ESTABLISHED`/`LISTENING`) plus `DISC` und `DATAGRAM`.
   **`SABM` wird zu `CONNECTING`** (Thomas, 2026-10-14): `SABM` benennt den
   AX.25-Layer, nicht den Zustand.  Damit das eine Tabelle bleibt, muss
   **netd dasselbe Wort drucken** — `ax25netd/mux.c:1330`
   (`s->connecting ? "SABM" : ...`) und der Kommentar darüber.

   | WAMPES-Zustand | Wort |
   |---|---|
   | `LAPB_SETUP`, `NR4STCPEND` | `CONNECTING` |
   | `LAPB_CONNECTED`, `NR4STCON`, `LAPB_RECOVERY` | `ESTABLISHED` |
   | `LAPB_DISCPENDING`, `NR4STDPEND`, `LAPB_DISCONNECTED`, `NR4STDISC` | `DISC` |
   | `LAPB_LISTEN`, `NR4STLISTEN` | `LISTENING` |
   | I-Claim ohne Session | `LISTENING` |
   | UI-Claim | `DATAGRAM` |

   **Weitere Zustände als vermutet:** ja.  Im LAPB gibt es neben `SETUP`,
   `CONNECTED` und `DISCPENDING` noch **`LAPB_RECOVERY`** (Link war oben, T1/T3
   fragen per Poll nach, ob die Gegenstelle noch da ist — die Session läuft
   weiter) und **`LAPB_LISTEN`**; NET/ROM spiegelt das mit `NR4STCPEND`,
   `NR4STCON`, `NR4STDPEND`, `NR4STDISC`, `NR4STLISTEN`.  `RECOVERY` wird auf
   `ESTABLISHED` abgebildet (entschieden 2026-10-14), weil die Session aus Sicht
   des Nutzers steht.

10. **PROTO-Spalte:** netd zeigt `0xNN` (AX.25-PID).  Für WAMPES-Sessions den
    `lp->pid` des Claims, sonst `0x00`.
11. **I-Listener ohne Session:** als eigene `LISTENING`-Zeile mitlisten (wie
    netd) — ja, Symmetrie.

**Beim Durcharbeiten aufgefallen (kleine Protokoll-/Integrationspunkte):**

12. **Ende der Liste:** der Dienst-Socket des Nodes hat keinen Prompt, und
    `sessions` liefert beliebig viele Zeilen — ein maschineller Leser (libax25)
    kann also nicht wissen, wann die Liste zu Ende ist.

    *Thomas' Vorschlag (2026-10-14):* die Session nicht dauerhaft halten,
    sondern für die Abfrage öffnen und den Socket danach schließen; dann ist
    das Ende automatisch, wenn WAMPES die Session nach dem Befehl schließt.
    Seine Einschränkung: nur bei *unvollständigen* Daten (blocking write, dann
    close) hätte man unvollständige Daten, die man ohne End-Marker nicht
    erkennt.

    *Befund:* `sessions` schließt die Session **heute nicht** —
    `sessions_command` liefert 0, der Dienst-Socket bleibt für weitere Befehle
    offen (ein Sysop tippt `sessions` und danach `close 3`).  Damit EOF das
    Ende sein *kann*, muss die Node-Seite schließen.  Zwei Wege:
    (a) der Client halbschließt nach dem Absenden (`shutdown(SHUT_WR)`); die
    Node liest den Befehl, schreibt die Liste, sieht dann EOF und schließt —
    **keine Protokolländerung**; oder (b) ein eigenes Einmal-Kommando, das 1
    zurückgibt.  In beiden Fällen ist EOF das Ende, und Thomas' Einwand
    stimmt: eine abgebrochene Liste ist von einer kurzen vollständigen nicht
    zu unterscheiden.  Der `***`-Abschluss (`*** N session(s)`) ist genau das,
    was diese Lücke schließt.  Neigung: Marker, plus eigene kurzlebige
    Verbindung pro Abfrage.
13. **`-k <id>` und kein Node:** `ax25netctl -s` hat kein Port-Argument, und
    eine WAMPES-Id gilt nur innerhalb *eines* Nodes.  `axkill`/`-k port:dest
    src` ist eindeutig (Port → Node); `-k <id>` allein nicht.  Vorschlag: `-s`
    fragt alle in `wampes.conf` konfigurierten Nodes, und die Id-Spalte bekommt
    für WAMPES den Node als Präfix (`wampes:3`), damit `-k wampes:3` den Node
    mitnennt.

    **Zur vermuteten Race:** die Id wird bei der Erzeugung **einmal** vergeben
    (`pp->id = NextAxpipeId++`, `axserver.c`) und **nie wiederverwendet** — der
    Zähler läuft nur aufwärts.  Sie verschiebt sich also nicht, wenn neue
    Sessions dazukommen: die als `1` gesehene Zeile bleibt `1`, solange die
    Weiterleitung lebt; `-k wampes:1` trifft sie oder meldet „keine Session 1",
    aber nie eine *andere*.  Ein Zeiger als Id wäre **schlechter**, nicht
    besser: nach `free()` kann der Allokator dieselbe Adresse an eine neue
    Session (oder etwas anderes) geben, und eine veraltete Zeiger-Id träfe
    dann lautlos eine andere.  Also: Zähler behalten, Node-Präfix davor.
    Verbleibende Lücke: **ein Node-Neustart** setzt den Zähler auf 1 zurück.
14. **Owner von Session-Zeilen:** PID/Prog kommen aus den Peer-Credentials des
    Dienst-Sockets, über den die Session kam — für einen `LK_CLIENT`-Claim
    `lp->clientfd`, für NET/ROM der Claim `cp->clientfd`, für den ausgehenden
    Handover der `fd` der `connect`-Verbindung.  Der Axpipe müsste sich diesen
    `fd` beim Anlegen merken (kleines neues Feld `ownerfd`).

### 8.7 Entschieden am 2026-10-15 (und ein neuer Befund)

**Entschieden (Thomas, 2026-10-15):**

1. **netd mitziehen:** ja — `ax25netd/mux.c` druckt `CONNECTING` statt `SABM`
   (samt Kommentar), und das Beispiel in `agwpe_client.h` ist angepaßt.
2. **Listen-Ende:** beides.  Der Client öffnet eine kurzlebige Verbindung,
   halbschließt nach dem Absenden, die Node schließt nach der Liste (EOF) —
   und die Liste endet zusätzlich mit dem Marker `*** N session(s)`.  Beides
   zusammen: EOF beendet zuverlässig, der Marker macht eine trotzdem
   abgeschnittene Liste erkennbar.
3. **Umfang/Anzeige/Kill** wie in §8.6 (nur libax25; nur verbundene
   Sessions killbar; PID/Prog für Listener).

**Neu aufgetaucht (2026-10-15): FROM/TO-Orientierung — korrigiert 2026-10-15.**

Beim Umsetzen zeigte sich erst, dass die Spalten in §8.6/§7 verdreht waren
(ich hatte „from = Gegenstelle" geschrieben); die Korrektur darauf war aber
selbst falsch: „`FROM` = die lokale Call" (netd's `s->call_from` = near).
Thomas hat den Punkt entschieden — maßgeblich ist **nicht** die Sicht „lokal
vs. Gegenstelle", sondern die **Richtung des Rufs**:

* **`FROM` = der Rufende (Initiator), `TO` = der Angerufene.**  Ein Trace
  `A>B via C` allein sagt nicht, wer den Ruf aufgebaut hat; wir wissen es aber
  — ob die Unix-Seite der Initiator war (ein `connect()`) oder ob es ein
  `accept()` auf ein `listen()` war.  Genau das gehört in die Zeile.

Warum das wichtig ist, zeigt derselbe Fall wie beim Digi-Pfad: sind wir
DB0FHN und ruft DL9SAU-3 über DL9SAU-2, DB0AAA an, so ergibt
`FROM=DL9SAU-3 TO=DB0FHN` mit Pfad `DL9SAU-2,DB0AAA` den Satz „DL9SAU-3 rief
DB0FHN über DL9SAU-2, DB0AAA" — lesbar und eindeutig.  Mit `FROM=DB0FHN`
(lokal) läse die Zeile dagegen wie ein Ruf von DB0FHN über DL9SAU-2, und
DL9SAU-2 ist nicht DB0FHNs Nachbar.  **`FROM`→`TO` ist die Richtung, in der
der Pfad gelesen wird**, und nur als Rufender stimmt das.

**Der Mechanismus:** beide Öffnungswege lassen `hdr.source` = wir — bei einem
hereinkommenden Link hat `build_path(reverse)` den Kopf gedreht
(`hdr.source` = angerufene = wir), bei einem von uns geöffneten hat
`ax25_adopt_path()` unseren eigenen Kopf kopiert (`hdr.source` = Mycall =
wir).  Der Rufende ist also `hdr.source`, wenn **wir** geöffnet haben, und
`hdr.dest`, wenn **der Fernstehende** geöffnet hat.  Das `axpipe` merkt sich
beim Anlegen genau den Weg (`pp->we_called`: 0 = `listen()`/`accept()`, 1 =
`connect()`-Handover), so daß Zeile und Pfadrichtung derselben Angabe folgen.

Für NET/ROM ist der Rufende der User, der den Circuit geöffnet hat
(`pc->cuser`, gesetzt aus dem NR4 CONNECT REQUEST in `circuit_manager`), der
Angerufene die geclaimte Call; der Knoten (`pc->node`) bleibt der `path`.

**Fortsetzungszeile unter `FROM`:** bestätigt — sie steht unter `FROM`, weil
der Pfad in `FROM`→`TO`-Richtung läuft und damit zu `FROM` gehört (Thomas'
„unter FROM" war von Anfang an richtig; meine §8.7-Begründung für `TO` hing an
der verdrehten Feldbedeutung).

**Nebenbefund beim Prüfen — netd speichert einheitlich, zeigt aber gemischt.**
Die erste Vermutung, netd fülle seine Paare je Weg verschieden, war zu grob:
`mux.c` legt **jede** Session als `session_add(u, lokal, gegenstelle, …)` an,
d.h. `s->call_from` ist in allen Wegen die **lokale** Call (der
Struct-Kommentar in `ax25netd.h` sagt genau das) — eingehende Radio-Session
`(hdr->call_to, hdr->call_from)` = (lokal = Angerufener, Gegenstelle =
Rufender), ausgehende `(hdr->call_from, hdr->call_to)` = (lokal = Rufender,
Gegenstelle), Loop-Rufender-Seite `(hdr->call_from, hdr->call_to)`.  Gemischt
war nur die **Anzeige**: `session_row()` druckt stur `call_from, call_to`, und
damit steht bei der eingehenden Radio-Session der *Angerufene* in `FROM` (die
„trace"-Sicht, die Thomas bemängelt), sonst der Rufende.  Die Korrektur ist
deshalb eine reine Anzeigefrage — ein Richtungsbit (`remote_initiated`), das
nur ein eingehendes Radio-`'C'` setzt, und ein Tausch in `session_row()`
(§8.9); Speicherung und sämtliche Suchschlüssel bleiben, wie sie sind.
`session_listed_before()` faltet bei der Loop ohnehin die Gegenseite weg, und
ein Kill sucht das Paar in *beiden* Reihenfolgen — beides bleibt unberührt.

**Kleinigkeit beim Umsetzen:** ein geclaimtes Rufzeichen wird **immer** als
eigene Zeile gelistet, auch während eine Session darauf läuft (netd listet
seine Registrierungen ebenso unabhängig von den Sessions).  Die Zeile sagt
„dieses Rufzeichen hält Programm X"; die Session-Zeilen sagen, was gerade
läuft.

### 8.8 Digi-Pfad: Richtung (Nachtrag 2026-10-15)

Thomas' Nachfrage war berechtigt: der Pfad stand **verkehrt herum**.

`axp->hdr` ist der **umgedrehte** Kopf — `build_path(reverse)` hat source/dest
getauscht (`hdr.source` = wir, `hdr.dest` = Gegenstelle) und die Digis mit
`axp->hdr.digis[i] = hdr->digis[ndigis-1-i]` **mitgedreht** (`lapb.c:3415-3418`).
Auf dem Draht steht der Pfad in Fahrtrichtung (erster Hop zuerst, s. `ax25.h`
zu `digis[]`, `htonax25`/`ntohax25` und die Digipeat-Logik `idest =
digis[nextdigi]`), also für A→Z über B,C,D als `[B,C,D]`.  Im umgedrehten Kopf
steht er damit als `[D,C,B]` — genau die Reihenfolge, die `axpipe_line()` mit
`for (i = 0; i < ndigis; i++)` ausgab.

Gewollt ist die **Fahrtrichtung**, also B,C,D:
* Thomas: „sollte … als B, C, D stehen und nicht als D, C, B";
* der Kommentar über `axpipe_line()` sagt „the digipeaters the frame came
  through";
* `doc/LIBAX25-BRIDGE.md` §Sessions sagt dasselbe;
* es ist die Reihe, die der Draht-Monitor zeigt (`ax25_dump`:
  `A->Z v B C D`).

`axpipe_line()` läuft die Digis deshalb **rückwärts** (`i = ndigis-1 … 0`) —
aber **nur auf einem hereinkommenden Link**.  Auf einem von uns geöffneten ist
der Kopf nie gedreht worden (s. §8.7), `hdr.digis` läuft dort schon von
unserem ersten Hop nach außen, und der Pfad wird **vorwärts** (`i = 0 …
ndigis-1`) gelesen.  Beide Fälle hängen an derselben Angabe `pp->we_called`:
die Zeile bleibt so immer `FROM → TO via …` in Fahrtrichtung des Rufs.  Für
NET/ROM ist das Feld der Knoten und unverändert.

**Nicht geändert und bewusst so gelassen:** die **Accept-Zeile** (`LK_CLIENT`)
und **`AX25_PATH`** (`LK_PROGRAM`) zeigen den Pfad weiterhin in der
Antwortrichtung.  `build_path()` legt die REPEATED-Bits genau dafür als
**Spiegel** ab (der einzige Leser dieser Marken ist die Accept-Zeile,
`axserver.c:2677`), und `htonax25()` stempelt das Draht-Bit ohnehin neu
(`lapb.c:3420-3435`).  Die beiden sind also „wie geht die Antwort hinaus /
was der Client als Peer-Digis sieht", nicht „wie kam der Ruf" — die Sitzungs-
zeile ist die Anzeige für den Sysop.  Wenn beide gleich laufen sollen, ist das
eine eigene Entscheidung (die Marken müssten dann komplementär gelesen
werden), kein Nachtrag zu dieser.

**Fortsetzungszeile unter `FROM`:** siehe §8.7 — sie steht unter `FROM`.  Das
ist reine Anzeige aus Schritt 3; die Feldreihenfolge der `sessions`-Zeile
bleibt `<id> <port> <from> <to> … <path>`.

### 8.9 Erledigt: netd auf dieselbe FROM/TO-Regel gezogen (2026-10-15)

Thomas (2026-10-15): **„1) Jetzt"** — netd mitziehen, „weil bisher haben wir nur
ein paar Testuser".  WAMPES ist durchgängig „Rufender/Angerufener"; netd zieht
nach.

Die Sorge aus der ersten Fassung, das berühre die Session-Schlüssel, war
unbegründet (§8.7): netd **speichert** jedes Paar einheitlich als
`(lokal, Gegenstelle)`, gemischt war nur die Anzeige.  Umgesetzt ist deshalb
die reine Anzeige-Korrektur:

* **`ax25netd.h`** — neues Feld `int remote_initiated` an
  `struct ax25netd_session`; `0` außer bei einem eingehenden Radio-`'C'`.
  (Die `memset(…, 0, …)` in `session_add()` setzt es für alle anderen Wege.)
* **`mux.c`, `mux_upstream_frame()`** — das eingehende `'C'` (und das
  eingehende `'D'`, das ohne vorherige Session einen Link nachmeldet) setzt
  `ns->remote_initiated = 1`, weil *der Funk* den Link geöffnet hat.
  Der „our own connect went out first"-Zweig (Antwort auf unseren Connect)
  bleibt unverändert — dort ist die Session schon von uns angelegt.
* **`mux.c`, `session_row()`** — `FROM`/`TO` werden nach `remote_initiated`
  getauscht: `FROM = call_to` (der Fernstehende = Rufender) bei eingehendem
  Funk, sonst `FROM = call_from` (wir = Rufender).  Damit liest die Zeile wie
  die WAMPES-Zeile in Richtung des Rufs.

**Was bewußt unverändert bleibt:** die Speicherung `(lokal, Gegenstelle)`,
alle `session_find()`-Schlüssel, `session_listed_before()` (Loop-Faltung) und
die Kill-Suche (`mux_ctl_kill`, `session_find(from,to)` mit Rückfall auf
`(to,from)`) — ein Kill trifft das Paar in **beiden** Reihenfolgen, die
Anzeige-Orientierung ist ihm also gleich.  Kein Drahtformat der Schlüssel
ändert sich; nur die gedruckte Zeile.

netd-Zeilen führen inzwischen **doch** einen Pfad (Nachtrag §8.10), so daß
WAMPES- und netd-Zeile beide mit `<path>` enden.

### 8.10 Nachtrag: weshalb AGWPE „keinen Pfad" hatte (2026-10-15)

Thomas: *„Weshalb gibt es bei agwpe keinen Pfad?  User initiiert doch auch
via-connects.  Nur bei eingehender connection bin ich mir nicht sicher."*  Er
hat in beide Richtungen recht — und die Antwort ist zweigeteilt:

**Selbst initiiert: den Pfad gibt es, netd warf ihn weg.**  Der AGWPE-Client
schickt den Via-Connect als `'v'` (`AGWPE_CMD_CONNECT_VIA`), dessen Datenfeld
`[ndigi][call1(10)][call2(10)]…` ist (`agwpe_client.c:669-680`, aus der
Via-Liste des `connect(2)`); der Autorouter ersetzt ein `'C'` ohne Route
durch genau so ein `'v'` (`mux.c`, `ax25netd_route_lookup()`).  netd **sah**
die Digis also und reichte sie per `client_send_upstream()` an den TNC weiter —
legte sie aber nicht in der Session ab.  Jetzt:

* **`ax25netd.h`** — `char path[AGWPE_MAX_DIGIS * (AGWPE_MAX_CALL + 1)]` in
  `struct ax25netd_session` (Komma-join, leer = keiner).
* **`mux.c`, `session_set_path()`** — entpackt das `'v'`-Datenfeld
  (`agwpe_call_unpack`), in Fahrtrichtung, wie es schon kommt (Client-Liste
  bzw. aufgelöste Route); erste Hop zuerst, wie WAMPES `hdr.digis` eines von
  uns geöffneten Links.
* **`mux.c`, Connect-Zweig** — `session_set_path(cs, data, len)` beim `'v'`
  und `session_set_path(cs, digis, …)` im Autoroute-Zweig (die eben
  aufgelöste Route ist der Pfad, den der Ruf nimmt).
* **`mux.c`, `session_row()`** — hängt `<path>` als **letztes** Feld an
  (`-` wenn keiner), genau wie die WAMPES-Zeile.  Der `listener_row()` bekommt
  ebenfalls ein abschließendes `-`, damit die Feldzahl gleich bleibt.
  `parse_sessions()` (`agwpe_client.c:937`) liest nur die ersten zehn Felder
  (`sscanf(…) < 7` = Zeile verwerfen), ein elftes wird ignoriert — **kein
  Bruch** für den bestehenden Leser; die Anzeige des Feldes ist Schritt 3.

**Eingehend: die AGWPE-Seite hat wirklich keinen Pfad.**  Ein eingehender
Connect kommt als `'C'` mit `data_len = 0` und ohne Digi-Liste; das
AGWPE-Protokoll (AGWPE/Direwolf) überträgt den Pfad einer *empfangenen*
Verbindung nicht.  netd könnte ihn nur aus den Roh-Monitorframes (`'K'`)
rekonstruieren, indem es das verbindungsaufbauende SABM dem Link zuordnet —
ein eigenes, größeres Stück (Frame-Ebene, vgl. §4c).  Bis dahin zeigt eine
eingehende Funk-Session `-`, und eine selbst initiierte ihren Pfad.

**Der `loop` bleibt außen vor:** `loop_connect()` verwirft die Digis weiterhin
bewußt (`mux.c:818-819`) — auf dem Loop gibt es kein Funk, ein Via-Pfad wäre
dort bedeutungslos.

### 8.11 Eingehender Pfad: aus den Roh-Monitorframes (2026-10-15)

Thomas: *„AGWPE ist schon ein blödes Protokoll.  Ich sehe dass ich connected
wurde, aber nicht von wem.  Dazu müsste ich in den Monitor-Channel schauen."*
— und dann: **„ja, Eingehender Pfad und Anzeige bauen wir."**  §8.10 hatte das
als „eigenes, größeres Stück" auf die lange Bank geschoben; hier ist es.

**Was genau fehlt.**  *Wer* gerufen hat, steht sehr wohl im `'C'`:
`call_from` = Rufender, `call_to` = angerufene (lokale) Call; netd sucht den
Client genau darüber (`hdr->call_to`, §8.9).  Was fehlt, ist der **Weg** —
„über wen".  Und den überträgt ein eingehender Connect nicht: das `'C'` hat
`data_len = 0` und keine Digi-Liste (§8.10).  Genau dafür ist der
Monitor-Kanal da.

**Der Rohstrom liegt schon an.**  netd abonniert die Roh-Frames (`'K'`) des
Upstreams ohnehin, solange die Heard-Liste läuft — und die ist Vorgabe:

    mux.c, mux_recalc_toggles():    want_k = ax25netd.mheard;

Ohne lauschenden `listen(1)`, aber **mit** mheard (Default) ist der Rohstrom
also immer an, und `case 'K'` wertet ihn bereits aus
(`ax25netd_mheard_frame()`).  Ein eingehendes Link-Setup kommt damit als
`'K'`-Frame **mit vollem Adreßfeld** an — bis zu 8 Digis.

**Frame-Aufbau** (wie `mheard.c` liest und `mux_mirror_raw_ctl()` baut):
`[KISS-Marker][dest(7)][src(7)][digi(7)]…[ctl][pid][info]`, die **letzte**
Adresse trägt das E-Bit `0x01` in Byte 6.  `ax25_ntoa()` liefert dabei
denselben druckbaren Ruf wie im `'C'` (die H/RR/SSID-Bits stören nicht), hat
aber **einen statischen Puffer** — jeder Ruf wird sofort kopiert.

**Richtung stimmt ohne Drehen.**  Ein *empfangener* Frame trägt `dest` = lokal
(angerufen), `src` = fern (Rufender); die Digis stehen in **Fahrtrichtung**,
also Rufender → lokal — genau die Richtung, in der die Zeile liest
(`FROM` = Rufender → `TO` = angerufen, §8.7).  Kein Umdrehen nötig; das
unterscheidet den Fall vom selbst geöffneten Link (§8.8).

**Die Reihenfolge `'K'` ↔ `'C'` ist offen** — also beide Wege abgedeckt:

* **Session existiert schon** (das `'C'` war zuerst da): der `'K'`-Handler
  setzt den Pfad direkt — aber **nur** bei `remote_initiated` und nur, wenn
  noch keiner steht.  Die Beschränkung auf `remote_initiated` ist wesentlich:
  ein *selbst* geöffneter Link hat seinen Pfad aus dem `'v'` und dürfte ihn
  nicht aus einem Rückrichtungs-Frame (fern → wir, also in `TO`→`FROM`-Reihen-
  folge) überschreiben.
* **Frame zuerst** (die Session gibt es noch nicht): der Pfad wird am Upstream
  kurz gehalten (`pending[]`, 8 Plätze je Upstream, TTL 30 s, ältester Platz
  wird wiederverwendet) und beim Anlegen durch `'C'`/`'D'` verbraucht.

Ein `'K'`-Frame, dessen `dest` **kein** registriertes Rufzeichen ist, ist
Fremdverkehr im Monitor und wird ignoriert (kein Halten, kein Setzen) — nur
Frames an eine Call, die ein Client hält (`client_by_call()`), zählen.

**Nicht betroffen:** der `loop` (kein Funk, kein `'K'`) und selbst geöffnete
Links (Pfad aus `'v'`, §8.10).  Die netd-Zeile führt den Pfad schon als
letztes Feld (§8.10) — mit dieser Ergänzung füllt sich der eingehende Fall von
selbst; die **Anzeige** ist unverändert und kommt mit Schritt 3.

**Laufzeit-Beleg (2026-10-15).**  Ein Fake-Upstream (`rowtest/up_path.py`)
schickt die Roh-Frames so, wie sie kämen — beide Reihenfolgen, ein Decoy und
acht Digis —, dazu der übliche lokale Via-Connect.  Die gedruckte Tabelle
(`rowtest/run_path.sh`, netd `--no-mheard`, damit nur dieser Pfad geprüft
wird):

| Fall | erwartet | bekommen |
|---|---|---|
| lokal: `v` DL9SAU→DB0AAA via DB0XXX,DB0YYY | `FROM=DL9SAU TO=DB0AAA` `DB0XXX,DB0YYY` | so |
| eingehend, `'K'` **vor** `'C'` (DB0ABC via DB0AAA,DB0BBB) | `FROM=DB0ABC TO=DL9SAU` `DB0AAA,DB0BBB` | so (Wartetabelle) |
| eingehend, `'C'` **vor** `'K'` (DB0DEF via DB0CCC) | `FROM=DB0DEF TO=DL9SAU` `DB0CCC` | so (direkt) |
| Rückrichtung auf **unserem** Connect (DB0AAA→DL9SAU via DB0ZZZ) | Pfad **bleibt** `DB0XXX,DB0YYY` | so (nicht überschrieben) |
| eingehend mit **8** Digis (DB0GGG) | alle 8, in Fahrtrichtung | `DB0D1,…,DB0D8` |
| Decoy: `'K'` an DB0PPP (hält niemand) | keine Zeile | keine |

Der Lauf ist zusätzlich unter **AddressSanitizer** sauber (kein Befund); `path`
faßt 8 maximale Rufe (= 79 Zeichen) in den 88-Byte-Puffer.  `mux.c` übersetzt
ohne neue Warnung (nur das vorbestehende `loop.c:519`).

### 8.12 Anzeige-Zuordnung: `UPSTREAM` und `PORT` (2026-10-15)

Thomas zu meinem Befund „die netd-Zeile hat `<up> <chan>`, die WAMPES-Zeile
nicht":

> wir haben mehrere radios im upstream, und Portnummern.  `<up>` ist eigentlich
> der ax25/axports Eintrag.  Bei wampes eigentlich vergleichbar: es kann
> theoretisch mehrere wampes-Instanzen geben.  `<up>` ist wampes (benutzen wir
> ja schon bei `ax25netctl -k wampes:5` zum Beenden einer session).  Und was
> Portnummer eines agwpe-Servers ist, ist bei wampes der interfacename.

Das ist keine neue Anforderung, sondern die **Bestätigung**, daß die beiden
Zeilen dieselben zwei Begriffe meinen — und beide stehen im Code:

* WAMPES' `<port>` ist **schon** der Interface-Name:
  `axlisten_portname(axp)` bzw. `"netrom"` (`axserver.c:1907,1948`).
* netd's `<up>` ist der Upstream-Name, der ein `axports`-Eintrag sein **muß**
  (sonst warnt der Daemon: *„upstream 'hf' is configured but no entry in
  …/axports names it"*); `<chan>` ist nur der Kanal *innerhalb* des Upstreams,
  und die flache Portnummer trägt ihn schon (`port = i*16 + chan`,
  `ax25netd.8` §PORT NUMBERS).

Zuordnung für die gemeinsame `-s`-Tabelle (Spalten wie bisher:
`ID PORT FROM TO PROTO STATE UPSTREAM PID PROG`):

| | netd-Zeile | WAMPES-Zeile |
|---|---|---|
| **ID** | die Id (`5`) | Instanz + Id (`wampes:5`) |
| **PORT** | die flache Portnummer (`0`) | der **Interface-Name** (`hf`, `netrom`) |
| **UPSTREAM** | der Upstream-Name = der `axports`-Eintrag (`hf`) | die **Instanz** (`wampes`) |

Beide „oberen" Namen sind vergleichbar: netd kann mehrere Upstreams haben
(und mehrere netd-Instanzen stapeln, `ax25netd.8` §STACKING), WAMPES
theoretisch mehrere Instanzen.  `<chan>` bekommt **keine** eigene Spalte, es
geht in der Portnummer auf.

**Folge für Schritt 3:** die `PORT`-Spalte ist **keine Zahl mehr**, sondern eine
Zeichenkette — netd druckt die Nummer, WAMPES den Interface-Namen.  Die
`ID`-Spalte wird für WAMPES mit der Instanz präfixiert, damit der Kill-Handle
in einer Tabelle mit beiden Backends eindeutig bleibt.

## 9 Umsetzung Schritt 2 — die Brücke (`wampes_ioctl`) (2026-10-15)

`SIOCAX25CTLCON` wird jetzt auch vom WAMPES-Backend bedient.  Umgesetzt in
`libax25.git`:

* **`wampes.c`** — `wampes_ioctl()` und seine Helfer `wampes_kill()`,
  `wampes_row_handle()`, `wampes_pair_is()`;
* **`wampes.h`** — Deklaration;
* **`axsock.c`** — `AXSOCK_ENTRY(ioctl)` fragt `wampes_ioctl()` **vor**
  `agwpe_ioctl()`;
* **`agwpe_sock.c`** — der Kommentar „the node has no equivalent of
  SIOCAX25CTLCON" ist weg (er stimmt nicht mehr).

**Warum über den *Port*, nicht über den Fd.**  `axkill(8)`/`axctl(8)` legen
den Socket an und binden ihn **nicht** (`axkill.c`:109, `axctl.c`:162).  Ohne
Port hat ihn kein Backend übernommen — im „auto"-Verfahren ist es ein
AGWPE-Socket, kein WAMPES-Socket, `axsock_find_locked()` findet ihn also nicht.
Was das Backend benennt, ist der **Port in der Anfrage**: `ctl->port_addr` ist
das Rufzeichen eines axports-Eintrags, der Eintrag wird zum Namen aufgelöst
(`ax25_config_get_port()`, mit vorherigem `ax25_config_ports_ensure()` — die
Port-Tabelle gehört dem Programm, s. `axconfig.c`:589) und der Name über
`wampes_node_addr()`/`wampes_address()` zum Node.  Kein Node ⇒ `0` zurück,
das nächste Backend antwortet.  `WAMPES_SOCKET` gewinnt weiterhin.

**Ein Kommando, das wir nicht können, wird abgelehnt, nicht durchgereicht.**
`axctl(8)` schickt T1, N2, window … durch denselben ioctl.  Ein
Durchreichen würde im „auto"-Verfahren das AGWPE-Backend antworten lassen —
für eine Session, von der ax25netd nichts weiß.  Also `EOPNOTSUPP` für alles
außer `AX25_KILL`.

**Der Kill selbst** ist genau die Symmetrie zu `'Q'` KILL: `sessions` lesen
(bis zur `***`-Zeile), die Zeile mit dem Paar `{source_addr, dest_addr}`
suchen — in **beiden** Reihenfolgen, weil der Kernel auch beide akzeptiert —
und `close <handle>` schicken.  Eine `-`-Id (geclaimtes Rufzeichen) wird
übersprungen, sie ist keine Session.  Kein Treffer ⇒ `ESRCH`.  Die Reads
warten beliebig lange, wie die Connect-Quittung auch: der Node antwortet
sofort, und ein Kommando, das er nicht kennt, lässt ihn die Verbindung
schließen (`remote_net.c` `delete_controlblock`) — Warten kann hier also nicht
ewig warten.

**Kompiliert und gelinkt** (macOS, `-std=gnu23 -Wall -Wextra`): `wampes.c`,
`axsock.c`, `agwpe_sock.c` ohne neue Warnung; `rebuild_lib.sh` baut die
Shared Library und die Testprogramme.

**Laufzeit-Beleg (2026-10-15):** ein Mock-Node (Unix-Socket, spricht
`sessions`/`close`) und ein Programm, das `wampes_ioctl()` **direkt** ruft
(nicht über den interponierten `ioctl`, so muss die Library keine
Symbol-Suche gewinnen).  Ergebnis:

| port / Anfrage | erwartet | bekommen |
|---|---|---|
| `DL9SAU-1`, Paar in umgekehrter Reihenfolge | `close 3` | `handled=1 ret=0`, Mock: `CMD close 3` |
| dasselbe Paar in gerader Reihenfolge | `close 3` | `handled=1 ret=0`, Mock: `CMD close 3` |
| Paar, das keine Zeile hat | `ESRCH` | `handled=1 ret=-1` errno 3 |
| Paar wie die `-`-Zeile („geclaimt") | `ESRCH`, kein `close` | `handled=1 ret=-1` errno 3 |
| `AX25_T1` auf WAMPES-Port | `EOPNOTSUPP` | `handled=1 ret=-1` errno 102 |
| Port ohne Node (`LOOPA`) | durchreichen | `handled=0` |
| kein `SIOCAX25CTLCON` | durchreichen | `handled=0` |

Der Mock sah in den beiden Treffer-Fällen genau `sessions` und `close 3`, in
den anderen nur `sessions` — es wurde also nichts geschlossen, was nicht
getroffen war.  Kein echter Node nötig: `WAMPES_SOCKET` zeigt auf den Mock,
`wampes.conf` liefert nur den Node-Namen.

## 10 Umsetzung Schritt 3 — Anzeige und Kill in `ax25netctl` (2026-10-15)

§7 liefert die WAMPES-Zeile, §9 den Weg zu ihrem Kill; Schritt 3 bringt beides
in das Werkzeug.  Umgesetzt in `libax25.git` und `ax25-apps.git`:

**`libax25.git/netax25/wampes_client.h` (neu)** — die Bibliotheksseite des
Gesprächs mit dem Node.  Alle Funktionen liefern `0` bei Erfolg und `-1` mit
`errno` (`ECONNREFUSED` für einen Node, den es nicht gibt, `ESRCH` für eine
Session, die er nicht hat):

* `wampes_node_count()` / `wampes_node_name(i)` zählen die in `wampes.conf`
  genannten Nodes auf, wie `ax25_config_get_next()` die Ports aufzählt — bis
  der Name `NULL` kommt, was auch die Antwort für eine Maschine ohne Datei
  ist.  Ein Node wird über seinen **Namen aus der Datei** benannt, nicht über
  seinen Socket-Pfad; `WAMPES_SOCKET` und der einkompilierte Vorgabewert
  erreichen einen Node, den die Funktionen nicht kennen — so zeigt der
  Mock-Test auf einen Node, der in keiner Datei steht.
* `wampes_sessions(node, cb, arg)` liest die Tabelle und gibt **jede** Zeile
  an `cb`, **ohne** die abschließende `***`-Zeile; ein Rückgabewert ungleich
  0 von `cb` beendet den Lauf vorzeitig (und die Tabelle gilt als gelesen,
  weil der Aufrufer es so wollte).  Gefragt wird **einmal je Node**, nicht je
  Port: alle Ports eines Nodes antworten aus derselben Liste.
* `wampes_close_session(node, handle)` schickt `close <handle>`.  `ESRCH`
  für eine Weiterleitung, die es nicht gibt, und ebenso für eine Claim-Zeile
  (`-`) — die ist keine Weiterleitung.

Der Header sagt selbst, daß **die Zeile dem Node gehört**: Software soll
nicht auf das Feldformat bauen außer auf das, was sie druckt.  `Makefile.am`
installiert ihn mit.

**`ax25-apps.git/ax25netctl`** — `show_sessions(int with_netd)` druckt beide
Backends in **eine** Tabelle.  netd wird zuerst gefragt, seine Zeilen kommen
zuerst; die WAMPES-Nodes folgen.  Ein Backend, das nicht antwortet, ist ein
Grund für Rückgabewert 1 — aber nicht dafür, das andere auszulassen: eine
Maschine ohne ax25netd kann trotzdem zeigen, was ihr Node trägt, und
umgekehrt.  `print_session_row()` druckt die gemeinsame Zeile; die Feldfolge
ist die aus §7, der Pfad geht als Fortsetzungszeile unter `FROM`.

* **PORT ist keine Zahl mehr** (§8.12): netd druckt die flache Portnummer,
  WAMPES den **Interface-Namen** (`hf`, `netrom`).  Beide kommen als
  Zeichenkette an, der Drucker sieht die Herkunft nicht an — genau der Punkt,
  den §8.12 verlangt.
* **Die ID trägt den Node** (`wampes:5`): der Kill-Handle muß in einer
  Tabelle, die beide Backends führt, eindeutig bleiben.  Eine Claim-Zeile
  behält die `-` (sie hat keinen Handle).
* **UPSTREAM** ist bei netd der Upstream-Name, bei WAMPES die **Instanz**
  (der Node-Name), wie §8.12 festhält.
* **`-k <node>:<id>`** — die neue zweite Handle-Form.  Sie wird von
  `port:dest src` danach unterschieden, was hinter dem **letzten**
  Doppelpunkt steht: ein Handle ist ein Wort aus lauter Ziffern
  (`all_digits()`), eine Destination ein Rufzeichen, und `port:dest` führt
  sein `src` ohnehin als zweites Wort — die beiden sind nicht zu verwechseln.
  `kill_wampes_id(node, id)` schließt die Weiterleitung über die Bibliothek.
* **`-L`** heißt weiter „ax25netd nicht fragen" — ein WAMPES-Node ist kein
  ax25netd, also listet `-s -L` ihn **weiter**.  Das ist der Weg, auf einer
  Maschine ohne Daemon die Node-Sessions zu sehen.
* Die `-s`- und beide `-k`-Handle-Formen antworten **ohne** die
  axports-Datei; nur das ältere `-k port:dest` braucht sie, weil es den Port
  auflösen muß.

**Belegt:** kompiliert, gebunden und gegen Mock-Nodes end-to-end geprüft
(beide Backends in einer Tabelle, `-k wampes:5` schließt genau die getroffene
Zeile, `-k wampes:99` → `ESRCH`, eine Claim-Zeile hat id `-` und keinen
Handle, `-s -L` listet den Node ohne netd).

## 11 Entschieden (Thomas, 2026-10-15): (C), (E), (F) — WAMPES wird rohe Monitor-Quelle

### 11.1 Warum nicht mehr spiegeln

(C) und (E) aus §3 waren mit 4e (Synthese im Shim) nur notdürftig erfüllt: der
Shim **erfindet** Frames — ein SABM zum `connect()`, ein UA zum `accept()`,
ein DISC zum `close()`, und für UI die Kopie dessen, was über die
Unix-Verbindung ging.  Das ist zweimal halb wahr: ein Monitor sieht Frames,
die nie auf dem Draht waren, und was auf dem Draht war, sieht er nicht.

Thomas hat den Weg entschieden: **WAMPES liefert seinen AX.25-Verkehr roh**,
so wie ein Kernel-Port ihn über `AF_PACKET` liefert.  Dann braucht niemand
AGWPE zu lernen, und niemand erfindet Frames; `listen(1)` und die Heard-Liste
bekommen die Originale.  **Kein AGWPE-Direktionsbit, keine
Protokolländerung**: der Monitor ist ein **eigener** Kanal, kein neuer Rahmen
im AGWPE-Strom — deshalb kann er auch nichts brechen, was AGWPE-Kompatibilität
heißt.

### 11.2 Die Hooks: `ax_recv()` und `axsend()`, nicht `dump()`

* **IN**: `ax_recv()` (`wampes.git/src/ax25.c`) ist `rcvf` jedes AX.25-Ports —
  der sieben seriellen aus `config.c` und dazu axip/axudp, axtcp/kisstcp,
  bpqether über `setencap(ifp,"AX25UI")`.  Ein Hook dort erfaßt die ganze
  Familie; acht Treiberhooks täten es nicht und wären eine Stelle, die der
  neunte Port-Typ nicht kennt.
* **OUT**: `axsend()` ebenso für die Senderseite.
* **Bewußt nicht `dump()`**: das ist die Trace-Ausgabe des Nodes selbst, an
  ihr Format und ihr Publikum gebunden — kein Ort für eine zweite Ausgabe.

Beide Hooks sind ein Längenzählen und ein nicht-blockierendes Schreiben, und
nur, **wenn ein Konsument da ist** (§11.4): ohne Konsument kosten sie nichts.

### 11.3 Der Anschluß: ein eigenes `sockets/monitor`

`Listeners[]` in `remote_net.c` hat zwei **echte** Einträge und einen
Sentinel (`for (l = Listeners; l->name; l++)`): den Kommando-Kanal
(`.sockets/netcmd`, Socket 0600, Verzeichnis 0700) und den Dienst-Socket
(`sockets/ax25`).  Der Monitor
bekommt einen **dritten echten** Eintrag und damit einen vierten als Sentinel.

Warum getrennt: der Dienst-Socket entscheidet, wer **verbinden** darf; der
Monitor entscheidet, wer **zuschauen** darf.  Das sind zwei Fragen mit zwei
Antworten, und wer verbinden darf, darf deshalb noch lange nicht zuschauen —
deshalb zwei Tore und nicht eins.

* **Rahmen**: `[Länge][Interface-Name][0x00][AX.25-Frame]`.  Die Länge, weil
  ein AX.25-Frame NUL-Bytes enthält und die Grenze sonst nicht zu finden
  wäre; der Interface-Name, weil ein Node mehrere Interfaces hat und ihn ein
  Kernel-Monitor ebenso mitgibt.  `libax25` setzt `<node>:` davor, so daß
  `listen` `wampes:hf` zeigt (ein Kernel-Monitor zeigt `hf`).
* **uid/gid vom Socket werden nicht ausgewertet** — es gibt keine Filterung
  nach Benutzer; das Tor ist die Datei, genauer der Modus des Sockets (0660
  und die Gruppe von `sockets/`; §11.12).
* **Der Monitor blockiert den Node nie**: ein Kanal, der nicht liest, verliert
  Frames — er staut sie nicht, und er wird auch nicht hinausgeworfen.  Die
  Regel aus `axlisten_ui_deliver()` („ein Monitor-Frame, der nicht geschrieben
  werden kann, kostet den Client alles, Logins inklusive") gilt hier
  **nicht**: der Monitor hält nichts, also darf er verlieren.

### 11.4 Der Handshake: `monitor [iface=<list>] [input|output]`

* **Default `input`** (nur RX).
* **`input`** = auf dem Port **Angekommenes** — im Kernel `ETH_P_AX25`, der
  Vorgabewert von `listen`, und genau das, was `mheard` will.
* **`output`** = zusätzlich **unser eigenes TX** — im Kernel `ETH_P_ALL`,
  `listen -a`.
* Das Vokabular ist das von `trace` (`IF_TRACE_IN` / `IF_TRACE_OUT`), damit
  dieselbe Sprache an beiden Enden gesprochen wird.
* **`iface=<list>`** übernimmt die Liste, die `listen ax25 add` schon kennt
  (`axserver.c`): Komma-getrennt, `!` vor der ganzen Liste schließt aus,
  `all` ist alles — und nichts anzugeben ist ebenfalls alles.
* Gefiltert wird **serverseitig**: wer nur `input` bestellt, dem wird nichts
  gesendet, statt beides zu senden und den Rest am Client zu verwerfen.

### 11.5 Die Empfangsseite: promiskuitiv pro Interface (die Korrektur)

Der naheliegende Weg wäre, es wie `ax25_rcv` zu machen: Ziel-Call ansehen,
PID ansehen, dann zustellen.  **Das ist nicht der Monitor.**  Ein Monitor
sieht den Verkehr des Interfaces, nicht eine Zustellung an einen Halter.

Der Beweis ist APRS: `A>APRS,WIDE2-2` bzw. `A>APRS,DB0AAA,WIDE2-1*` — das
Ziel ist `APRS`, nicht unser Ruf.  Wer auf `iface wampes:aprs` hören darf,
muß **den ganzen Verkehr dieses Interfaces** sehen, nicht nur, was an eine
individuell geclaimte Call gerichtet war.  Deshalb: **kein
Callsign-Matching auf der Empfangsseite.**

Der einzige Filter ist **Typ, nicht Adresse**: ein `SOCK_DGRAM`-Konsument
bekommt UI mit PID und keine I-Frames (dieselbe Prüfung, die der
Kernel-Monitor für Daten-Sockets macht).  Weil der Filter nur am Typ hängt,
ist die Leseseite eines Datagram-Sockets dieselbe wie die eines Monitors.

> **Nebenbefund, der eine Annahme geradezieht.**  `axlisten_ui_deliver()`
> macht heute `addreq(lp->call, hdr->dest)` — es stellt eine UI nur zu, wenn
> das Ziel die geclaimte Call ist.  Das ist die **Datagram-Zustellung an
> einen Client** und bleibt dort richtig; für den Monitor fällt es weg.  Der
> APRS-Fall zeigt, warum: es hätte nie getroffen.

**Was das für die Rechte heißt.**  Wer `sockets/monitor` lesen darf, sieht
den ganzen Verkehr der Interfaces, die er mit `iface=` genannt hat.  Die
Sicherheitsgrenze ist also „darfst du zuschauen", **nicht** „welche Rufe
darfst du sehen" — und genau dafür ist das eigene Rechte-Tor da (§11.3).
Daß ein netd- oder Kernel-Interface sich anders verhält, ist **noch zu
prüfen, nicht anzunehmen**.

### 11.6 (C): was bleibt, was fällt

`(C)` war „Nicht-I-Frames → netd → mheard, ohne WAMPES zu belasten".  Mit der
rohen Quelle kommt der Frame **schon** original an — die Synthese wäre eine
zweite, konkurrierende Quelle desselben Geschehens.

* **Fällt aus `libax25.git/wampes.c`**: die `WAMPES_CTL_*`-Werte
  (SABM/UA/DISC) und alles `wampes_mirror_*` — der UI-Spiegel **und** die
  synthetisierten Link-Frames, samt `wampes_mirror_client`,
  `wampes_mirror_open()` und ihren Aufrufen in `wampes_connect()`,
  `wampes_accept()` und `wampes_close()`.
* **Fällt aus `ax25-apps.git/ax25netd`**: der `(C)`-Weg loop→mheard, der den
  Loop bisher synthetisch mitgespeist hat — zurücknehmen.
* **Der Loop-Port bleibt** (`AGWPE_PORT_LOOP`, Port 8200); nur der
  UI-Mirror-Hack geht.

### 11.7 (F): der Pfad

`(F)` war „via-Pfad je Session merken und rotieren".  Das diente dem Spiegel:
`struct wampes_sock` bekam `path[]`, `npath` und `we_called`, und
`wampes_mirror_setup()` drehte den Pfad je Richtung.

Was **bleibt**, ist die Weitergabe des Pfades an den Client: `wampes_accept()`
liest ihn schon immer aus der Accept-Zeile des Nodes in `him`
(`fsa_digipeater[]`), und `ax25d` fragt ihn dort ab (`AX25_PATH`).  Diese
Weitergabe gab es vor `(F)` und sie hängt nicht am Spiegel.

Was **mit `(C)` fällt**, sind `path[]`/`npath`/`we_called` in
`struct wampes_sock` und ihre Leser im Spiegel — mehr Leser hatten sie nicht.
(Die `we_called`-/Pfad-Logik der WAMPES-**Session-Zeile**, §8.7/§8.8, ist
etwas anderes und bleibt.)

### 11.8 Zweiter, unabhängiger Schritt: roh senden — `PF_PACKET`, nicht `sockets/ax25` (2026-10-17)

**Die Vorfrage hat der Kernel beantwortet.**  Der erste Entwurf wollte das
rohe Senden als Kommando auf dem **bestehenden** `sockets/ax25` (dem
Dienst-Socket) — die binäre Variante der `datagram`-Route.  Beim Nachlesen
der Kernel-Schnittstellen (siehe `LINUX-AX25-SOCKETS.de.md`) fällt das weg:
im Linux-Kernel ist rohes Senden **kein** `AF_AX25`-Vorgang.

* Ein `AF_AX25`-Datagramm-Socket hat das Interface am **Socket**
  (`bind()` beziehungsweise `SO_BINDTODEVICE`); sein `sendto()` trägt nur
  Zielruf und Pfad.  Ein Interface **je Paket** gibt es dort nicht, und
  `recvfrom()` nennt gar keins.
* Rohes Senden ist `AF_PACKET` + `ETH_P_AX25`: `sendto()` mit dem
  Interface je Paket, den Rahmen **komplett selbst gebaut** (Adressen +
  Ctl + PID + Nutzlast), direkt über `dev_queue_xmit` — kein
  Rufzeichen-Lookup, kein `ax25_cb`, keine LAPB-Zustandsmaschine.

**Damit ist §11.8 die Sendeseite von `(E)`**, nicht dessen Nachbar: der
Monitor ist bereits die **Empfangsseite** desselben Sockeltyps (`AF_PACKET`
RX, `ETH_P_AX25`/`ETH_P_ALL`, §11.3/§11.5), und §11.8 macht denselben Kanal
**schreibbar**.  Der Rahmen ist derselbe wie der des Monitors:

    [4 Byte Länge, big endian][Interface-Name][0x00][AX.25-Rahmen]

„Was der Monitor liefert, kann man zurückschicken" — genau das ist der
transparente Zugriff auf das Interface, den ein IP-, NET/ROM-, ROSE- oder
APRS-Programm braucht.  Und das Interface kommt wie beim Kernel mit
**jedem** Paket statt mit dem Socket: ein einziger Kanal kann auf mehreren
Ports senden.

**Der Gewinn gegenüber `datagram`.**  Dort gilt die Kernel-Bedeutung: der
PID ist der des Sockets, und ein Datagramm-Socket sieht ausschließlich
`AX25_P_TEXT` (0xF0).  Der Monitor-/§11.8-Kanal dagegen trägt den PID im
Rahmen — jeder PID ist sichtbar und wählbar, und gefiltert wird **im
Knoten** statt in jedem Client (`LINUX-AX25-SOCKETS.de.md` §1, §5, §6).

**Was das nicht ist.**  Keine zweite LAPB-Zustandsmaschine und kein Ersatz
für `datagram` oder die Sitzungen: `datagram` bleibt das
rufzeichen-orientierte UI-Senden, die Sitzungen bleiben die I-Frame-Seite.
`ax_send_ui()` in `wampes.git/src/ax25.c` bleibt der Sendeausgang für den
UI-Fall (siehe §11.9).

**Warum ein eigener Sockel und kein Kommando auf `sockets/ax25`.**  Der
erste Entwurf hatte es dort.  Möglich ist es, aber drei Gründe sprechen
dagegen:

* **Die Vollmacht.**  Wer `sockets/ax25` darf, darf **verbinden und
  lauschen**.  Wer rohe Pakete einspeisen darf, bekommt eine andere,
  stärkere Vollmacht.  Am Dienst-Sockel hängt beides untrennbar zusammen,
  weil die Dateirechte die *Verbindung* öffnen und nicht die Absicht
  prüfen.  Getrennt läßt sich „verbinden/lauschen erlauben, einspeisen
  nicht" überhaupt erst ausdrücken.
* **Die Form.**  `sockets/ax25` ist Kommando und Dialog: zeilenweise,
  textuell (`datagram` schaltet danach auf „jede weitere Zeile ist ein
  Rahmen").  Der Rohkanal ist ein **binärer Rekordstrom in beide
  Richtungen**.  Zwei Formen auf einer Verbindung sind zwei
  Rahmenprotokolle und ein Modus, den man erklären muß.
* **Die Art des Sockels.**  `sockets/ax25` ist ein Dienst, der etwas
  aushandelt.  Der Rohkanal ist ein **Fenster auf die Schnittstelle**, mit
  dem Interface am *Paket* statt an der Verbindung — dieselbe Art wie der
  Monitor.

**Der Name und die Rechteleiter.**  Der Sockel heißt `sockets/packet`
(nach `PF_PACKET`, der Semantik, die wir übernommen haben) und ist **kein
Nur-Schreib-Sockel**: er liest und schreibt wie sein Kernel-Vorbild.
`sockets/monitor` bleibt und ist die **Nur-Lese-Sicht** auf denselben
Strom — dieselben Rekorde, dieselben Optionen, aber der Knoten nimmt dort
keine Rekorde entgegen (eine Antwort, keine stille Verwerfung).  Damit ist
jede Sprosse eine Datei mit eigenem Modus und eigener Gruppe:

    sockets/ax25      verbinden, lauschen
    sockets/monitor   zusehen
    sockets/packet    zusehen und einspeisen

Ein Programm, das Kernel-Treue will, bekommt die zwei Verbindungen im Shim
hinter *einem* Deskriptor gebündelt — und muß dafür nichts umschreiben,
denn der Wille steht im Syscall (§11.9).

### 11.9 Randbedingungen und offene Punkte

* **Umfang**: alle AX.25-Interfaces (die `ax_recv()`-Familie), nicht nur der
  Loop — und das gilt jetzt für **beide** Richtungen, RX wie TX.
* **`MAXDIGIS` = 8** (§8.4) — der Rohtraffic trägt bis zu acht Digis.
* **Keine CPU-Last, die das Radio-Bitrate stiehlt**: der Hook zählt eine
  Länge und schreibt nicht-blockierend, und nur mit Konsument (§11.4).
  Für die **Sendeseite** heißt das umgekehrt: der Knoten darf nicht
  blockieren, wenn ein Client langsam schreibt — dieselbe
  Backpressure-Regel wie beim Monitor (`axmon_write`, §11.3).
* **macOS-Warnungen dürfen keine Linux-Warnungen einführen** (und umgekehrt):
  die portablen Teile bleiben portabel, die `SO_PEERCRED`/`LOCAL_PEERPID`-
  Verzweigung aus §10 ist das Muster.

**Offen (Sendeseite).**

* **UI zuerst.**  Der Kernel-`AF_PACKET`-Weg erlaubt jeden Rahmen; ein
  I-Frame von außen würde aber die LAPB-Zustandsmaschine des Knotens
  umgehen und den Partner verwirren.  Vorschlag: erst UI zulassen
  (`ax_send_ui()`), alles darüber ist eine eigene Entscheidung.
* **Die Sendeseite ist ein eigener Sockel** (`sockets/packet`, §11.8).
  Drei Gestalten standen zur Wahl — (a) den Monitor schreibbar machen,
  (b) ein eigenes Wort auf demselben Listener, (c) ein eigener Sockel mit
  eigenen Rechten — entschieden ist **(c)**, und zwar wegen der
  **Vollmacht**: §11.12 hat den Monitor bewußt auf 0660 mit der Gruppe von
  `sockets/` gesetzt, und bei einem einzigen Sockel kann jeder Leser auch
  senden (`connect()` verlangt Schreibrecht).  Ein eigener Sockel trägt
  eigenen Modus und eigene Gruppe; `sockets/monitor` bleibt die
  Nur-Lese-Sicht auf denselben Rekordstrom.
* **Entschieden und gebaut (§11.17).**  Vorgabe-Modus und -Gruppe von
  `sockets/packet`: **0600** mit der Gruppe von `sockets/`
  (`inherit_dir_group()`, wie der Monitor) — der gefährlichste Griff des
  Knotens wartet auf den Sysop, und eine `chgrp` allein öffnet ihn nie.
  Der Handshake ist **wortgleich zu §11.4**: dasselbe Wort `monitor`,
  dieselben Optionen (`iface=`, `input`, `output`); `iface=` wirkt hier
  als Erlaubnisliste fürs Einspeisen, Vorgabe: alle AX.25-Interfaces.
* **Der Shim führt beide Richtungen hinter *einem* Deskriptor**
  (entschieden 2026-10-17).  Kein Programm soll umgeschrieben werden
  müssen, um zwei Sockets zu öffnen: der **Wille steht im Syscall**, und
  der Shim sieht ihn.  Er tut das heute schon für die Leseseite —
  `agwpe_socket_packet()` fängt `socket(PF_PACKET, SOCK_PACKET, …)` ab
  (`SOCK_PACKET` ist die Marke, nicht die Adressfamilie: `listen` fragt
  `AF_PACKET`, `net2kiss` fragt `AF_INET`), `recvfrom()`/`read()` landen
  in `agwpe_recvfrom()` → `agwpe_mon_read()` und schreiben den
  Schnittstellennamen aus `sa_data` zurück.  Künftig landen
  `sendto()`/`send()`/`write()` in einem `s->raw`-Zweig von
  `agwpe_sendto()`: eine fd, beide Richtungen, wie im Kernel.

  Die Aufteilung bleibt **im** Shim.  Gelesen wird auf der bestehenden
  Monitor-Verbindung; die **erste** Schreiboperation öffnet
  `sockets/packet` daneben (lazy) und schreibt dort.  Ein reiner Zuhörer
  (`listen`, unprivilegiert) braucht damit nie die Einspeise-Vollmacht,
  und wer nur `sockets/monitor` darf, bekommt auf `sendto()` ehrlich
  `EPERM` — dieselbe Antwort, die der Kernel ohne `CAP_NET_RAW` gibt.
  Kein Poll-Umbau nötig: der Roh-fd des Programms ist ein interner
  Socketpair, dessen Leserichtung der Knoten speist; die Schreibrichtung
  geht über eine zweite Verbindung, die nur der Shim hält.

  **Die eine Lücke:** `agwpe_sendto()` kennt heute keinen `s->raw`-Zweig.
  Ein Schreiben auf einem Monitor-fd fällt bis auf `real_sendto()` durch —
  auf dem internen Socketpair, wo es nichts zu suchen hat.
* **AGWPE-Anwender mitdenken.**  Jede Fähigkeit, die wir der WAMPES-Seite
  geben, einmal daraufhin ansehen, ob sie auch einem AGWPE-Anwender nützt.
  AGWPE kennt Ports, aber kein Interface je Paket; der Gewinn dort liegt
  bei selbstgebauten Rahmen, der Sicht auf PIDs und der Transparenz — nicht
  im Nachbau der syscall-Semantik.

### 11.10 Umsetzung (libax25) und Laufzeit-Beleg (2026-10-16)

**Was gebaut wurde.**  Die Knoten-Seite (§§11.1–11.9) ist mit `b311251`
committet.  Auf der libax25-Seite sind es drei Funktionen in
`libax25/wampes.c` — `wampes_mon_open`, `wampes_mon_read`, `wampes_mon_close`
—, deklariert im internen `wampes.h`, und in `axmon.c` die dritte Quelle:
`axmon_open_mask()` öffnet sie als `AXMON_WAMPES` (jetzt `AXMON_MAX_FD` = 3),
`axmon_read()` gibt einen `AXMON_FRAMED_WAMPES`-fd an `wampes_mon_read()`
weiter, `axmon_source_name()` nennt sie „WAMPES monitor".  Die Quelle entsteht
**neben** Kern und Shim, weil ein Knoten kein AGWPE spricht: so wie die
Shim-Quelle auf einem Rechner ohne ax25netd nichts zu speisen hat, hat die
WAMPES-Quelle auf einem Rechner ohne Knoten nichts zu lesen.

**Der Record.**  `[4B Big-Endian Länge][Interface-Name][NUL][Frame]`, wie
`remote_net.c` ihn schreibt.  Der Leser liest die Länge, dann den Namen bis
zum NUL (begrenzt, damit ein Peer ohne NUL nicht ewig lesen läßt), dann den
Frame.  Zurück kommt ein Frame in derselben Form wie von Kern und Shim: ein
KISS-Datenbyte (`0x00`), dann der AX.25-Rahmen.  In `sa_data` steht
`<knoten>:<interface>` — der Name, den `listen` anzeigt und den `mheardd`
durch `ax25_config_get_name()` auflösen kann.

**Welchen Knoten ein Port meint.**  Ein Name vor dem Doppelpunkt, der einen
`wampes.conf`-Knoten nennt, öffnet diesen (`wampes_port_hook()`); die Adresse
kommt über `wampes_address(port)`, damit `WAMPES_SOCKET` gewinnt.  Aus
`<nodedir>/sockets/ax25` wird `<nodedir>/sockets/monitor` — das Geschwister;
ein anderer Name (TCP, Mock-`node.sock`) hat keinen Monitor und liefert 0.
Ohne Port (`NULL`/`""`) gilt `wampes_address(NULL)`: `WAMPES_SOCKET`, sonst
der einkompilierte Ort.  Der Knotenname fürs `<node>:`-Präfix kommt aus
`wampes.conf` über die Adresse, sonst `"wampes"`.

**Die Richtung** kommt aus dem Protokoll, das der Aufrufer (in
Netzwerk-Byte-Ordnung) übergibt, genau wie am Kern: `ETH_P_AX25` (0x0002) →
`input`, `ETH_P_ALL` (0x0003) → zusätzlich `output`.  Die Maske erreicht den
Knoten so wenig wie den Kern — sie wird debug-notiert, nicht übertragen.

**Eine Grenze weniger, als der Leser faßt.**  Der Leser hält
`AXMON_FRAME_MAX` (1500) Bytes Nutzlast und legt das KISS-Byte davor; der
Knoten kappte den Frame bei `> AXMON_FRAME_MAX`, also bis 1500 — eine Nutzlast
von 1501, eine zu viel.  Der Knoten kappt jetzt bei `>= AXMON_FRAME_MAX`, also
**≤ 1499** (`remote_net.c`).  Ein so langer Rahmen ist kein AX.25-Rahmen, und
so muß der Leser nie einen ganzen Rahmen verwerfen.

**Belegt (Laufzeit, macOS).**  Ein echter Knoten (`rig.py`, Port `a0`),
`WAMPES_SOCKET=<dir>/sockets/ax25`:

* **`moncli`** — genau der Weg von `listen`/`mheardd` (`axmon_open()`,
  `poll()`, `axmon_read()`): `axmon_open()` öffnet die WAMPES-Quelle
  (`kind=2`, `framed=2`).  Ohne `all` (`ETH_P_AX25`) kommt, was **B an A**
  sendet (`DB0BBB-1 -> DB0AAA-1`), und **nicht** A's eigenes Senden; mit
  `all` (`ETH_P_ALL`) kommt A's eigenes Senden (`DB0AAA-1 -> DB0BBB-1`) dazu.
  `sa_data` = `wampes:a0`, und `ax25_config_get_name("wampes:a0")` löst es auf
  (axports-Eintrag `wampes:a0`; ein Userspace-Port hat `Device == Name`).
  `-p wampes:a0` öffnet dieselbe Quelle.

* **`listen -a`** unter einem pty:

  ```
  listen: raw monitor via libax25 (WAMPES monitor, framed)
  wampes:a0: fm DB0AAA-1 to DB0BBB-1 ctl UI pid=F0(Text) len 11
  0000  hallo-von-a
  ```

* **`mheardd -f -F`** (Port `NULL`, also `WAMPES_SOCKET`), danach der
  `mheard`-Klient:

  ```
  Callsign   Port    Packets   Last Heard
  DB0BBB-1   wampes:a0         1   2026-10-16 ...
  ```

Nicht in diesem Lauf: mehrere Knoten zugleich (§8.12); auf einem TCP-Knoten
gibt es keinen Monitor (die Ableitung liefert dann 0).

### 11.11 Der Spiegel ist weg (2026-10-16)

**(C) und (F) zurückgenommen.**  Der Shim spiegelt nicht mehr.  Gefallen sind
aus `libax25/wampes.c` die Synthese `connect → SABM`, `accept → UA`,
`close → DISC`/`DM` (`WAMPES_CTL_*`, `wampes_mirror_*`, `wampes_link_path`)
und der UI-Mirror in `wampes_sendto()`/`wampes_recvfrom()`, samt der Felder
`path[]`, `npath` und `we_called` in `struct wampes_sock` (§11.7).  Was
bleibt, ist der echte Weitergabeweg: `wampes_source_call()` (der Sendepfad)
und der Accept-Zeilen-Pfad in `him`/`AX25_PATH`.  Die Sitzung selbst — der
Deskriptor, den der Knoten übergibt — war nie Teil des Spiegels.

**Der Loop speist mheardd nicht mehr.**  `d48a258` hatte in
`ax25netd/mux.c` (`mux_mirror_raw_ctl()`, Zweig `AGWPE_CMD_RAW`) die
Loop-Ausnahme `u != &ax25netd.loop` gestrichen, damit der über den Loop
gespiegelte WAMPES-Verkehr in der Heard-Liste erscheint.  Mit (E) holt sich
`mheardd` die Rahmen direkt vom Knotenmonitor (§11.10); die Ausnahme ist
wieder da, und der Loop ist wieder, was er war: kein Funk.  Der Loop-Port
selbst (`AGWPE_PORT_LOOP`, 8200) bleibt unberührt.

**Belegt (Laufzeit, macOS).**  Ein Mock-Knoten (`wampesmock.py`) spricht die
Dienst-Protokolle; `AXSOCK_BACKEND=wampes`, `WAMPES_SOCKET=<mock>`:

* **`connect()`** — mit Handover-Deskriptor und ohne (dann wird die
  Kontrollverbindung die Sitzung): die Verbindung steht, `getsockname()` /
  `getpeername()` nennen Quell- und Zielruf, ein Byte kommt zurück, `close()`
  kehrt zurück.
* **`listen()` / `accept()`** — der Knoten schickt `wampes:a0 DL1ABC-1 >
  DL9SAU-3` samt Deskriptor, `accept()` gibt den Ruf durch, die Sitzung
  trägt.
* Die (E)-2-Kette bleibt grün: `mon_e2e.py` (FAILURES: 0), `listen_e2e.py`
  (PASS), `mheard_e2e.py` (`DB0BBB-1  wampes:a0`), `svc_regress.py`,
  `mon_test{,2,3,4}.py`.

**Noch offen.**  Der zweite, unabhängige Schritt (§11.8, roh senden auf
`sockets/ax25`).

### 11.12 Der Monitor-Socket und sein Modus (2026-10-17)

**Der Monitor hat eigene Rechte.**  Bis hierher teilte er sich die Vorgabe
des Dienst-Sockets: `set_service_rights()` setzte auf **beide** 0666, weil
das Verzeichnis `sockets/` (0750) das Tor sein sollte.  Der Monitor bekommt
jetzt seine eigenen: **0660** als Vorgabe, und seine Gruppe ist die Gruppe
von `sockets/`.

**Warum 0660 und nicht 0666.**  Der Dienst-Socket entscheidet **CONNECT**,
und `connect()` an einem Unix-Socket fragt nach *write* — „weit offen" ist
dort 0666.  Der Monitor entscheidet **ZUSCHAUEN**, und zuschauen heißt, in
jeden Rahmen des Knotens sehen zu dürfen.  Das Publikum dafür ist die
Gruppe, und 0660 sagt das.  Dieselbe Regel wie in §11.3: nicht nach
Benutzer filtern — das Tor ist die Datei.

**Warum die Gruppe vererbt wird.**  BSD/macOS gibt einem neuen Socket die
Gruppe seines **Verzeichnisses**, Linux die des **Prozesses** (nur mit
setgid am Verzeichnis wäre es die des Verzeichnisses).  Auf einem Knoten,
der als root läuft, wäre der Monitor auf Linux damit `root:root` 0660 —
nur root dürfte zuschauen —, während auf macOS `root:staff` jeden lokalen
Account zuließe.  Also *sagt* der Knoten es, statt es zu erben:
`inherit_dir_group()` in `open_listener()` gibt dem Monitor die Gruppe von
`sockets/`.  Damit meint `chgrp hams $TCPDIR/sockets` auf beiden Systemen
dasselbe, und `axsock monitor group <name>` kann es aus der `net.rc`
übersteuern.

**Das Verzeichnis ist nicht mehr das Tor.**  `sockets/` kommt jetzt **0755**
hoch; `.sockets` bleibt bei **0700** (`create_rundir()`/`create_admin_rundir()`
in `lib/rundir.c`, `nodedir_setup()` in `src/main.c`, `_secure` im Makefile —
die vier Stellen, die den Modus festlegen, sind gleichgezogen: `sockets/` 0755,
`.sockets` 0700).  Die 0750
auf `sockets/` hatte nichts entschieden: auf BSD wurde sie `root:staff`
und ließ jeden zu, auf Linux `root:root` und ließ niemanden zu — was den
Sysop zu `chmod 755` trieb.  Was das Verzeichnis noch entscheidet, ist der
Weg dorthin (`x`) und wer einen Socket anlegen darf (`bind()` braucht
*write*) — beides nicht die Frage, wer senden darf.  Die steht auf dem
Socket, und `axsock mode`/`group` sowie `axsock monitor mode`/`group`
bewegen sie aus der `net.rc`.

**Belegt (Laufzeit, macOS).**  Ein echter Knoten (`net -g -d <dir>`, ein
Pty-Port, keine Rahmen), `axsock_mon_mode.py`: `sockets/` 0755, `.sockets`
0700, `sockets/ax25` 0666, `sockets/monitor` 0660; ein vorab mit fremder
Gruppe angelegtes `sockets/` vererbt sie an den Monitor; `axsock monitor
mode 0640` und `axsock monitor group <name>` wirken; Unfug wird abgewiesen;
das Listing zeigt beide Sockets.  FAILURES: 0.

**Doku.**  `wampes.git/doc/PERMISSIONS.md` ist entsprechend umgeschrieben —
der Abschnitt „The door is the directory" heißt jetzt „The socket is the
door", die `chgrp`-Beispiele sind `axsock group`/`axsock monitor group`
geworden, und „Setting it right" nennt 0700/0755 statt 0700/0750.
`doc/INSTALL-PATHS.md` (Layout-Tabelle und „The two socket directories\":
`.sockets` 0700, `sockets/ax25` 0666, `sockets/monitor` neu) und
`doc/LIBAX25-BRIDGE.md` (die 0660 im Dienst-Satz) sind nachgezogen.

### 11.13 Port-Scoping: `-p <Name>` fragte den Kernel, obwohl der Port kein Kernel-Port war (2026-10-17)

**Der Befund (db0fhn).**  `listen -a -p loop` und `listen -a -p radio0`
endeten mit *„cannot watch for AX.25 frames: No such device or address"*
(ENXIO), für **jeden** Namen im `axports`.  Ohne `-p` lief alles.

**Die Ursache.**  Die Port-Aufteilung in `axmon_open_mask()` entscheidet pro
Name, welche der drei Quellen den Port bedient (`kernel_owner`,
`wampes_owner`, `shim_owner`, §11.10).  Für den Kernel stand dort

```c
kernel_owner = named && ax25_config_get_dev((char *)port) != NULL;
```

und das ist **kein** Kernel-Test.  `ax25_config_get_dev()` liefert für jede
`axports`-Zeile ein Device: `ax25_config_init_port()` setzt `dev = name` für
genau den Eintrag, der kein Interface nennt (`axconfig.c`).  `get_dev()`
beantwortet also „steht dieser Name in `axports`", nicht „ist das ein
Kernel-Port" — und antwortete damit auf jedem Rechner mit **ja** für jeden
Port, auch die Userspace-Ports.

*(Nachtrag 2026-10-19: Dieses `get_dev()`-Verhalten war selbst ein Fehler, den
`24e3aba` beim Umbau von `ax25_config_init_port()` eingeführt hatte.  Der
Getter ist in §11.18 wieder strikt — die Aussage „für jede `axports`-Zeile ein
Device" beschreibt den damaligen, nicht mehr den heutigen Stand.  Die Korrektur
hier, `axmon` fragt `ax25_config_port_is_kernel()`, bleibt richtig und
unberührt.)*

**Was daraus folgte.**  `kernel_owner` war immer wahr, und daran hängt mehr
als der Kernel-Zweig: `shim_owner = named && !kernel_owner && …` wurde damit
falsch, also fiel der **Shim**-Zweig aus.  Übrig blieben nur der
Knoten-Zweig (läuft nur, wenn der Name einen konfigurierten Knoten nennt) und
der Kernel-Zweig — und letzterer steht hinter `ax25_config_kernel_ports() > 0`.
Auf db0fhn ist Kernel-AX.25 **nicht geladen** (das einzige Kernel-Interface wäre
`bpq0`), also ist `kernel_ports` **0** und der Kernel-Zweig wird gar nicht
betreten.  Damit lief **kein** Zweig, `nfd == 0`, und `axmon_open_mask()`
antwortete mit `errno = ENXIO` — genau das gemeldete „No such device or
address", für `loop` **und** `radio0`.  (Auf einem Wirt mit geladenem Kernel
nähme `-p <Userspace-Name>` zusätzlich den Kernel-Zweig, fände kein Interface
und gäbe **dort** schon fatal auf — dieselbe falsche Frage, zweite sichtbare
Form.)  Auf macOS ist der Ausgang derselbe wie auf db0fhn (`kernel_ports` 0),
deshalb ist der Fehler hier reproduzierbar — die e2e-Tests fanden ihn nicht,
weil sie `wampes:…`-Namen benutzten, die der Knoten-Zweig ohnehin bedient.

**Die Korrektur.**  Das Prädikat ist das, das `bind()` selbst benutzt:

```c
kernel_owner = named && ax25_config_port_is_kernel(port);
```

`ax25_config_port_is_kernel()` (`axconfig.h`:204) trägt das Urteil aus
`ax25_config_init_port()`: Kernel nur dann, wenn das Rufzeichen zu einem
*up*-Interface paßt.  `listen -p loop`, `-p radio0`, `-p wampes:…` treffen
jetzt genau die eine Quelle, die den Port bedient, und deren Fehler ist fatal.

**Das `?` in `listen -a` ist kein Fehler.**  `-a` **ohne** `-p` öffnet einen
**ungebundenen** Monitor.  `axsock_raw_name()` (`agwpe_sock.c`) nennt einen
Rahmen nur dann aus der Portnummer, wenn der Bind den Port kannte
(`s->port_known`) oder es der loop-Port (255) ist — dessen Nummer ist eindeutig,
weil das Protokoll sie ihm vorbehält.  Ohne das ist die Nummer eines anonymen
Senders (0) nicht von „erster Port" zu unterscheiden, und eine `axports`-Zeile
auf dieser Nummer wäre eine falsche Antwort.  Für einen ungebundenen Monitor
bleibt deshalb jeder Nicht-loop-Rahmen namenlos — `listen` druckt `?` statt
einen Namen zu erfinden.  Die Abhilfe ist `-p radio0`: der Bind setzt das
Device in `s->bound`, und der Rahmen heißt dann `radio0:`.

**Ein Name, zwei Bedeutungen (Hinweis).**  Der Knoten-Zweig beansprucht
**jeden** Namen, dessen Teil vor dem Doppelpunkt ein konfigurierter Knoten ist
(`wampes_port_hook()`, `wampes.c`:326).  Ein Knoten namens `radio0` nimmt damit
auch den *einfachen* Port `radio0` mit — dieselbe Reihenfolge wie
`wampes_bind()` (Kernel → Knoten → Shim).  Das ist Absicht, aber es heißt: wer
in der `axports` einen Shim-Port `radio0` **und** einen Knoten `radio0` (etwa
über `radio0:5`) führt, erreicht mit `-p radio0` den Knoten, nicht den Shim.
Einer der beiden Namen muß weg.

**Belegt (Laufzeit, macOS).**  `scoping_probe` gegen `HEAD:axmon.c` und gegen
den Fix: vorher liefert `axmon_open_mask()` für **jeden** Namen ENXIO
(`loop`, `radio0`, `wampes`, `wampes:a0`), nachher öffnet `loop`/`radio0` die
Shim-Quelle und `wampes`/`wampes:a0` scheitern nur, solange kein Knoten läuft.
`listen_p_scope.py` (neu) fängt die Vorbedingung selbst ab — es prüft erst
ohne `-p`, ob überhaupt eine Shim-Quelle da ist, und verlangt dann, daß
`-p loop` genau diese öffnet: **Fix 0 Fehler, BUGGY 2 Fehler**.  `listen_e2e.py`
PASS, `mheard_e2e.py` PASS, `mon_e2e.py` 0 Fehler, `session_test.sh` ok.

### 11.14 Benannter Kernel-Port: der `SOCK_PACKET`-Bind war nie einer (2026-10-17)

**Der Befund (db0fhn).**  Nach `modprobe bpqether`, `ifconfig bpq0 up` und
`axparms -setcall bpq0 te1st` zeigt `listen -a` die `bpq0`-Rahmen
(`bpq0: fm TE1ST to IDENT …`), aber `listen -a -p bpq0` endet mit
*„listen: invalid port name - bpq0"* — obwohl `bpq0` in `/etc/ax25/axports`
steht.  Auffällig: mit einem **nicht** passenden Rufzeichen (`axparms -setcall
bpq0 te1st-1`, das den Eintrag zum Userspace-Port macht) „gelingt" der Aufruf
(er geht dann in den Shim-Zweig), mit dem **passenden** `te1st` kommt `EINVAL`.

**Die Ursache, erste Hälfte — der Bind.**  Der Kernel-Zweig von
`axmon_open_mask()` band das Packet-Socket mit einem `struct sockaddr_ll`:

```c
int fd = real_socket(PF_PACKET, SOCK_PACKET, protocol);
struct sockaddr_ll sll;
sll.sll_family = AF_PACKET;
sll.sll_ifindex = (int)if_nametoindex(dev);
real_bind(fd, (struct sockaddr *)&sll, sizeof(sll));	/* 20 Byte */
```

Ein `SOCK_PACKET`-Socket wird vom Kernel aber nicht von `packet_bind()`,
sondern von `packet_bind_spkt()` bedient — und das prüft als **erstes**

```c
if (addr_len != sizeof(struct sockaddr))	/* 16, nicht 20 */
	return -EINVAL;
```

bevor es `sa_data` überhaupt als Gerätenamen ansieht.  Ein `struct
sockaddr_ll` ist vier Byte länger als `struct sockaddr`; jeder Bind eines
**benannten** Kernel-Ports scheiterte damit an `EINVAL` — *egal welches
Gerät*.  (Der `sockaddr_ll`-Weg gehört zu `SOCK_RAW`, dem modernen
`packet_bind()`.)

**Warum das nie auffiel.**  `-p` **ohne** Kernel-Ports überspringt den Zweig
ganz (`ax25_config_kernel_ports() > 0`), das war db0fhn bis `modprobe`.  `-a`
**ohne** `-p` bindet gar nicht: ein ungebundenes Packet-Socket sieht alle
Kernel-Ports und meldet sie unter ihrem Namen aus `sa_data` — der Weg, den
`listen -a | grep bpq0` nimmt.  Und macOS hat keinen Kernel-AX.25-Stack, der
Zweig wird dort nicht einmal kompiliert.  Nur der Fall „geladener Kernel-AX.25
**und** `-p <Kernel-Port>`" führte hinein, und den gab es vorher nicht.

**Die Ursache, zweite Hälfte — die Meldung.**  `listen` liest `EINVAL` als
*„invalid port name"*:

```c
if (axmon_open(htons(proto), port, &mon) < 0) {
	if (errno == EINVAL)	/* „Name in keiner axports-Zeile" */
		fprintf(stderr, "listen: invalid port name - %s\n", port);
	else
		perror("listen: cannot watch for AX.25 frames");
```

`EINVAL` ist in `axmon.h` für genau *diesen* Fall reserviert („a name in no
axports entry").  Der Kernel-`EINVAL` des Binds schlüpfte in dieselbe Schublade
und wurde als Tippfehler ausgegeben — eine falsche Antwort auf die richtige
Frage.

**Die Korrektur.**  `SOCK_PACKET` bleibt (siehe unten); gebunden wird es wie in
`krnlif.c` (`USE_OBSOLETE_SOCK_PACKET`): ein `struct sockaddr` **genau seiner
Länge** mit dem Gerätenamen in `sa_data` — dieselbe Hälfte, aus der der
Lesepfad (`display_port(sa.sa_data)`) den Quellnamen jedes Rahmens nimmt:

```c
struct sockaddr sa;
size_t n = strlen(dev);
if (n > sizeof(sa.sa_data) - 1)
	n = sizeof(sa.sa_data) - 1;
memset(&sa, 0, sizeof(sa));
sa.sa_family = AF_PACKET;
memcpy(sa.sa_data, dev, n);
if (real_bind(fd, &sa, sizeof(sa)) < 0) { … }
```

`if_nametoindex()` entfällt: das `bind()` selbst ist die Existenzprüfung
(`packet_bind_spkt()` → `dev_get_by_name()`).  Und damit der reservierte
`EINVAL` auch künftig nur eines bedeutet, wird ein Kernel-`EINVAL` aus dem Bind
nicht durchgereicht:

```c
errno = (save == 0 || save == EINVAL) ? ENODEV : save;
```

Kein `listen.c`-Eingriff nötig; der Vertrag des Headers gilt wieder.

**Warum nicht `SOCK_RAW`/`sockaddr_ll`.**  `SOCK_RAW` füllt beim `recvfrom()`
ein `sockaddr_ll`; in `sa_data` stünden dann Protokoll und `ifindex` statt des
Gerätenamens, und `listen`s `display_port()` benennt die Quelle aus genau
`sa_data`.  Der Name käme nie an.  `SOCK_PACKET` ist die eine API, die den
Device-Namen in `sa_data` liefert — also bleibt sie, und der Bind mußte sich
ihr anpassen, nicht umgekehrt.

**Belegt (macOS).**  Der Linux-Zweig wird hier nie kompiliert; nach Entfernen
der nun überflüssigen Linux-Header (`net/if.h` diente `if_nametoindex()`,
`netpacket/packet.h` dem `sockaddr_ll`; `PF_/AF_PACKET` und `SOCK_PACKET`
haben Fallback-Defines) läßt er sich aber mit `-D__linux__` auf macOS als
Übersetzungseinheit prüfen: `-Wall -Wextra` warnungsfrei.  Regression ohne
Verhaltensänderung auf macOS: `listen_p_scope.py` **0 Fehler**,
`listen_e2e.py` PASS, `mheard_e2e.py` PASS, `mon_e2e.py` **0 Fehler**,
`session_test.sh` ok.

**Belegt auf db0fhn (2026-10-17).**

* `listen -a -p bpq0` zeigt
  `bpq0: fm TE1ST to IDENT ctl UI^ pid=F0(Text) len 4 … test` — kein
  „invalid port name" mehr.
* `listen -p bpq0` ohne `-a` (RX only) zeigt nichts: richtig, denn eigenes
  Senden kommt nicht als Empfang zurück.

**Und der Rufzeichen-Mismatch ist keine Regression.**  `axparms -setcall
bpq0 te1st-1` macht `bpq0` zum Userspace-Port; `beacon -s bpq0` sagt dann
„no backend serves the port 'bpq0'".  Das ist die Rufzeichen-Identität aus
§2 (Kernel: `ax25_bind()` → `ax25_addr_ax25dev()` → sonst
`EADDRNOTAVAIL`; das alte libax25 verwarf den Eintrag ohne passendes
Interface-Rufzeichen).  Die Kernel-Port-Erkennung („Kernel-Port
nachweislich gefunden") bestätigt sich damit: bei aktivem Kernel-Interface
mit passendem Rufzeichen wird der Port als Kernel-Port bedient, bei
Rufzeichen-Mismatch als Userspace-Port.  Die Meldung nennt seit `9979b60`
das Rufzeichen und ist damit diagnostisch statt irreführend.  Siehe
`LINUX-AX25-SOCKETS.de.md` §2.

### 11.15 Offene Frage: IP-Nutzlast über den AGWPE-Port (2026-10-17)

**Frage:** Was gilt, wenn ein Anwender über den AGWPE-Port IP nach außen
sprechen bzw. von außen über ihn erreichbar sein will?

Hintergrund:

* Kernel-AX.25 kann IP-Nutzlast tragen (IP-over-AX.25).
* Mit WAMPES wird IP dem `net`-Programm überlassen (klassische
  WAMPES-Trennung: AX.25-Framing/Forwarding vs. Netz-Layer via `net`).
* Über den AGWPE-Port (Shim/Bridge) ist unklar, wie IP-Verkehr behandelt
  wird: soll IP über AGWPE durchgereicht/geroutet werden, oder bleibt IP
  ausschließlich beim WAMPES-`net`-Programm?

Der Kernel liefert den Rahmen dafür frei Haus: `AX25_P_IP`/`AX25_P_ARP`
gehen in `ax25_rcv()` an `netif_rx()`, IP und ARP macht der Kernel selbst
(`LINUX-AX25-SOCKETS.de.md` §5).  Die Frage bleibt offen.

**Siehe auch:**
* `ax25-non-kernel-stack.sh`, `build-unix-ax25-stack.sh`
* `AX25-WITHOUT-KERNEL-SYSOP*.md`, `AX25-WITHOUT-KERNEL-DEVELOPER*.md`

### 11.16 `AX25_PIDINCL` und mehrere Dienste unter **einem** Rufzeichen (Idee, 2026-10-17)

**Ein eigenes Thema.**  Nicht Teil von §11.8 und ohne Berührung von LAPB.
§11.8 ist **UI**: roh senden und empfangen, ein Rahmen, kein
Verbindungszustand.  Hier geht es darum, einem **verbindungsorientierten**
libax25-Programm zu erlauben, sich für **alle PIDs** zuständig zu
erklären — die Grundlage für einen PID-Multiplexer.

**Der Anlass.**  Ein Userspace-`netromd` und ein `rosed` sollen unter
**demselben** Rufzeichen (gleicher SSID) erreichbar sein.  Über SSIDs löst
man das nicht — mehrere SSIDs verwirren den Anwender und ggf. das Netz.

**Warum eine AX.25-Verbindung es hergibt.**  Eine AX.25-Verbindung ist
**eine** Verbindung; was die Dienste unterscheidet, steht im **PID der
I-Frames**.  Der Kernel kennt dafür `AX25_PIDINCL`
(`setsockopt(fd, SOL_AX25, AX25_PIDINCL, …)`, Wert 8):

* **Empfangen:** ist es gesetzt, bleibt der PID als erstes Byte im
  empfangenen Datenstrom stehen, statt abgeschnitten zu werden — der
  Prozeß sieht, *welcher* Dienst gemeint ist.
* **Senden:** ist es gesetzt, schiebt der Kernel den PID **nicht** davor;
  das Programm gibt ihn selbst mit — ein Socket spricht damit mehrere PIDs.

**Warum es hier interessant ist.**  Über `SO_PEERCRED` (§10) weiß der
Knoten, *wer* verbunden ist, aber nicht, *welchen Dienst* ein I-Frame
meint.  Der Knoten könnte der Linux-Seite den PID sichtbar machen (das
`AX25_PIDINCL`-Verhalten nachbilden), und der Prozeß entscheidet selbst:
PID 0xCF → NET/ROM, PID 0x01 → ROSE.  Ohne das bleibt nur, je Dienst ein
eigenes Rufzeichen bzw. eine eigene SSID zu verlangen.

**Reichweite nach Kernel-Art.**  `AX25_PIDINCL` wirkt im Kernel nur
**verbindungsorientiert** (`ax25_rx_iframe`, `ax25_recvmsg`); für
UI-Datagramme wird `ctl+pid` in `ax25_rcv()` bedingungslos abgeschnitten.
Wenn wir es anbieten, dann genau dort — nicht als UI-Angebot.

**Nicht zu verwechseln mit `ax25d`s NET/ROM-Konfiguration.**  `ax25d.conf`
kennt sehr wohl eine NET/ROM-Zeile; sie meint aber **NET/ROM-L4-Verbindungen**
(das, was `netromd` aus der NET/ROM-L3-Sitzung als L4-Nutzlast übergibt,
`nr_config_get_addr()`), nicht einen AX.25-PID.

**Offen.**  Ob und wann; und wie es sich zum eingehenden Pfad aus den
Roh-Monitorframes (§8.11) verhält.  Notiert, nicht entschieden.

### 11.17 `sockets/packet` ist gebaut (2026-10-18)

**Der Sockel.**  Die Sendeseite aus §11.8 steht: **`sockets/packet`**, ein
dritter Listener (`restricted = 3`) neben Dienst (`0`) und Monitor (`2`).
Er ist die *eine* Datei mit **beiden** Richtungen — Zusehen **und**
Einspeisen — und die zwei getrennten Sockel bleiben trotzdem: `ax25` (0666)
verbindet, `monitor` (0660, Gruppe) sieht zu, `packet` (0600, Gruppe)
speist ein.  Die Gruppe werden `monitor` und `packet` von `sockets/`
geerbt (§11.12); `ax25` bekommt keine, weil sie bei `0666` nichts
entscheidet.

**Was entschieden ist.**  Zwei Fragen aus §11.9 sind beantwortet:

* Vorgabe-Modus und -Gruppe: **0600** + `inherit_dir_group()`.
* Der Handshake bleibt **wortgleich zu §11.4**: dasselbe Wort `monitor`,
  dieselben Optionen (`iface=`, `input`, `output`).  `iface=` ist hier die
  Erlaubnisliste fürs Einspeisen (Vorgabe: alle AX.25-Interfaces).

Kein Missverständnis beim Wort `output`: der `packet`-Client liest **den
Monitorstrom** des Knotens — dieselben Rekorde, dieselben Filter wie
`sockets/monitor` (`ETH_P_AX25` → eingehende, `ETH_P_ALL` → zusätzlich die
eigenen Aussendungen des Knotens).  `output` filtert also die
**zurückkommenden** Rahmen.  Das Einspeisen selbst hat keine Richtung: es
geht immer hinaus.

**Der Knoten (`remote_net.c`).**  Der Listener kommt als Ordnernde
`packet` neben `ax25` und `monitor` hoch (`open_listener()`).  Ein
angenommener `packet`-Client geht nach dem `monitor`-Wort in
`packet_receive()`: derselbe Rekordstrom wird gelesen, aber jede volle
Aufzeichnung läuft durch `packet_byte()`/`packet_send()`.  `packet_send()`
holt das Interface über `if_lookup()` und verlangt AX.25: ist `ifp` kein
AX.25-Träger (`ifp->output != ax_output`), fällt der Rahmen still — der
Knoten kann keine Antwort in einen binären Strom schreiben.  Der Rahmen
wird als **UI** erwartet und über `ax_send_ui()` hinausgegeben; die
Adressfelder liest `ntohax25()` (`ax25hdr.c`) ab, Ctl/PID kommen nicht mit
(§11.8).  Ein reiner Leser (`restricted == 2`) bekommt auf ein schreibendes
Byte eine einmalige Warnzeile — read-only, rohe Rahmen gehen über
`sockets/packet`.

**Die libax25-Seite.**  `wampes_raw_send()` (`wampes.c`) ist das
Gegenstück zu `wampes_mon_read()`: lazy eine Verbindung auf
`sockets/packet` (Geschwisterableitung des Dienst-Pfades, wie der Monitor),
Handshake `monitor\n`, dann der Rekord `[4B Länge][Name][NUL][Frame]`.
Reißt ein Schreiben, wird der Descriptor geschlossen und `*fdp = -1`; die
nächste Aufzeichnung beginnt von vorn, statt in einen Knoten zu schreiben,
der nicht mehr liest.  Im Shim landet **`sendto()`** auf einem
`SOCK_PACKET`-fd im `s->raw`-Zweig von `agwpe_sendto()`: der
Interface-Name kommt aus `sa_data` (wie beim `bind()`), sonst aus dem
gebundenen `s->bound`.  Ohne WAMPES-Knoten gibt es kein `sockets/packet`
und der Rekord paßt in kein AGWPE-Kommando — dann ehrlich `EOPNOTSUPP`
statt eines Schreibens ins Leere.

`send()`/`write()` sind **noch nicht** dort: sie laufen weiter in
`axsock_send_data()` (den Paar-Pfad).  Das ist die verbleibende Hälfte von
§11.9.  Ein Programm, das roh sendet, benutzt ohnehin `sendto()` mit dem
Gerätenamen — wie der Kernel, der ein `write()` auf einem unadressierten
`SOCK_PACKET`-fd mit `ENOTCONN` quittiert; der Test (`rawsend.py`) nimmt
genau diesen Ruf.

**Belegt (Laufzeit, macOS).**  `packete2e.py` (Pty-Port, Knoten mit `-d`,
ein Client auf `sockets/packet`) fährt beide Richtungen: ein Rekord auf
`sockets/packet` erscheint als KISS-Rahmen auf der Leitung, ein
KISS-Rahmen auf der Leitung kommt als Rekord auf `sockets/packet` an, ein
Rekord mit unbekanntem Port wird still verworfen (die Verbindung und der
Knoten bleiben), und `sockets/packet` liegt auf **0600** mit der Gruppe des
Verzeichnisses.  Mit `--shim` läuft zusätzlich `rawsend.py` mit der
Bibliothek (`DYLD_INSERT_LIBRARIES`): sein `sendto()` auf dem
`SOCK_PACKET`-fd erreicht den `s->raw`-Zweig und derselbe Rahmen steht auf
der Leitung.  Die Werkzeuge liegen in `wampes-import/testtools/`
(`packetinject.py`, `packetread.py`, `rawsend.py`, `packete2e.py`).

**Siehe auch:** `wampes.git/doc/PERMISSIONS.md`, `INSTALL-PATHS.md` (die
Rechte-Leiter), `AX25-WITHOUT-KERNEL-SYSOP.{de.,}md` (der Sysop-Abschnitt),
`LINUX-AX25-SOCKETS.de.md` §8 (Dateirechte statt `CAP_NET_RAW`).

### 11.18 `ax25_config_get_dev()` wieder strikt: Device nur für Kernel-Ports (2026-10-19)

**Der Befund (db0fhn).**  Kernel-AX.25 ist dort nicht geladen (§11.13: das
einzige konfigurierte Kernel-Interface wäre `bpq0`).  Trotzdem bekam jedes
Werkzeug, das einen Port auf sein Interface abbildet, für **jeden**
`axports`-Eintrag ein Device: `ax25_config_get_dev("bpq0")` lieferte den String
`bpq0`, obwohl `ifconfig bpq0` „no such device" sagt.  Ein Gerätesucher wie
`rmsfinddev` (rmsgw#16, `rmschanstat`) hielt den verwaisten Eintrag damit für
einen bedienbaren Port.  Dieselbe Wurzel hatte der in §11.13 beschriebene
`listen`-Fehler.

**Die Ursache.**  Vor `24e3aba` hielt `ax25_config_init_port()` einen Port,
dessen Rufzeichen auf kein *up*-Interface paßt, gar nicht erst in der Tabelle
(`found = 0` → `return FALSE`); der Getter konnte so nie einen Userspace-Port
sehen.  `24e3aba` („axsock: name every bind, and every raw frame, by its own
port") kehrte das um: der Eintrag bleibt, `dev = name`, `Kernel = FALSE`.  Das
ist richtig für die Namens- und Bind-Pfade, die den Namen als Schlüssel
brauchen (Rückwärtssuche, `ax25_port_info()`, Monitor-Matching) — aber
`ax25_config_get_dev()` las genau dieses `Device` und beantwortete damit „steht
der Name in `axports`" statt „ist das ein Kernel-Port".  Der Commit kannte die
Falle und nannte sie im Log („the device name is the trap"), ließ den Getter
aber unangepaßt.

**Die Korrektur.**  `ax25_config_get_dev()` gibt das Device nur noch heraus,
wenn der Port ein Kernel-Port ist, sonst `NULL`:

```c
if (AX25_KERNEL_IFACES && !p->Kernel)
        return NULL;
return p->Device;
```

`AX25_KERNEL_IFACES` ist 1 auf Linux (`#ifdef __linux__`), 0 sonst.  Die
Unterscheidung ist nötig, weil nur Linux die Kernel-Interfaces gegen die
Rufzeichen halten kann: dort ist `p->Kernel` das Urteil aus dem Laden
(Rufzeichen == AX.25-Adresse eines *up*-Interfaces).  Wo keine solche Liste zu
haben ist, steht der Name wie eh und je als Device — das Verhalten, das ein
Wirt ohne heimischen Kernel-AX.25-Stack immer hatte.  `p->Device` bleibt für
alle anderen Leser (`ax25_port_info()`, Rückwärtssuche, Monitor) der Name; nur
der Getter für das Interface rückt wieder gerade.  Die Tabelle hält den
Eintrag weiter, damit die Userspace-Backends ihn anhand des Namens finden
können.

**Was das für die Userspace-Ports heißt.**  Ein WAMPES- oder AGWPE-Port *ist*
kein Interface, und `ax25_config_get_dev()` sagt das jetzt auch.  Auffindbar
bleiben sie über `ax25_port_info()`, das genau dafür da ist und die
**Verfügbarkeit** beantwortet statt eines Devices: Kernel lokal, WAMPES aus
`wampes.conf` (lokal, **ohne** laufenden ax25netd), AGWPE über ax25netd, der den
Zustand der Upstreams kennt.  `ax25netctl` gibt das als Tabelle aus; für jene
Ports ist ein *Geräte*suche der falsche Aufruf.  Nachgezogen in `ax25-apps`
(`ax25netctl`): der vorgezogene netd-Query setzt kein `rc=1` mehr, und ein
Kernel- oder WAMPES-Name kommt ohne ax25netd sauber mit `rc=0` zurück.

**Belegt (Laufzeit, Linux/amd64).**  Echte Kernel-Interfaces ließen sich in der
Sandbox nicht erzeugen (`modprobe`/`unshare` gesperrt), daher ein
`LD_PRELOAD`-Harnisch mit gefälschtem `/proc/net/dev` (`fake0`/Ruf `TE1ST`/up,
`fakedown`/Ruf `TE3ST`/down).  Gegen die alte `/lib/libax25.so.0` und gegen den
Fix:

```
alte Lib:   bpq0 -> fake0, radio0 -> radio0, loop -> loop, wampes -> wampes, foo -> foo
mit Fix:    bpq0 -> fake0, radio0/loop/wampes/foo -> (kein Device)
```

Die alte Lib gab jeden `axports`-Eintrag als Device aus — die Regression
sichtbar.  `ax25_port_info()` ist unverändert und bleibt der Weg für die
Auffindbarkeit: `wampes` → `wampes` (auch bei totem ax25netd), `radio0`/`loop`
→ `agwpe`, `bpq0` → `none`.

**Geändert:** `axconfig.c` (`AX25_KERNEL_IFACES`, der Getter, der Kommentar in
`ax25_config_init_port()`), `netax25/axconfig.h` (der Vertrag des Getters),
`axmon.c` (nur der Kommentar; die Prüfung bleibt
`ax25_config_port_is_kernel()`).  Siehe auch §11.13.

## 12 Callsign-Eingabe: nicht raten, sondern sterben (2026-10-15)

**Der Befund.**  `ax25_aton_entry()` schreibt die sechs Rufzeichen-Bytes, wie
es sie liest, und **den SSID-Byte erst am Ende** (`buf[6] = ((ssid + '0')
<< 1) & 0x1E`, `axutils.c`:120).  Beide Fehlerpfade — `dl9sau-` (kein SSID)
und `-2 … -10`/`-` (SSID unbrauchbar) — kehren **vor** dieser Zeile um.  Der
Aufrufer bekam also `-1` **und** einen halbgeschriebenen Puffer, in dem
`buf[6]` noch der SSID-Byte der **vorher** in dieselben Bytes geparsten
Adresse war — bei `call` der SSID der Port-Adresse aus `axports`.

`call.c` verwarf dieses `-1` an fünf Stellen; `bind()` band die
halbgeschriebene Adresse, und `call -r -s dl9sau- loop db0fhn-9` ging mit
einem Quellruf hinaus, den `ax25_ntoa()` als leerer Ruf mit SSID −15 liest.
Der Shim verwarf dasselbe `-1` in `wampes_connect()` und ließ `s->me` genullt
zurück — der Node bekam einen **leeren lokalen Ruf**.  Kein Race: die Meldung
nennt den `-s`-String und kam vor „Trying…".

**Die Entscheidung.**

1. **`ax25_aton_entry()` wird nicht angefaßt** — eine bewährte alte Funktion,
   und `ax25_ntoa()` in libax25 ebenso wenig.  Der Parser bleibt bytegenau,
   wie er ist.
2. **Nicht in der Bibliothek sterben.**  Ein „`exit(1)` bei `-1`" blanket in
   `axutils.c` wäre falsch: `ax25netd` liest das `-1` **absichtlich** (einen
   Frame verwerfen, einen Digi überspringen — `mux.c`:345/351/373/2694/2700),
   `ax25d` fällt bei fehlgeschlagenem Reload auf die alte Konfiguration
   zurück, und `agwpe_sock.c` parst im **Lesepfad** eines langlaufenden
   Programms (`hdr+4`, ~3870).  Nur `wampes.git/src` hat **null** Aufrufe.
   Die Politik „sterben / abbrechen / überspringen" gehört **pro Aufrufer** —
   und „besser keine Details als falsche Details" (Thomas): wenn `axutils`
   schon eine Meldung geschrieben hat, ist das der Grund zu sterben, nicht
   der Anlaß, eine zweite zu erfinden.
3. **Für Kommandozeilen-Werkzeuge: eine `_or_exit`-Form.**  Neu in
   `axutils.c` + `netax25/axlib.h`: `ax25_aton_entry_or_exit()`,
   `ax25_aton_or_exit()`, `ax25_aton_arglist_or_exit()`.  Sie rufen die alte
   Funktion und **sterben mit `exit(1)`**, wenn sie `-1` liefert.  Kein neuer
   Parser.
4. **`call`** — die fünf Adress-Parsing-Stellen umgestellt: NET/ROM-
   Portadresse (672), Portadresse aus `axports` (699/701), `-s mycall` (715),
   Ziel + Digis (823), ROSE Call + Digi (770/791).  Die 701-Stelle beseitigt
   zugleich ein **Lesen von undefiniertem Inhalt**: `sockaddr` ist eine
   uninitialisierte Union, und der Test `sax25_ndigis == 0` las daraus.  Die
   823-Stelle **war** schon geprüft — sie endete nur mit Exit 0.
5. **Der Shim** (`libax25.git/wampes.c`, `wampes_connect`): die Quelle wird
   **vor dem Dial** geparst; bei `-1` `errno = EINVAL; sock_put(s); return 1;`.
   Der Connect schlägt **hart** fehl, es geht kein Dial hinaus, und `s->me`
   wird nicht aus einem halben Puffer gefüllt.

**Zwei Kleinigkeiten aus derselben Sitzung (Thomas, 2026-10-15):**

* **`-s ""` / `-s "  "` / `-s "\\t"` → Fallback auf den Port-Ruf.**  Von außen
  ist das egal („aus User-Sicht ist das egal"): ein leerer Quellruf ist keine
  Station, und `call` tut, als wäre `-s` nicht angegeben worden — es bindet
  die Portadresse.  Nur abschließende Blanks werden entfernt; **führende**
  bleiben und werden vom Parser abgewiesen (`" dl9sau-1"` ist kein Ruf).
* **`call` endet bei einem echten Fehler mit != 0.**  Bisher lief ein
  fehlgeschlagener Connect in `cmd_call()` auf `return FALSE` → `main()` →
  `return 0` — Exit 0 für einen Ruf, der nie lief.  Jetzt: `connect_to()` ==
  `-1` ⇒ `exit(1)`.  `cmd_call()`s `FALSE` heißt weiter „nicht neu verbinden"
  (das gilt auch für ein sauberes Ende); nur der Connect-Fehler-Zweig stirbt.

**Belegt (macOS, `-std=gnu23`):** alle fehlerhaften Eingaben (`dl9sau-`,
`-10`, `" dl9saU-1"`, eine kaputte Portadresse in `axports`, ein kaputtes
Ziel) → axutils-Meldung + **Exit 1**; gültige (`dl9sau-15`, `db0fhn`,
`db0fhn-9 via WIDE2-1`, kein `-s`) → „Trying…" und dann Exit 1, weil kein
Node antwortet.  Shim-Test mit All-Spaces-Ruf und SSID 15: `local='-15'`,
axutils-Meldung, `connect -> -1, errno=22`, **kein Dial**.  Drei Symbole
exportiert; `libax25` und `ax25netd` bauen ohne neue Warnung.  ROSE bleibt
unter macOS nur **kompiliert** (`socket(AF_ROSE)` scheitert mit
`EAFNOSUPPORT`, bevor der Parse läuft); `rose_aton()` selbst meldet weiterhin
ohne `exit(1)`, aber sein `-1` erreicht jetzt den Connect-Fehler-Zweig und
damit Exit 1.

## 13 Quellen
* `documentation/david-netd-mail3-and-mapping.md` §Wampes, §G, §H
* `libax25.git/doc/MONITOR-255-BRIDGE.md`
* `libax25.git/wampes.c` (`struct wampes_sock`, `wampes_connect/accept/listen`,
  `wampes_mirror_*`)
* `ax25-apps.git/ax25netd/{mux.c,ax25netd.h,mheard.c}` und
  `ax25netctl/ax25netctl.c` (`check_kill_name`)
* `wampes.git/src/{remote_net.c,axserver.c,ax25subr.c}` (`service_table`,
  `axserv_handover`, `axpipe_*`, `Ax25_cb`, `disc_ax25`)
* `wampes.git/doc/{LIBAX25-BRIDGE.md,AGWPE-SERVER.md}`
* `libax25.git/doc/LINUX-AX25-SOCKETS.de.md` — die Kernel-Socketstrukturen
  (`AF_AX25` DGRAM/SEQPACKET/RAW, `AF_PACKET`, `SO_BINDTODEVICE`, PID,
  `AX25_PIDINCL`); Grundlage für §11.8
