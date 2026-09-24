# Das Protokoll zwischen libax25 und einem AX.25-Knoten — für Entwickler

*Deutsche Fassung von `AX25-WITHOUT-KERNEL-DEVELOPER.md`.  Bei Zweifeln gilt
das englische Original; es ist die Fassung, die gepflegt wird.  Fachbegriffe
stehen bewusst englisch, wo sie im Code so heißen.*

Teil zwei.  Der Sysop-Teil daneben sagt, was der Aufbau tut; dieser sagt wie —
ausführlich genug, dass ein zweiter Knoten dagegen geschrieben werden könnte.

Genau darum wird es aufgeschrieben.  WAMPES ist heute der Knoten, aber nichts
hier ist ihm eigen: der Treffpunkt ist ein **Socket und eine Grammatik in
Zeilen**, und jedes Programm mit einem AX.25-Stack — TheNetNode, ein XNET,
etwas Eigenes — kann dieselbe anbieten.  Jedes `libax25`-Programm erreicht es
dann, ohne dass eine Zeile geändert wird.

---

## Der Dienstsocket

Ein Unix-Stream-Socket, in einer WAMPES-Standardinstallation
`/tcp/sockets/ax25`, auf der Clientseite in `wampes.conf(5)` benannt.  Das
Dateisystem ist die gesamte Zugriffskontrolle — ab Werk das Verzeichnis
darum, `sockets/` mit 0750, während der Socket selbst mit 0666 angelegt wird;
dem Knoten kann gesagt werden, die Bedingungen statt dessen am Socket zu nennen
(`axsock group`, `axsock mode`).  Eine Anmeldung gibt es im Protokoll nicht,
und eine Identität ebenso wenig.  Derselbe Dienst kann über TCP auf dem
Loopback angeboten werden (`axsock tcp-listen on`), was diese Kontrolle
aufgibt: eine TCP-Verbindung trägt keine Kennung, nach der man abstufen könnte.

**Ein Stream, kein Datagrammsocket.**  Diese eine Tatsache formt alles
Folgende: nichts darauf hat eine eigene Grenze, also endet jede Nachricht
entweder an einem Zeilenende oder nennt ihre eigene Länge.  Sie ist auch der
Grund für zwei der Fehler, die beim Bau gefunden wurden — beide am Ende
beschrieben.

Eine Verbindung trägt eine Sache.  Ein Client, der einen ausgehenden Ruf, einen
Listener und einen Datagrammsender will, öffnet drei.

---

## Die Befehle

Jeder ist eine Zeile.  Ein Knoten antwortet nur, wenn er etwas zu sagen hat;
Erfolg ist meist Schweigen, und genau eine Zeile beginnt mit `***`.

### `binary` und `ascii`

```
-> binary
```

Ob der Knoten auf dieser Verbindung Zeilenenden umsetzt.  `binary` heißt: tut
er nicht, und das will ein Programm — ein AX.25-Socket war das, was der Kernel
gab, und der Kernel setzte nichts um.  `ascii` ist für einen Menschen an einem
Terminal.

Die Vorgabe folgt der *Art* des Eintrags, zu dem eine Sitzung gehört, nicht der
Verbindung: ein Client bekommt binary, ein aus einem Pfad gestartetes Programm
oder eine über TCP gewählte Sitzung ascii.  `datagram` schaltet seine
Verbindung selbst auf binary, weil ein counted frame aus Bytes besteht.

### `handover`

```
-> handover
```

Bittet den Knoten, den nächsten `connect` mit einem **Deskriptor** zu
beantworten, statt diese Verbindung zur Sitzung werden zu lassen.  Es wird vor
`connect` gesendet, und ein Knoten, der das Wort nicht kennt, antwortet nichts
darauf — dann ist die Verbindung die Sitzung, wie zuvor.  Keine Aushandlung,
keine Version, kein Hin und Her.

**Warum ein ausgehender Ruf das überhaupt will**, ist nicht offensichtlich, und
es hat nichts mit der Richtung zu tun.  Die Dienstverbindung hat der *Client*
gemacht, ihr Typ ist also seiner: ein Stream.  Kernel-AX.25 war
`SOCK_SEQPACKET` — ein `write()` ist ein Rahmen — und Protokolle, die auf einer
Verbindung reiten, verlassen sich darauf; FBBs komprimiertes Forwarding liest
das Ende eines unkomprimierten Blocks an der Rahmengrenze ab.  Ein Knoten, der
das Paar selbst anlegt, kann wählen:

```c
if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0) return 1;
if (socketpair(AF_UNIX, SOCK_STREAM,    0, sv) == 0) return 0;
```

SEQPACKET, wo das System es hat, ein Stream, wo nicht — macOS bietet es auf
einem Unix-Socket nicht an.  Dort gehen die Grenzen verloren, was diese eine
Art von Dienst kostet und sonst nichts.

### `connect`

```
-> connect [<iface>:]<ziel>[ via <digi>[,<digi>...]][ < <quelle>][ --pid 0x<nn>]
<- link setup (hfb)...                     Fortschritt, darf ignoriert werden
<- *** connected to <ziel>                 mit einem Deskriptor daneben
```

