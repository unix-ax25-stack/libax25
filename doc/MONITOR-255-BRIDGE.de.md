# WAMPES/libax25-Frames zum AGWPE-Monitor-Kanal 255 und nach `listen`

Implementiert (Stand: Sep 2026) in `libax25/wampes.c`; getestet end-to-end
gegen ax25netd: der Spiegel legt wampes-Verkehr als Raw-`'K'` auf Loop-Port
255, `listen`-Clients re-broadcast das ax25netd 1:1 (einschließlich H-Bits).

Das Dokument hält fest, wie Data von **libax25-Nutzern**, die über die
Backends **wampes** und **agwpe** zum Radio gehen, in den ax25netd-
**Monitor-Kanal 255** gelangen und so ein `listen`-Programm erreichen — mit
dem `repeated`-Bit (H-Bit) an jedem Digipeater. AGWPE, direwolf und ax25netd
bleiben unverändert.

Ergänzt `LIBAX25-BRIDGE.md` (die Session-Brücke `wampes_dial`) und schließt
die dort unter *"Not built yet"* notierte Lücke:

> **Datagrams inbound.** `datagram` sends UI frames; nothing pushes received
> ones back, so `recvfrom()` has no source yet.

## Der Weg (so ist er umgesetzt)

Nicht der WAMPES-**Node** speist den Monitor, sondern **die libax25-Client-
Seite** der wampes-Brücke. Ein libax25-Programm, das über **wampes** und über
**agwpe** mit dem Radio verbunden ist, füttert den ax25netd-Monitor-255
selbst — und zwar **nur mit dem wampes-Verkehr**:

* **vom wampes-Backend empfangen**: Frames, die libax25 von wampes bekommt
  (eingehend, inkl. der H-Bits des Funk-Wegs).
* **an wampes gesendet**: Frames, die das Programm über den wampes-Kanal
  abschickt (ausgehend; hier sind die H-Bits i.A. noch nicht gesetzt).

**Was ein libax25-User von/an AGWPE (direwolf) sendet und empfängt, wird
NICHT noch einmal an den Monitor geschickt** — das übernimmt ax25netd von
selbst.

### Warum nur wampes — und wie die Aufteilung stimmt

* **AGWPE-Frames**: ax25netd verbindet sich als AGWPE-**Client** an jede
  Upstream-Quelle (direwolf). Ein dort eingehendes `'K'`-Raw-Frame läuft
  `on_raw_frame` (`upstream.c:37`) → `mux_upstream_frame` → an `cl->raw`-
  Monitor-Clients (`mux.c:1382-1387`), **unverändert inklusive der H-Bits**.
  Der Monitor wird für den AGWPE-Pfad also komplett von ax25netd gefüllt —
  ein weiteres Einspeisen durch libax25 wäre eine Doppelung.
* **WAMPES-Frames**: wampes ist **kein** AGWPE-Upstream für ax25netd. Es gibt
  dort keinen automatischen `'K'`-Abgriff. Deshalb muss die libax25-Seite der
  wampes-Brücke diese Frames selbst an den Loop-Port 255 legen.

## Wie die Einspeisung umgesetzt ist

Der wampes-Backend in `wampes.c` hält — zusätzlich zur/m Session-Kanal — einen
**AGWPE-Client an den Loop-Port 255** und **spiegelt** jede wampes-UI-im-/send
in ein `'K'`-Frame auf Port 255, ohne den Session-Datenfluss zu berühren:

* `wampes_mirror_open()` verbindet faul (beim ersten Frame) als AGWPE-Client
  gegen ax25netd. Zieladresse wie beim agwpe-Backend: `AXSOCK_HOST`/`AXSOCK_PORT`
  (Default `127.0.0.1:8100`, `AXSOCK_DEFAULT_HOST/PORT` in `agpe_sock.h`), ein
  mit `/` beginnender Host ist ein Unix-Socket; zeigt `AXSOCK_USER` an, wird
  dort `login` ausgeführt. Ist eine frühere Verbindung abgerissen
  (`!agwpe_client_connected`), wird der Stale-Client verworfen und beim nächsten
  Frame neu verbunden. Ein nicht erreichbarer Server scheitert **still** — Die
  wampes-Brücke selbst funktioniert ohne ax25netd weiter.
