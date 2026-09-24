# Der WAMPES-Spiegel auf den AGWPE-Monitor-Kanal

Wie Frames, die ein libax25-Programm über das Backend **wampes** sendet und
empfängt, in den AGWPE-**Monitor-Kanal 255** gelangen, wo ein `listen`-Programm
sie mit dem Wiederholt-Markierer (`*`) an den Digis zeigt, die bereits
weitergeleitet haben. AGWPE, direwolf und ax25netd werden nicht angefasst.

## Was es ist

Nicht der WAMPES-**Node** speist den Monitor, sondern die **libax25-Client-
Seite** der wampes-Brücke. Ein Programm, das über **wampes** mit dem Funk
verbunden ist, hält neben seinem Session-Kanal einen AGWPE-Client am Loop-Port
255 von ax25netd und **spiegelt** jedes wampes-UI-Frame — ein- wie ausgehend —
als Raw-`'K'`-Record. Der Session-Datenfluss wird nicht berührt.

Der Spiegel trägt **nur wampes-Verkehr**. Was ein Programm über AGWPE
(direwolf) sendet und empfängt, wird nicht noch einmal gespiegelt: dort füllt
ax25netd den Monitor ganz von selbst, und eine zweite Kopie wäre eine
Doppelung.

Warum diese Aufteilung: ax25netd verbindet sich als AGWPE-**Client** an jede
Upstream-Quelle (direwolf) und reicht jedes eingehende Raw-Frame unverändert,
inklusive der H-Bits, an seine Monitor-Clients weiter. Ein WAMPES-Node ist kein
AGWPE-Upstream und gibt ax25netd nichts zum Abgreifen — deshalb legt die
libax25-Seite der wampes-Brücke diese Frames selbst auf den Loop-Port.

Wo der Spiegel anbindet, sucht auch das AGWPE-Backend: `AXSOCK_HOST`/
`AXSOCK_PORT` (Default `127.0.0.1:8100`; führendes `/` ist ein Unix-Socket;
`AXSOCK_USER`/`AXSOCK_PASSWORD` melden sich an, wo der Server das verlangt).
Eine abgerissene Verbindung wird verworfen und beim nächsten Frame neu
aufgebaut. Ein nicht erreichbarer Server scheitert **still** — die wampes-
Brücke selbst funktioniert ohne ax25netd weiter.

wampes-Textframes kennen keine Protokoll-ID; der Spiegel stempelt ihnen 0xf0
(AX.25) auf, wenn der Socket keine eigene PID gesetzt hat.

## Das Wiederholt-Bit

Auf dem Monitor liegen zwei Frame-Arten. Raw-`'K'`-Records tragen das
Wiederholt-Bit als `0x80`-Bit im SSID-Byte jedes Digis — 1:1. Dekodierte
Records (`'I'`/`'S'`/`'U'`/`'T'`) zeigen es als `*` hinter dem Namen, das
direwolf oft weglässt. libax25 und `listen` nutzen nur die Raw-Records, also
kommt das `*` als echtes Bit an, genau wie ein Digi es stehen ließ.

Auf dem Weg hinein meldet der Node den Pfad mit `*` an den bereits
wiederholten Hops; die Brücke macht daraus das Wiederholt-Bit und normalisiert
die Lücken, die der Node in einem Pfad lässt (er markiert nur den letzten).
Auf dem Weg hinaus setzt eine Anwendung das Bit im SSID-Byte des `sendto(2)`-
sockaddr; die Header-Zeile an den Node trägt das `*` wieder, und der Node
sendet das On-Air-Bit aus. Da ein Pfad auf dem Funk zusammenhängend ist,
füllen beide Richtungen jeden Digi bis einschließlich des letzten Wiederholten
auf — Spiegel und Draht zeigen dasselbe.

## Was nicht zu sehen ist

Der Spiegel hängt am Datagramm-Pfad (`sendto`/`recvfrom`) und deckt **nur
UI-Frames** ab. Eine verbundene AX.25-Session (connect) läuft vollständig
daran vorbei: die Bibliothek übergibt der Anwendung einen gewöhnlichen
Descriptor, und danach kreuzt nur das I-Frame-Payload die Brücke — nie die
Steuerrahmen (SABM/SABME/UA/DISC/REJ/FRMR), nie die N(S)/N(R)-Folge, nie die
Adress-Header, die im Session-Zustand des Nodes liegen. Aus dem, was die
libax25-Seite sieht, lässt sich kein listen-tauglicher KISS-Frame bauen.

Für UI- und Datagramm-Verkehr funktioniert der Monitor also; für verbundene
Sessions braucht es eine Quelle im wampes-Kern — einen Monitor-Export auf
seinem LAPB/AX.25-Layer, so etwas wie ein wampes-internes `listen`. Ein
Frontend an der libax25↔wampes-Session kann das Bild eines verbundenen Links
grundsätzlich nicht liefern.

## Was bewusst nicht geändert wird

- AGWPE, direwolf und ax25netd bleiben, wie sie sind. Ihre Frames werden nur
  einmal gespiegelt — von ax25netd selbst.
- `listen(1)` braucht keine Änderung: es zeigt `*` bereits aus dem
  Wiederholt-Bit.
- `setcall()` bleibt `*`-ablehnend. Das Wiederholt-Bit ist eine
  Adresseigenschaft, gesetzt im sockaddr, nicht etwas, das man in einen Namen
  tippt.

## Offene Punkte

- **Protokoll-ID**: wampes-Textframes kennen keine PID; der Spiegel stempelt
  0xf0, wenn der Socket nichts gesetzt hat. Eine verlorene PID ist eine
  bekannte Grenze der Idee.
- **Still bei Nichterreichbarkeit**: Ein fehlendes ax25netd oder Loop-Port
  meldet der Anwendung keinen Fehler. Der Kanal ist rein beobachtend.
- **Verteilung an Listener**: Bevor ein neuer Name entsteht, ist zu prüfen, ob
  sich das bestehende `ax25mond(8)` (ax25-apps, ein Monitor-Socket-Verteiler)
  mit den gespiegelten Frames füttern und an listen/mheardd weiterreichen
  lässt.
- **Konfiguration**: Den Spiegel (und den wampes-Pfad überhaupt) zu nutzen
  braucht heute `AXSOCK_BACKEND=wampes`, `WAMPES_SOCKET=...` und, solange die
  neue Bibliothek nicht installiert ist, `LD_PRELOAD=...` beim Aufruf. Das ist
  zu viel zum Merken; die Werte wollen an einem Ort liegen — `axports`/
  `wampes.conf`, und kein Preload mehr, sobald `make install` gelaufen ist.