Das Interface-Präfix ist der Teil hinter dem Doppelpunkt in einem
`axports(5)`-Eintrag und dasselbe, das ein Operator an einem RMNC tippt.
`< quelle` ist das Quellrufzeichen aus `bind()`; der Knoten prüft es gegen
nichts.  `--pid` ist das dritte Argument von `socket()`, das Kernel-AX.25
ignorierte und jedes Programm als 0 übergibt.

Jede Zeile, die nicht mit `***` beginnt, ist Fortschritt.  Genau eine tut es,
und die ist die Antwort.  Ein Dateiende davor heißt, der Link kam nie zustande.

**`connect()` blockiert hier, und es lohnt zu wissen, wo genau.**  Der Aufruf
schreibt das Kommando und liest dann Zeilen, bis das Urteil kommt; dieses Lesen
ist die Wartezeit, und sie dauert so lange, wie der Knoten braucht, um den Link
aufzubauen oder aufzugeben — T1 mal N2, auf einem schlechten Kanal also
zehnersekunden.  Eine eigene Frist gibt es nicht: gibt der Knoten auf, schließt
er, und das Dateiende wird zu `ETIMEDOUT`.

`O_NONBLOCK` verkürzt das nicht, und der Grund ist die Reihenfolge der Dinge.
Zu diesem Zeitpunkt ist der Deskriptor der Anwendung noch der Platzhalter;
`fcntl()` wird gar nicht abgefangen, eine dort gesetzte Marke sagt also nichts
über die Dienstverbindung, auf die gewartet wird.  Erst wenn `connect()`
zurückkehrt, ist der Deskriptor die Sitzung selbst, und von da an tut
`O_NONBLOCK` genau das, was draufsteht — und genau dann setzt es auch jedes
Programm der Suite, für seine E/A-Schleife.

Ehrlich `EINPROGRESS` zu antworten hieße, den Deskriptor genau dann schreibbar
zu machen, wenn der Link steht, und von allein kann er das nicht: vor dem Ruf
ist er ein Platzhalter und danach ein socketpair-Ende, in beiden Fällen sofort
schreibbar.  Über ihn zu signalisieren hieße, `poll()` und `select()`
mitzufangen — die Bibliothek zurück in den Datenweg, und das ist das eine, was
dieser Entwurf vermeiden soll.  Also nicht angeboten statt fehlend.
Kernel-AX.25 blockierte an derselben Stelle, ein Programm sieht also keinen
Unterschied.

Die Absagen tragen einen Grund, und der ist ihr ganzer Sinn:

| Zeile | errno |
|---|---|
| `*** link failure with X - no route` | `EHOSTUNREACH` |
| `*** link failure with X - busy` | `EADDRINUSE` |
| `*** link failure with X - nomem` | `ENOBUFS` |
| `*** link failure - invalid call "…"` | `EINVAL` |
| `*** no interface "X"` | `ENODEV` |
| Dateiende ohne Urteil | `ETIMEDOUT` |

### `listen`

```
-> listen [ui] [pid=<n>] <call>
<- *** listening on <call> pid 0x<nn>
```

Beansprucht ein Rufzeichen.  Die Konfiguration des Sysops ist die Erlaubnis —
ein Rufzeichen ohne `client`-Eintrag kann nicht beansprucht werden — und dies
ist der Anspruch dagegen.

| Absage | errno |
|---|---|
| `*** X is already taken` | `EADDRINUSE` |
| `*** X is not open for clients` | `EACCES` |
| `*** X belongs to a port` | `EADDRNOTAVAIL` |

**Der Schlüssel ist (Rufzeichen, PID, verbunden-oder-UI), und ein Client hält
ihn.**  Ein Rose-Daemon und eine Textanwendung können sich also ein Rufzeichen
teilen; zwei Programme, die dieselbe PID wollen, nicht.  Der Anspruch lebt
genau so lange wie die Verbindung, auf der er gestellt wurde — ein Client, der
stirbt, hinterlässt nichts.

`pid=` nimmt Namen wie Zahlen — `pid=netrom` und `pid=0xcf` sind dasselbe —,
während die Antwort die Zahl behält, weil ein Client sie liest.

Ein `pid=any` gibt es nicht, und es einzuführen ist weniger naheliegend, als
es aussieht.  Es spart dem Sysop einen Eintrag je PID und lässt ein Programm
alles auf einem Rufzeichen nehmen — ein berechtigter Wunsch.  Im Weg steht die
Übergabe: die Sitzung kommt als roher Deskriptor, und liegen mehrere
Protokoll-IDs auf einem Strom, kann der Client sie nicht auseinanderhalten,
denn die PID steht nicht in den Bytes.  Sie mitzuführen hieße gezählte Form
statt Übergabe, so wie es in der Datagrammrichtung schon ist — also eine
andere Art von Client und nicht bloß eine lockerere Suche.  Als Aussicht
vermerkt; entschieden wird es im Knoten.

Auf der AGWPE-Seite hat dieselbe Frage eine härtere Antwort.  `X` meldet ein
Rufzeichen an und trägt gar keine PID, der Server gibt seinem einen Besitzer
also alles, was an dieses Rufzeichen geht, und sortieren muss der Client.  Der
Shim sortiert auch — ein Lauscher, der die PID des Rahmens beansprucht, hat
Vorrang —, aber zwei *Prozesse* können ein Rufzeichen so nicht teilen, weil
nur einer es beim Server besitzen kann.  Über einen Knoten geht das, und das
ist der schärfere Grund, für mehrere Dienste auf einem Rufzeichen das
Knoten-Backend zu nehmen.