* `wampes_mirror_frame()` baut den KISS-Body wie `mux_mirror_raw`
  (`mux.c:145-258`): Marker `0`, DEST `|=0xE0`, SRC `|=0x60`, jeder Digi
  `|=0x60`, letzte Adresse `|=0x01` (E-Bit); Kontrollfeld `0x03` (UI), dann PID
  und Payload. Gesendet wird `AGWPE_CMD_RAW` ('K') auf `AGWPE_PORT_LOOP`.
* **Andockpunkte** (reflektieren nur, stören nichts):
  * gesendet: in `wampes_sendto` nach erfolgreichem `datagram`-write — als
    SRC die lokale Rufzeichen (`s->local` oder die Port-Rufzeichen), H-Bits
    `& 0x7F` weggemaskt (die Ausgabe von `ax25_ntoa` zeigt sie ohnehin nicht;
    der Zeiger `may only` — das Frame verlässt die Port so).
  * empfangen: in `wampes_recvfrom` nach `parse_ui_header` — als SRC der
    Absender aus dem `[n]SRC>DEST,...`-Header, die `*`-markierten Digis tragen
    ihr H-Bit (0x80) wieder, genau wie der wampes-Eingang sie gemeldet hat.
  Die Spiegelung tastet `errno` und das an die Anwendung gereichte Frame nicht
  an (eigener Scope, letzter Schritt vor dem return).
* **PID**: Text-Frames (wampes) transportieren keine PID; der Spiegel nimmt
  `s->pid ? s->pid : AGWPE_PID_AX25` (0xf0).

Die komfortablen Helfer `send_unproto`/`connect`/`data` **können das
repeated-Bit nicht** — sie bauen eigene Header. Deshalb ist `send_frame` mit
`datakind='K'` und einem KISS-Body der richtige Weg (analog zu `mux_mirror_raw`
in `mux.c:145-258`, das denselben Body aufbaut wie direwolf).

## repeated-Bit: wo es herkommt und erhalten bleibt

Monitorkanal 255, zwei Frame-Typen:

| Typ | Kennung | Format | repeated-Bit |
|---|---|---|---|
| roh | `'K'` | KISS-encapsulierter AX.25-Frame | H-Bit `0x80` in Byte 6 jedes Digis — 1:1 erhalten |
| dekodiert | `'I'`/`'S'`/`'U'`/`'T'` | ASCII `SRC>DST,DIGI*` | `*` — von direwolf oft weggelassen |

libax25/`listen` nutzt **nur Raw-`'K'`**: `agwpe_sock.c:1404` kopiert die Daten
1:1 an SOCK_PACKET-Monitor-Sockets; `axsock_dispatch` ignoriert die
dekodierten Frames (monitor-Callback NULL, `agwpe_sock.c:1740-1741`). `listen`
zeigt Digis mit `*` aus `data[ALEN] & REPEATED` (`ax25dump.c:143`). Die
dekodierten Frames sind für diesen Pfad also egal — immer Raw-`'K'` mit den
echten H-Bits.

Wo WAMPES die H-Bits führt (für den Fall, dass ein wampes-empfangenes Frame
eingespeist wird):
* Empfang: `ntohax25` (`ax25hdr.c:169-170`), `nextdigi` = Anzahl repeateter
  Digis.
* Digikette umdrehen: `build_path(reverse)` (`lapb.c:1825-1857`), spiegelt die
  H-Bits wie Kernel `ax25_digi_invert` (`ax25_addr.c:289-301`) —
  verifiziert kernel-konform.
* Senden: `htonax25` (`ax25hdr.c:92-95`) setzt das Draht-H-Bit aus `nextdigi`;
  die gespeicherten H-Bits in `digis[i]` sind Anzeige-Zweck.

## Was NICHT geändert wird

* **AGWPE/direwolf/ax25netd**: unverändert. Für AGWPE-Frames füllt ax25netd
  den Monitor selbst; die Raw-`'K'`-Records tragen die H-Bits korrekt.
* **`listen`**: unverändert — zeigt `*` bereits aus dem H-Bit.
* **`setcall()`** (`ax25subr.c:281`): bleibt `*`-ablehnend (Sendepfad; der
  Kernel ignoriert `*` beim Datagram-Senden ebenfalls).
* **Kein Doppelsenden**: AGWPE-Frames werden nicht zusätzlich von libax25
  eingespeist.

## Grenzen des Spiegels: verbundene Sessions sind nicht abgreifbar

Der Spiegel hängt nur an `wampes_sendto`/`wampes_recvfrom` — also am
Datagramm-/UI-Pfad (verbindungslos). **Verbundene AX.25-Sessions (connect)
laufen vollständig daran vorbei**, nicht nur teilweise:

`wampes_connect` (`wampes.c:867`) übergibt nach dem AX.25-Handshake einen
**Session-Descriptor** an die Anwendung (`axsock_replace`, `wampes.c:1001`
und `1081`) und notiert ausdrücklich:

> "The session descriptor is tracked from here on, so that `getsockname()`
> and `getpeername()` can be answered for it. **Nothing else about it is
> intercepted.**" (`wampes.c:1220-1224`)

Die I-Frame-Daten der verbundenen Verbindung fließen danach als nackter
Byte-Strom über `read()`/`write()` auf diesem Descriptor. Die libax25-Seite
sieht nur das **I-Frame-Payload** — niemals:

* die Steuerrahmen SABM/SABME/UA/DISC/REJ/FRMR,
* das ctl-Feld mit N(S)/N(R) und P/F-Bit,
* die Adress-Header (sie stecken im Session-Zustand des wampes-Kerns).

Ein Zusatz-Hook in den Session-Pfad bräuchte Frame-Daten, die auf der
libax25-Seite gar nicht existieren; selbst dann ließe sich daraus **kein**
listen-tauglicher KISS-Frame bauen.

**Folge für Listen-Zwecke (z.B. ax25mond/mheardd):** Ein Zwischenprogramm
an der libax25↔wampes-Session kann prinzipiell nie die Monitor-Ansicht
verbundener Links liefern — so wie sie z.B. der Kernel-Pfad (`listen` mit
`rmnc`-Beispiel) zeigt. Die einzige vollständige Quelle wäre **wampes
selbst**: Es müsste seine interne Rohframe-Sicht (die Ebene, auf der auch
`axlisten_ui_deliver`/`axserver.c:1118` liegt — LAPB/AX.25-Layer) als
KISS/AGWPE-Frames exportieren: ein wampes-internes "listen", kein
AGWPE-Upstream, sondern ein Monitor-Export. Von dort könnten
ax25mond/listen/mheardd alle Frames — auch die verbundenen — beziehen.

**Merke (Designfalle):** Die wampes-Brücke beliefert den Monitor **nur für
UI/Datagramm-Verkehr**. Wer verbundene Sessions im Monitor sehen will, muss
die Quelle im wampes-Kern anzapfen, nicht an der libax25-Session.

## Offene Punkte / getestet

* **PID-Transport**: wampes-Textframes kennen keine PID; der Spiegel setzt
  pauschal 0xf0, wenn der Sock keine eigene PID gesetzt hat. Eine verlorene
  PID ist eine bekannte Grenze dieses Konzepts.
* **Fehlende Frames setzen keinen Fehler** an der Anwendung: weder beim TX-
  noch beim RX-Hook wird ein nicht erreichbares ax25netd/der Loop dem Nutzer
  gemeldet (Kanal ist rein beobachtend).

End-to-end verifiziert (mit dem Loop-Upstream aus `ax25netd`-Testconfig,
`loop socket …`, `loop tcp 8100`):
* **TX**: `sendto` → `datagram` an den Node-Pseudo → Spiegel → `'K'` auf 255 →
  ax25netd re-broadcasts 1:1 an den Raw-Monitor-Client (observer). Empfangener
  Body byte-genau: `00` Marker, DEST `|=0xE0`, SRC `|=0x60`, Digi
  `|=0x60`+E-Bit `0x01`, `03 f0`, Payload.
* **RX**: Node-Pseudo schiebt `[n]SRC>DEST,DIGI*` → `recvfrom` liefert den
  `*`-Digi mit H-Bit → Spiegel auf 255 → Observer zeigt das H-Bit als `0x80`
  im Digi-Byte wieder.