### `datagram`

```
-> datagram [<iface>:] [--pid 0x<nn>] [--silent]
```

Versetzt die Verbindung in den Datagrammbetrieb; der Knoten antwortet nur bei
einem Parse-Fehler.  Ohne Interface gehen die Rahmen über jedes AX.25-Interface
hinaus, was eine Bake auf einem knotenweiten Eintrag verlangt.

Mit einem Ziel auf derselben Zeile steht der Kopf für die ganze Sitzung fest
und jede Zeile ist Nutzlast.  Ohne eines — die Form, die `libax25` benutzt —
bringt jeder Rahmen seinen eigenen Kopf mit, in **counted form**.

---

## Counted form

Beide Richtungen, und die einzige Gestalt, die beliebige Bytes überlebt:

```
[<n>]<quelle>><ziel>[,<digi>[*]...]:<genau n Bytes>
```

`[n]` steht ganz vorn, damit das erste Byte entscheidet und keine Nutzlast als
Länge gelesen werden kann.  Der Kopf endet am Doppelpunkt; die Nutzlast beginnt
unmittelbar danach und ist genau n Bytes lang.  **Kein Zeilenende beendet sie**
— CR, LF und NUL sind gewöhnlicher Inhalt, und das ist der ganze Zweck.

Ein `*` markiert ein Element, das den Rahmen bereits wiederholt hat.  Herein
trägt es das repeated-bit ins SSID-Byte des Digipeaters, wohin es gehört.
Hinaus schreibt `libax25` es nicht, was eine Lücke ist und keine Entscheidung.

Senden, ein Rahmen:

```
-> datagram hfb:
-> [17]DL9SAU-2>DL1ABC,DB0AAA:dies ist ein test
```

Empfangen, nach `listen ui`:

```
<- [5]DL1ABC>DB0FHN-13,DB0AAA*:hallo
```

Man beachte, was *nicht* hier steht: kein Längenfeld in einem Kopf von uns,
kein Escaping, keine Framing-Schicht.  Der Zählwert ist die Rahmengrenze, die
ein Stream nicht hat.

---

## Wie aus einem Anruf ein `accept()` wird

Diesen Teil liest man zweimal, denn hier wechselt der Deskriptor den Besitzer.

1. Die Anwendung ruft `listen()`.  Der Shim öffnet eine Dienstverbindung,
   sendet den Anspruch, und — das ist der Kniff — **diese Verbindung wird zum
   listening descriptor**.  `poll()` und `select()` darauf funktionieren deshalb
   ohne jedes Zutun von uns: er ist genau dann lesbar, wenn ein Anruf wartet.

2. Je eingehendem Anruf legt der Knoten ein `socketpair` an, gibt ein Ende an
   dieselbe Maschinerie, die auch ein gestartetes Programm bedient, und sendet
   das andere mit `SCM_RIGHTS` — zusammen mit einer Spurzeile, in **einem**
   `sendmsg()`:

   ```
   hfa DL1TST-1,DB0BBB > DL9SAU-13
   <port> <anrufer>[,<pfad>...] > <das erreichte Rufzeichen>
   ```

   Zusammen mit Absicht.  Ein Deskriptor, der allein ankäme, müsste einer
   getrennt ankommenden Zeile zugeordnet werden, und es gibt keinen Schlüssel,
   mit dem das ginge.

3. `accept()` ist dieses eine `recvmsg()` — genauer: es liest bis zum
   Zeilenende und sammelt den Deskriptor von dem Byte auf, das ihn trug.  Der
   Anrufer geht in die Adresse, die die Anwendung bekommt; das erreichte
   Rufzeichen beantwortet später `getsockname()`, woran `ax25d` seine Stanza
   erkennt.

Der Knoten sendet mit `MSG_DONTWAIT`: ein Client, der aufgehört hat,
`accept()` zu rufen, darf einen kooperativ geschedulten Knoten nicht anhalten.

**Eine NET/ROM-Sitzung kommt auf demselben Weg**, in derselben Gestalt
angekündigt, mit dem Knoten, auf dem der Benutzer sitzt, dort, wo sonst ein
Digipeater stünde:

```
netrom DL1ABC-7,DL1TST-1 > DL9SAU-8
```

Der Shim liest zwei Rufzeichen daraus und fragt nie, welches Protokoll den Ruf
getragen hat.

---

## Was der Shim mit einem Deskriptor macht

**Die Interception entscheidet und geht dann aus dem Weg.**  Sie ist kein
Proxy: nichts reicht Bytes weiter, und kein Thread sitzt zwischen der Anwendung
und ihrer Sitzung.  Jeder Aufruf wird so kurz wie möglich beantwortet und
weitergegeben — und der letzte gibt den Deskriptor selbst her, danach ist die
Bibliothek überhaupt nicht mehr beteiligt.

Die Reihenfolge der Aufrufe macht das möglich und erzwingt zugleich den einen
unbequemen Teil.  `socket()` kann den Port noch nicht kennen, und die Anwendung
will jetzt eine Nummer.  Also:

```
socket()    ein Platzhalter — ein ungebundener Unix-Socket, und ein echter
            Deskriptor
bind()      der erste Moment, in dem der Port bekannt ist, und damit der
            Moment, in dem das Backend gewaehlt wird: Kernel, AGWPE oder ein
            Knoten, je Port
connect()   das Gespraech von oben, dann dup2() der Sitzung auf die Nummer,
            die die Anwendung schon haelt
listen()    der Anspruch, und die Verbindung nimmt den Platz des Deskriptors
            ein
```

Ab `connect()` ist **nichts von uns im Datenweg**.  `read()`, `write()`,
`poll()` und `close()` erreichen den Kernel unmittelbar; das ist der ganze
Grund, einen Deskriptor zu übergeben statt Bytes weiterzureichen.

Dass `bind()` der Entscheidungsmoment ist, hat eine Folge, die genannt gehört,
weil sie es überhaupt erst erlaubt, das auszuprobieren: **das Backend wird je
Port gewählt, nicht je Prozess.**  Das Rufzeichen wird in `axports(5)`
nachgeschlagen; gehört sein Eintrag zu einem Knoten aus `wampes.conf(5)`,
wechselt der Socket den Besitzer — und zwar auch dann, wenn der Deskriptor vom
Kernel-Stack kam, denn die Übergabe ersetzt, was hinter der Nummer stand.  Ein
Programm kann also nacheinander einen Kernel-Port und einen Knoten-Port
erreichen, in einem Prozess, ohne dass etwas konfiguriert wäre, das das sagt.

Das gilt für jeden Aufruf, der einen Socket benennt, nicht nur für `connect()`:
ein Deskriptor ist ab seinem eigenen `bind()`, was er ist, ein Prozess kann
also gleichzeitig auf einem Kernel-Port und auf einem Knoten-Port lauschen,
jeder über seine eigene Maschinerie.

Noch *nicht* je Port ist die Frage Kernel-oder-AGWPE: sie wird einmal je
Prozess beantwortet, durch eine einzelne Probe beim ersten AX.25-Socket, und
zwischengespeichert.  Deshalb sind diese beiden das eine Paar, das sich nicht
mischen lässt, während jedes von beiden sich mit Knoten-Ports mischt.  Auch das
je Port zu machen ist der naheliegende nächste Schritt und braucht keine
Protokolländerung — nur dieselbe Nachschlagerei eine Schicht tiefer.

Ein Datagrammsocket beansprucht sein Rufzeichen bei `bind()` und nicht erst
beim ersten `recvfrom()`, damit `select()` lesbar wird, wenn ein Rahmen
ankommt, statt nie.  Eine Ablehnung dort ist kein Fehler — ein Beacon bindet
auch, meist das Rufzeichen eines Ports, das kein Client beanspruchen darf —,
also wird der Grund aufgehoben und dem gegeben, der `recvfrom()` ruft.

Über AGWPE geschieht dasselbe aus demselben Grund: `bind()` meldet das
Rufzeichen an, ein eingehender Unproto-Rahmen wird gegen die Datagrammsockets
gehalten, die es führen, und die PID wählt zwischen ihnen.  Anders ist die
Rahmung auf dem Weg zur Anwendung — der Deskriptor ist eine Socketpaar-Hälfte
und damit ein Bytestrom, also kommt jeder Rahmen hinter einer Länge und dem
Rufzeichen des Absenders, und genau daraus kann `recvfrom()` ihn benennen.

Beide Wege gibt es, und welcher gilt, folgt dem Port.  Auf dem Loop-Port von
`ax25netd(8)` stellt der Daemon einen UI-Rahmen dem zu, der das Zielrufzeichen
angemeldet hat — er kommt direkt an.  Überall sonst tut das niemand: AGWPE
kennt keine UI-Zustellung je Rufzeichen, nur den Mitschnittstrom.  Ein
Datagrammsocket dort bittet also um Rohmitschnitt und holt sich heraus, was er
will.  Das ist kein Umweg — jeden Rahmen zu tragen ist der Sinn eines
Monitorkanals.

Die Kopie der eigenen Aussendung wird unterdrückt, denn einer Station werden
ihre eigenen Rahmen nicht zugestellt.  Nur die: ein Digipeater, der uns
wiederholt, trägt dasselbe Quellrufzeichen und ist ein anderer Rahmen — der
Beweis, dass der Hop stattfand —, und derselbe Rahmen auf einem anderen Port
gehört sagt etwas über das Netz.  Ein Echo ist alles drei zugleich: gleicher
Port, nichts wiederholt, gleicher Rahmen.

### Welche Aufrufe abgefangen werden

`socket`, `bind`, `connect`, `listen`, `accept`, `send`, `sendto`, `write`,
`recv`, `recvfrom`, `shutdown`, `close`, `setsockopt`, `getsockopt`, `ioctl`,
`getsockname`, `getpeername`.  Alles, was nicht AX.25 ist, fällt zum echten
Aufruf durch, abgesichert durch einen Zähler, der null ist, bis der erste
AX.25-Socket existiert.

`read()` ist mit Absicht **nicht** dabei: der Deskriptor ist von sich aus
lesbar.  Wie die Bibliothek auf dem jeweiligen System überhaupt vor ein
Programm kommt, steht in `axsock(7)` — das unterscheidet sich: starke Symbole
auf ELF, eine Interpose-Tabelle auf macOS.

### Wenn der Kernel-Stack da ist