Bleibt unverändert gewollt: **Doppelsenden gibt es nicht** — AGWPE-Frames
speist ax25netd selbst in den Monitor ein; der Spiegel legt nur wampes-Verkehr
auf Port 255. Für die gewünschte englische Fassung: dieses Dokument neben die
bestehenden `doc/`-Dateien legen (Bestand dort ist englisch).

## AX25_PIDINCL: verweigert, und warum kein "alle Protokolle" möglich ist

`AX25_PIDINCL` (SOL_AX25) heißt im Kernel: die **PID ist Teil der Nutzdaten** —
das erste Byte jedes Frames in und aus dem Socket ist die Protokoll-ID. Die
Anwendung will also alle Protokolle in *einem* Stream sehen bzw. senden.

Der wampes-Kern bindet seinen UI-Listener aber scharf an **eine** PID:
`axlisten_ui_deliver()` überspringt jeden Eintrag mit `lp->pid != pid`
(`axserver.c:1128`). Es gibt **keinen Any-PID-Wildcard-Listener**; wer alle
254 Protokolle bekommen will, müsste für jede PID einzeln `listen ... pid=<n>`
anmelden (254 Claims, jeder mit eigener Verbindung).

Beide Backends antworten deshalb auf `AX25_PIDINCL` mit `ENOPROTOOPT`
(`axsock_opt_refuse`, `axsock.c:487`; `wampes_setsockopt`, `wampes.c:1901`).
Grund: Akzeptiert man die Option und tut nichts, schreibt die Anwendung
(rsuplnk/rsdwnlnk) ihr PID-Byte *in die Daten* — auf dem Weg zu wampes wird
die PID zum gewöhnlichen Payload, und beim Empfang wird das erste Byte einer
Nachricht als PID fehlinterpretiert. Das wäre stiller, dauerhafter
Datenschaden. Das saubere `ENOPROTOOPT` lässt die Programme an ihrem
Returnwert prüfen und abbrechen.

## Merkliste (später entscheiden)

* **Nutzen des Spiegels trotz lückenhaftem Verbindungsbild**: `listen` zeigt
  dank des Spiegels die UI/Datagramm-Frames — zumindest das Linklose lässt
  sich damit tracen. Für mheard ist der Spiegel speisbar, aber die
  S/C-Frame-Information (SABM/UA/DISC/REJ/FRMR etc., verbundene I-Frames)
  geht verloren. Trotzdem interessant: die gespiegelten Frames liefern
  **Paketstatistiken** (Anzahl UI- und I-Pakete pro Rufzeichen, First/Last
  Heard) für den wampes-Verkehr.
* **Namenwahl**: Der Name **`ax25mond`** darf nicht für ein neues Werkzeug
  verwendet werden — dieses Programm existiert bereits (`ax25mond(8)`,
  ax25-apps: liest `/etc/ax25/ax25mond.conf`, wartet auf Verbindungen auf den
  dortigen Sockets und re-transmittiert an sie, was es auf dem AX.25-
  Monitorsocket empfängt). Es wäre zu prüfen, ob **das bestehende**
  `ax25mond` für unsere Zwecke nutzbar ist (Verteiler der gespiegelten
  Frames an listen/mheardd), bevor ein eigener Name/Programm entsteht.
* **Konfiguration vereinfachen (kein Export-Gefummel)**: Damit der Spiegel
  und der wampes-Pfad überhaupt greifen, müssen beim Aufruf
  `AXSOCK_BACKEND=wampes`, `WAMPES_SOCKET=/tcp/sockets/ax25` und (für die
  neue Lib, solange sie nicht installiert ist) `LD_PRELOAD=...` gesetzt sein.
  Das ist für Alltags-Nutzer zu kompliziert und niemandem zumutbar sich zu
  merken. Zu optimieren: die Werte an einem Ort konfigurierbar machen
  (z.B. in `axports`/`wampes.conf` respektive durch `make install` der neuen
  libax25, sodass `LD_PRELOAD` entfällt), damit `beacon`/`listen` ohne
  Export-Zeilen laufen.