Ob es einen gibt, wird einmal gefragt, beim ersten AX.25-Socket, indem einer
geöffnet wird:

```c
int fd = real_socket(AF_AX25, SOCK_SEQPACKET, 0);
```

* er geht auf — das Modul ist geladen, also nimm es
* `EAFNOSUPPORT`, `EPROTONOSUPPORT`, `EPFNOSUPPORT` — die Familie hat niemand
  registriert, es gibt also keinen nativen Stack, und dann eben der
  Userspace-Weg
* alles andere, etwa `EPERM` — Kernel bleibt gewählt, damit das `socket()` des
  Aufrufers den echten Grund meldet, statt still umgeleitet zu werden

Gefragt wird eine Tatsache, nicht eine Politik gewählt: *ist das Modul
geladen*, einmal.

Ein Kernel-Deskriptor steht danach in keiner unserer Tabellen, und jeder
abgefangene Aufruf fällt sofort zum echten durch — `send`, `sendto`, `write`,
`recv`, `recvfrom`, `shutdown`, `close`, `setsockopt`, `getsockopt`, `ioctl`,
`getsockname`, `getpeername`, `listen`, `accept`, `connect`.  Zwei Aufrufe
stehen nicht auf dieser Liste:

* `socket()`, wo die Frage oben gestellt wird
* `bind()`, das angesehen wird, weil dort ein Rufzeichen einen Port benennt —
  und gehört der Port zu einem Knoten, wechselt der Deskriptor genau dort den
  Besitzer, ein Kernel-Socket eingeschlossen

Eine Folge auf einer Maschine mit beidem, die man wissen sollte, bevor sie
jemanden ratlos macht: `axctl(8)` und `axkill(8)` öffnen einen Socket und
setzen ein `ioctl` ab, ohne je zu binden — sie erreichen also die Verbindungen
des Kernels.  Eine Sitzung, die über einen Knoten läuft, können sie nicht
steuern.

**Ausblick.**  Verschwindet der Kernel-Stack endgültig, ist der natürliche
Endzustand, dass er zu einem dritten Backend derselben Gestalt wird wie die
beiden anderen — ein Dispatch, drei Plugins, nirgends ein Sonderfall.  Heute
ist er das mit Absicht nicht: ein Kernel-Deskriptor steht in keiner unserer
Tabellen, und jeder Aufruf darauf fällt sofort durch, was den einen Pfad, der
nachweislich funktioniert, von allem unberührt lässt, was wir tun.

### Was ein Kind erbt

`ax25d` gibt einem Dienst die Verbindung auf Deskriptor 0, und das Kind fragt
`getpeername(0)`, wer ruft.  Über ein socketpair antwortet der Kernel
`AF_UNIX`, also sagt der Elternprozess in einer Variablen, wer da ist:

```
AXSOCK_INHERIT=<fd> <eigenes Rufzeichen> <Gegenstelle>
```

Die Bibliothek liest sie beim Laden und antwortet für diesen Deskriptor.  Kein
Kind muss geändert werden.  Sie wird nach dem Lesen aus der Umgebung entfernt.

---

## Tracen

Vier Aussichtspunkte, und der Trick ist zu wissen, welcher welche Frage
beantwortet.

```
AXSOCK_DEBUG=1 <programm>     jede Entscheidung der Bibliothek und das ganze
                              Gespraech mit dem Knoten.  Hier faengt man an:
                              "die Bibliothek ist nicht geladen", "dieser Port
                              ist kein Knotenport", "der Knoten hat uns
                              abgewiesen" und "der Knoten laeuft nicht" sehen
                              von aussen gleich aus und hier voellig
                              verschieden

strace / dtruss               ob der Aufruf die Bibliothek ueberhaupt erreicht
                              hat.  Ein socket(AF_AX25, ...), das mit
                              EAFNOSUPPORT scheitert und danach keinen
                              Unix-Socket zeigt, heisst: nichts hat abgefangen

svc.py <socket> <befehl>      den Dienstsocket von Hand sprechen.  Der
                              schnellste Weg herauszufinden, ob eine Absage
                              unsere ist oder die des Knotens

fakewampes.py, fakedgram.py,  ein Knoten, den es nicht gibt.  Jeder Fehler
fakeuirx.py                   unten wurde mit einer dieser Attrappen gefunden
                              oder bestaetigt
```

Die Attrappen sind die zwanzig Zeilen wert, die sie kosten.  Ein Knoten, den
man **absichtlich falsch spielen lassen** kann — eine Zeile zerteilen, einen
Deskriptor übergeben und die Zeile nie beenden, einen Anspruch beantworten und
dann ein Datagramm hinterherschieben — macht aus „manchmal fällt eine Sitzung
weg" einen Test, der jedes Mal fehlschlägt.

---

## Fallen, alle auf die harte Tour gefunden

Aufgeschrieben, weil jede einen Abend gekostet hat und eine zweite
Implementierung denselben begegnen wird.

**Ein Stream liefert keine Nachrichten aus.**  `sendmsg()` mit `MSG_DONTWAIT`
nimmt bei knappem Puffer *einen Teil* einer Nachricht an und meldet, wie viel.
Ein Knoten, der das für Erfolg hält, hat einen Deskriptor übergeben und beendet
die Zeile nie, die ihn beschreibt; ein Client, der annimmt, ein `recvmsg()` sei
eine Zeile, reicht einen halb geparsten Anruf nach oben und lässt den Rest für
das nächste `accept()` liegen, das dann eine Zeile ohne Deskriptor findet.
Lies bis zum Zeilenende; schreib zu Ende, was Du angefangen hast.

**`EAGAIN` ist eine Aufforderung, keine Absage.**  Es sagt: nichts genommen,
komm wieder — und ein nichtblockierendes Schreiben, das darin eine Antwort
sieht und den Rest des Rahmens wegwirft, verliert Daten, ohne dass es jemand
merken könnte.  Auf einem Bytestrom-Socketpaar ist der Verlust nicht einmal
ein fehlender Rahmen, sondern ein Loch mitten im Strom.  Gemessen: mit einem
Leser, der drei Sekunden schlief, kam genau der Puffer an und ein Drittel der
Übertragung war weg.  Behalte, was nicht hineinpasste, und schiebe nach, wenn
der Deskriptor wieder nimmt; eine Obergrenze, hinter der die Sitzung laut
geschlossen wird, ist das ehrliche Ende dieses Wegs — und genau das tut
`ax25netd(8)` für seine eigenen Clients längst.

**Ein Deskriptor, den die Anwendung hält, gehört nicht Dir.**  Ihn früh zu
schließen kostet, was sie noch nicht gelesen hat — und, schlimmer und leiser,
gibt die Nummer an den Prozess zurück, während die Anwendung sie noch
benutzt.  Das nächste `open()` kann dieselbe bekommen, und von da an liest das
Programm in einer fremden Datei.  Schließe Dein eigenes Ende und lass es das
Dateiende sehen.

**Ein Eigentümer, sonst gibt der zweite frei, was der erste noch hält.**  Ein
angenommener Socket hat hier einen Faden, der für ihn weiterleitet, und eine
Anwendung, die ihn schließen darf; wer abräumt, muss einmal festgelegt sein
und nicht je nach Fall.  Es war je nach Fall festgelegt — der Faden besaß ihn
nur, solange die Sitzung stand —, also wurde ein Socket, den man schließt,
nachdem die Gegenstelle aufgelegt hat (das gewöhnliche Ende einer Sitzung),
von beiden freigegeben.  Das bricht in `malloc` ab und nimmt das Programm mit,
und es braucht einen *zweiten* Ruf im selben Prozess, um sich zu zeigen —
weshalb es hier drei Wochen niemand sah und `ax25d(8)` es am ersten Tag
gesehen hätte.

**Eine Anfrage ist keine Antwort, auch wenn es derselbe Rahmen ist.**  Ein
Connect hat drei Schreibweisen auf dem Hinweg — schlicht, über Digipeater, mit
Protokoll-ID — und ein Daemon, der den Rahmen weiterreicht, wie er kam, gibt
das Wort des Clients als das des Servers aus.  Ein Client nach Spezifikation
kennt nur die eine Schreibweise, die ein Server benutzt, und verpasst den Ruf
vollständig.  Normalisiere auf dem Rückweg und behalte, was die andere
Schreibweise trug, dort wo es hingehört.

**Dateiende ist kein Fehler, und es ist auch nicht nichts.**  Ein `read()`, das
0 liefert, ist die Art, wie eine geschlossene Sitzung ankommt, wenn sie kein
Kernel-Socket ist — ein socketpair hat keine Ausnahmebedingung zu melden.  Code,
der nur auf `< 0` prüft, hält das für einen kurzen Lesevorgang, schreibt null
Bytes und dreht mit 100 % CPU, solange der Prozess lebt, denn Dateiende bleibt
lesbar.

**Ein Zählwert ist keine Zeile.**  Alles, was Nutzlast trägt, braucht die
counted form.  Ein Rahmen mit einem CR darin ist nichts Exotisches; es ist
das, was eine gewöhnliche AX.25-Station sendet.

**Die Sockelart gehört zur Identität.**  Wenn ein Backend bei `bind()` einen
Deskriptor an ein anderes übergibt, muss die bei `socket()` entschiedene Art
mitgehen, sonst kommt ein Datagrammsocket als Verbindung an und der Sendeweg
fällt bis zum Deskriptor selbst durch.

**Nicht jedes Programm benennt den Port gleich.**  `bind()` trägt den Port im
ersten Digipeater-Slot, und `call(1)` legt ihn immer dorthin — `beacon(8)` nur
dann, wenn sein `-c` vom Portrufzeichen abweicht.  Also auch das
Quellrufzeichen nachschlagen.

**macOS hat kein `SOCK_SEQPACKET` für Unix-Sockets** und keinen Platz für
`sa_len` in einer Linux-förmigen `sockaddr`.  Frag das System, statt es
anzunehmen; nimm einen Stream, wenn die Antwort nein lautet, und sag, was das
kostet.

---

## Socket-Optionen, und die zwei Sorten davon

Unter `SOL_AX25` stehen zwei verschiedene Sorten, und sie lassen sich nicht
gleich beantworten.

Die meisten sind Kanalparameter — `AX25_WINDOW`, die Timer, `AX25_PACLEN` —
und die besitzt die Gegenseite.  Ein Knoten oder ein `direwolf` nimmt sie aus
seiner eigenen Interface-Konfiguration und würde ignorieren, was hier gesagt
wird; also meldet `setsockopt()` Erfolg und sagt einmal je Option auf
Standardfehler, dass es ins Leere ging.  Nichts hängt daran, dass der Wert
ankam: `call(1)` liest `window` aus `axports(5)` und setzt es, und läuft so
wie so.