* **Sendepfad mit wiederholtem Bit — GELÖST**: Da wampes auf dem Sendeweg
  `*` kennt (`datagram_line` `remote_net.c:904-927`), ist nun durchgereicht,
  was libax25 vorher wegließ. Umsetzung in `wampes.c`:
  - `#define AX25_REPEATED 0x80` nach oben verschoben (war hinter
    `wampes_sendto` definiert und daher für den Sendepfad unbenutzbar).
  - `wampes_sendto` (`wampes.c:1557-1568`) hängt jedem Digi mit gesetztem
    `AX25_REPEATED`-Bit ein `*` an den TNC2-Header (`ax25_ntoa` + `strncat`
    `"*"`). Der Node setzt daraus via `htonax25` (`ax25hdr.c:92-95`) wieder
    das on-air-H-Bit; `nextdigi` regelt dabei das bereits Wiederholte.
  - Der TX-Spiegel (`wampes_mirror_path`, `wampes.c:1462-1472`) maskt das
    H-Bit nicht mehr weg (`& 0x7F` entfernt): die alte Begründung „der Node
    sieht es nie, also ist es nicht auf dem Draht" stimmt nicht mehr, weil
    die App es jetzt wirklich an den Node und damit an den Port bringt.
    E2E-belegt (Build-Lib, pnode + Observer): pnode empfing
    `[10]DB0FHN-13>DL1ABC,DB0AAA*:hello star`, Observer auf AGWPE-Port 255
    zeigte im KISS-Body das Digi-Byte `0xe1` (EOB+herkunftsbesetzte Bits+
    `0x80` repeated — vormals `0x61`).
  **Nachfolger — Lückenmuster normalisieren (GELÖST)**: Der Node kennt den
  Pfad nur als `hdr.nextdigi` (ein Index nach dem letzten `*`,
  `ax25hdr.c:158-170`) und serialisiert kontiguiert (`i < nextdigi` →
  REPEATED, `ax25hdr.c:92-95`). Ein sockaddr mit Lücke (repeated, nicht,
  repeated) kann on-air nicht existieren; `wampes_sendto` zieht den Pfad
  deshalb aus dem sockaddr in eine lokale Kopie und füllt alle Digis bis
  einschließlich des letzten REPEATED auf (`wampes.c`), ehe Frame und
  Spiegel daraus gebaut werden — Spiegel und Draht zeigen dasselbe.
  E2E-belegt: `TEST DB0FHN-8* -1 -2* DB0FHN -10 DL9SAU-1 TE2ST` wird zu
  `DB0FHN-8*,DB0FHN-1*,DB0FHN-2*,DB0FHN,...` normalisiert.
  **Einseitig (RX, GELÖST)**: `parse_ui_header` setzt das `*` aus der
  Node-Zeile pro Digi. Der Node markiert nur das letzte Wiederholte
  (`A,B,DB0FHN,DB0FHN-10*` — sein `nextdigi`-Blick auf den Pfad), also
  würde der sockaddr wieder Lücken bekommen. `parse_ui_header`
  normalisiert deshalb symmetrisch zu `wampes_sendto`: letztes Digi mit
  REPEATED suchen, den Präfix bis dorthin auffüllen — der
  `recvfrom()`-Aufrufer sieht das Frame, wie es wirklich war. Beide
  Node-Formate sind akzeptiert: kompakt (`A,B,DB0FHN,DB0FHN-10*`) und
  ausgeschrieben (`A*,B*,DB0FHN*,DB0FHN-10*`); beide landen im selben
  sockaddr. E2E-belegt: pnode sendete `[5]A>TEST,B,DB0FHN,DB0FHN-10*:hallo`,
  recvtest sah src=A, ndigis=3, H-bit=set auf allen drei Digis.
  **Treiber**: APRS-Gating — direwolf hört per Soundcard-Modem (AGWPE),
  eine Gateway-App leitet das Frame per `sendto` auf `wampes:...`-Port
  weiter (z.B. 70cm-aprs); die H-Bits aus dem empfangenen Frame bleiben
  erhalten. Das Bit liegt im `sockaddr` (`fsa_digipeater[n].ax25_call[6]`,
  parser `|= AX25_REPEATED`, `wampes.c:1741`); SOCK_DGRAM +
  `full_sockaddr_ax25` mit `AX25_REPEATED` im Digi-Byte ist der Weg, es
  zu transportieren (bewusst kein `setcall`-Pfad; frischer Sender hat
  nichts „schon wiederholt", ein `*` bleibt aber jetzt möglich).