Zwei ändern, was die Bytes bedeuten.  `AX25_PIDINCL` legt die Protokoll-ID vor
die Nutzlast jedes Rahmens, in beiden Richtungen, und `AX25_IAMDIGI` macht den
Socket zum Wiederholer.  Die anzunehmen und nichts zu tun kostet keine
Eigenschaft, es verdirbt Daten — `rsuplnk(8)` würde die PID als Nutzdaten
senden und Nutzdaten als PID lesen, still und dauerhaft.  Umsetzbar ist keine
von beiden über eine übergebene Sitzung, die ein schlichter Bytestrom ohne
Platz für eine ID je Rahmen ist, also antworten beide `ENOPROTOOPT`.  Beide
Programme prüfen den Rückgabewert und hören auf, und das ist das gewollte
Ergebnis: nicht laufen ist besser als falsch laufen.

`getsockopt(SOL_AX25)` antwortet mit Nullen.  Nichts in der Suite liest einen
Wert zurück.

---

## Nicht gebaut: Listener, die sich selbst anmelden

Ein Listener ist hier administrativ.  Der Sysop schreibt `listen ax25 add
<call> client`, und ein Programm beansprucht ihn; die Zeile ist die Erlaubnis
und der Anspruch wird dagegen gestellt.  Das war eine Entscheidung und kein
Versäumnis — eine Datei sagt, welche Rufzeichen die Kontrolle des Knotens
verlassen dürfen, und es ist eine Datei, die der Sysop ohnehin liest.

Ein Protokoll, bei dem `bind()` und `listen()` des Programms das Rufzeichen von
selbst anmelden — der Knoten lernt also, wofür gelauscht wird, statt es vorher
gesagt zu bekommen —, ist vorstellbar und durchaus wünschenswert (der Vergleich
mit UPnP ist nicht ganz ein Kompliment und nicht ganz unfair).  Wer es baut,
muss diese Fragen beantworten:

* **Wer darf was anmelden.**  Ohne die Zeile des Sysops sind die Rechte am
  Socket das einzige verbliebene Tor.  Entweder ist das die Antwort — wer den
  Socket öffnen darf, darf alles anmelden — oder der Knoten muss fragen, wer da
  spricht; siehe den nächsten Abschnitt.
* **Welche Rufzeichen.**  Das eigene mit beliebiger SSID, ein Muster, ein
  Bereich, oder alles.  Die eigenen Interface-Rufzeichen des Knotens müssen so
  oder so außer Reichweite bleiben, wie heute.
* **Die Einstellungen, die eine statische Zeile trägt.**  `pid` und
  verbunden-oder-UI sendet das Protokoll schon, aber ein Eintrag hält auch eine
  Interface-Liste, binary-oder-ascii, silent und wait.  Eine Anmeldung trägt
  sie mit oder nimmt Vorgaben — und die Vorgaben sind nicht für jede Art von
  Eintrag dieselben.
* **Vorrang.**  Ein konfigurierter Eintrag muss gewinnen.  Eine Anmeldung, die
  ihn verdecken oder ersetzen könnte, machte aus einer Konfigurationsdatei
  einen Vorschlag.
* **Das Übernahmeloch, laut ausgesprochen.**  Ein Dienst, der gerade nicht
  läuft, lässt sein Rufzeichen frei.  Mit statischen Einträgen hat der Sysop
  wenigstens die Menge der Rufzeichen benannt, denen das passieren kann; bei
  freier Anmeldung ist diese Menge alles.  Entweder ist das hinnehmbar, oder
  die Identitätsfrage muss zuerst beantwortet sein.
* **Lebensdauer.**  Der Anspruch stirbt schon heute mit der Verbindung, was die
  richtige Gestalt ist; was eine erneute Anmeldung bedeutet — abweisen,
  ersetzen oder teilen — hat noch niemand beantworten müssen.
* **Absagen bleiben unterscheidbar.**  Sie werden auf der anderen Seite zu
  errnos, und ein Client, der „vergeben" nicht von „nicht erlaubt" trennen
  kann, kann nichts Brauchbares melden.

---

## Nicht gebaut: Zugriffskontrolle von der Unix-Seite

Welche Unix-Identität welche AX.25-Ressource benutzen darf — ein Rufzeichen zum
Hinausrufen, eines zum Lauschen, eine PID, das repeated-bit.  Heute wird
diese Frage gar nicht gestellt: der Modus des Sockets ist ein Tor, und dahinter
ist alles erlaubt.

Der Dienste-Socket trägt keine Identität.  Der Knoten fragt nie, wer am anderen
Ende sitzt, und die Dateirechte sind die ganze Antwort — das ist einfach,
wirksam und ein wenig grob: ein Benutzer, der den Socket nicht öffnen darf,
kann auch nicht hinausrufen.

Ein Unix-Socket ließe sich fragen, und zwar billig:

```
Linux   getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)   struct ucred: pid, uid, gid
macOS   getpeereid(fd, &uid, &gid)
        getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, ...) struct xucred: uid und die
                                                       ganze Gruppenliste —
                                                       gemessen, 16 Gruppen kamen
                                                       zurueck
```

Auf Linux kommt nur die primäre Gruppe an, „ist dieser Benutzer in Gruppe
hams" braucht dort also `getpwuid()` und `getgrouplist()` dahinter.  Die
Kennung wird festgehalten, wenn die Verbindung entsteht, ein Prozess kann sie
danach also nicht wechseln.  Über TCP gibt es sie gar nicht — dieser Weg ist
nur dadurch geschützt, dass er an das Loopback bindet.

Damit ließe sich abstufen, was ein Programm darf — eine andere Frage als die,
wer einen Listener anmelden darf:

| | rausrufen als | darf lauschen | darf PID wählen | darf `*` setzen |
|---|---|---|---|---|
| eigene uid eines Dienstes (conversd) | sein Rufzeichen | ja | ja | ja |
| Gruppe `hamsoft` | beliebig | ja | ja | ja |
| Gruppe `hams` — ein gewöhnlicher Lizenzinhaber | eigenes Rufzeichen, SSID frei | nein | nur Text | nein |
| alle anderen | nichts | nein | nein | nein |

Die mittlere Zeile ist die interessante, und sie ist nicht hypothetisch: ein
lizenzierter Operator soll unter seinem eigenen Rufzeichen hinausrufen können
und sonst nichts — keine Quelle nach Wahl, kein Listener, kein Protokoll außer
Text.  Das ist nah an dem, was `axparms --assoc` für die ausgehende Hälfte tat,
und es ist die Zeile, die den Unterschied macht zwischen „wer den Socket öffnen
darf, ist Sysop" und „wer den Socket öffnen darf, darf ein gewöhnlicher
Benutzer sein".

Festzuhalten, weil beide Hälften heute ungeregelt sind — und zwar in
entgegengesetzte Richtungen: **jedes** Quellrufzeichen darf für einen
ausgehenden Ruf benutzt werden — nur seine Schreibweise wird geprüft —,
während ein Listener nur die Rufzeichen haben darf, die der Sysop geöffnet
hat.

Kernel AX.25 bietet das Spiegelbild der ersten Hälfte, und es lohnt, genau zu
sein, weil es *angeboten* statt *getan* ist.  `ax25_bind()`:

```c
user = ax25_findbyuid(current_euid());
if (user) call = user->call;              /* REPLACES what he asked for */
else if (ax25_uid_policy && !capable(CAP_NET_ADMIN))
        return -EACCES;
else call = addr->fsa_ax25.sax25_call;    /* he may call himself anything */
```

Die Zuordnung greift also nur für Benutzer, die **einen** Eintrag **haben**,
und `ax25_uid_policy` ist **0** standardmäßig (`AX25_NOUID_DEFAULT`).  Ohne
`axparms --assoc policy deny` bindet jeder lokale Benutzer ein beliebiges
Rufzeichen — der Kernel benotet ausgehende Rufe nicht, er liefert einen
Schalter zum Benoten und lässt ihn aus.  Wo sie greift, ist es eine uid zu
genau einem Rufzeichen: keine SSID-Spanne, keine Gruppen, kein Unterschied
zwischen Hinausrufen und Lauschen.  Und sie ersetzt **still** statt zu
verweigern — der eine Teil, den es nicht zu kopieren lohnt: ein Programm, das
glaubt, DL9SAU-7 zu sein, und still zu etwas anderem gemacht wird, hat keine
Möglichkeit, es zu merken.

Zum Lauschen brauchte es dort überhaupt keine Erlaubnis.  In dieser Hälfte
sind wir also schon strenger als der Kernel-Default, und das ist ein Argument
*für* das Verwaltungsmodell oben, nicht dagegen.  Keine der beiden Formen ist
falsch; unsere ist einfach gewachsen statt gewählt worden, und ein Knoten, der
nach Zugangsdaten fragte, könnte wählen.

Die letzte Spalte ist das repeated-bit, falls es gesetzt wird: einen
Rahmen so auszusenden, als hätte ein Sprung schon stattgefunden, ist das, was
ein Knoten tut, der sich in einen Pfad einfügt, und es ist kein Betriebsverhalten.
Es steht deshalb in derselben Rechtezeile wie ein Quellrufzeichen, das einem
nicht gehört — beides sind Arten, auf dem Kanal jemand anders zu scheinen, und
beides gehört zur Software, die die Station betreibt, und nicht zu der Person,
die sie benutzt.

---

## Was ein zweiter Knoten bauen müsste

Kürzer, als es aussieht:

* einen Unix-Stream-Socket und seine Rechte
* `binary`, `handover`, `connect`, `listen`, `datagram` wie oben — fünf Wörter
* eine `***`-Zeile je Befehl, mit ausgeschriebenen Gründen, denn daraus wird
  auf der anderen Seite ein errno
* `SCM_RIGHTS` mit der Zeile und dem Deskriptor in einem `sendmsg()`
* die counted form für Datagramme, in beide Richtungen
* eine Tabelle der Ansprüche, geschlüsselt nach Rufzeichen, PID und
  verbunden-oder-UI

Alles andere — Timer, Wiederholungen, Routing, der Kanal — ist die eigene Sache
des Knotens und nicht unsere.  Das ist die Arbeitsteilung, die dieser ganze
Aufbau ziehen soll.
