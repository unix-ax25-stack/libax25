# AX.25 ohne Kernel-Stack — für den Sysop

*Deutsche Fassung von `AX25-WITHOUT-KERNEL-SYSOP.md`.  Bei Zweifeln gilt das
englische Original; es ist die Fassung, die gepflegt wird.*

Der AX.25-Stack verlässt den Linux-Kernel mit 7.1.  Alles, was gegen `libax25`
geschrieben ist — `call`, `ax25d`, `axspawn`, Mailboxen, DX-Cluster,
Convers-Server — erwartet, dass `socket(AF_AX25, …)` funktioniert, und auf
einer Maschine ohne den Kernel-Stack antwortet darauf nichts mehr.

Dies ist ein Ausweg: **`libax25` beantwortet die Socket-Aufrufe selbst und gibt
die Arbeit an ein Programm weiter, das einen vollständigen AX.25-Stack im
Userspace hat.**  Heute ist das WAMPES.  Die Anwendungen merken davon nichts;
neu übersetzt wird nichts.

Es ist ein **Proof of Concept**, und zwar keiner auf dem Papier: es trägt
echten Verkehr auf **DB0FHN-10**.  Schaut es Euch an, verbindet Euch, macht es
kaputt.  Rückmeldungen und Fehlerberichte sind ausdrücklich erwünscht —
mehrere der bisher behobenen Fehler wurden genau so gefunden, dadurch dass
jemand sich anmeldete und etwas sich seltsam verhielt.

Es ist außerdem als **Referenzimplementierung** gemeint und nicht als private
Absprache.  Zwischen `libax25` und WAMPES steht ein kleines Protokoll auf einem
Unix-Socket, aufgeschrieben in `AX25-WITHOUT-KERNEL-DEVELOPER.md` daneben.
Jedes andere AX.25-fähige Programm kann es lernen — TheNetNode, ein XNET,
etwas Eigenes — und jedes `libax25`-Programm erreicht es dann, ohne dass eine
Zeile geändert wird.  Der Kernel-Stack war dreißig Jahre lang der Treffpunkt;
dies ist ein Vorschlag für den nächsten.

AGWPE, das `libax25` ebenfalls spricht, ist ein eigenes Thema und wird hier
nicht beschrieben: es ist der Weg zu einem `direwolf` oder zu AGWPE-Clients im
LAN, und seine Konfigurationsdateien haben mit dem Folgenden nichts zu tun.

---

## Wie es aussieht

```
   call, ax25d/axspawn, conversd, eine Mailbox
                  |
                  |  socket(AF_AX25, …), bind, connect, listen, accept
                  v
              libax25                      ein Unix-Socket
                  |  ------------------------------------------->  WAMPES
                  |  <-------------------------------------------
                  |     ein Deskriptor je Sitzung, uebergeben
                  v
     die Sitzung IST der Deskriptor — libax25 ist aus dem Weg
```

Zwei Dinge folgen daraus, und sie sind der ganze Grund für diesen Aufbau:

* **Sobald die Verbindung steht, ist nichts von uns im Datenweg.**  Der
  Deskriptor, den die Anwendung hält, *ist* die Sitzung.  `read()`, `write()`,
  `poll()` und `close()` gehen zum Kernel und sonst nirgendwohin.
* **Das AX.25 macht der Knoten.**  Timer, Wiederholungen, Digipeating,
  Routing, der Kanal — alles bleibt dort, wo es schon funktionierte.

---

## Was einzurichten ist

**Ausgehende Verbindungen brauchen nicht mehr als zwei Dateien.**  Ein
Programm, das hinauswählt — `call`, eine weiterleitende Mailbox, ein Cluster,
der einen Feed holt — funktioniert, sobald `axports` und `wampes.conf`
zusammenpassen.  *Gerufen zu werden* ist der dritte Schritt, und der ist
optional: den braucht nur eine Station, die antwortet.

### 1. `axports`: über welches Interface ein Programm hinausgeht

```
# Name        Rufzeichen  Baud  paclen  window  Beschreibung
wampes        DB0FHN-10   9600  256     2       WAMPES-Knoten
wampes:xnet   DB0FHN-2    9600  256     2       Knoten wampes, Interface xnet
```

Der Teil vor dem Doppelpunkt sagt, **welcher Knoten** gemeint ist; er wird auf
der Linux-Seite aufgelöst und geht nie hinaus.  Der Teil dahinter benennt
**eines der AX.25-Interfaces des Knotens** und reist als das Präfix, das WAMPES
ohnehin versteht (`connect xnet:DB0AAA`) — dasselbe, das ein Operator an einem
RMNC oder XNET tippt.

Beide Formen dürfen nebeneinanderstehen, wie oben: `axports` weist doppelte
Namen und doppelte Rufzeichen zurück, und diese unterscheiden sich in beidem.
Ein Eintrag ohne Suffix überlässt die Wahl des Interfaces dem Knoten, der den
Ruf dann routet; einer mit Suffix legt es fest.

Ein Eintrag je Interface und nicht je Knoten, weil jedes Interface ohnehin ein
eigenes Rufzeichen hat.  Dieses Rufzeichen benutzt eine ausgehende Verbindung
als Quelle, sofern das Programm nicht selbst eines bindet — `call -s` tut das.

### 2. `wampes.conf`: wo der Knoten lauscht

```
# Name    Adresse                  Beschreibung
wampes    /tcp/sockets/ax25        der Knoten auf dieser Maschine
```

Diese Datei ist zugleich das, was einen Eintrag zu einem WAMPES-Eintrag macht:
ein Port gehört zu einem Knoten, wenn der Name vor dem Doppelpunkt hier
auftaucht.  Nichts wird aus der Schreibweise erraten, kein Name ist reserviert,
und ein über TCP erreichbarer Knoten wird als `host:port` geschrieben.

**Mit diesen beiden funktioniert der ausgehende Betrieb.**  Es gibt keinen
Daemon von uns zu starten, keinen Port zu öffnen, keinen Zustand
gleichzuhalten.

### 3. Optional: gerufen werden

Damit ein `libax25`-Programm für ein Rufzeichen antworten darf, muss dem Knoten
gesagt werden, dass dieses Rufzeichen herausgegeben werden darf.  In WAMPES'
`net.rc`, je eine Zeile:

```
listen ax25 add db0fhn-9  client        # ax25d/axspawn: der Login
listen ax25 add db0fhn-11 client        # conversd
```

Auf das Wort `client` kommt es an: es heißt *gib die Sitzung an denjenigen
weiter, der dieses Rufzeichen über den Dienstsocket anfordert* — im Gegensatz
dazu, ein Programm zu starten oder einen TCP-Port zu wählen.  Ein Rufzeichen
ohne eine solche Zeile kann überhaupt nicht beansprucht werden — das ist die
Zugangsregel in einem Satz.

Diese Zeilen betreffen **verbundene Sitzungen**.  UI-Rahmen haben einen eigenen
Listener, und ein Programm, das sie empfangen will, braucht ihn:

```
listen ax25 add ui db0fhn-13 client     # UI-Rahmen fuer dieses Rufzeichen
```

Beide dürfen für dasselbe Rufzeichen nebeneinanderstehen — einer für
Verbindungen, einer für Datagramme.

**Ein Eintrag ist ein Rufzeichen, eine PID und verbunden-oder-nicht, und ein
Programm hält ihn.**  Das ist die ganze Regel, und zweierlei folgt daraus.  Ein
zweites Programm, das ein bereits gehaltenes Rufzeichen anfordert, wird mit
`*** DB0FHN-13 is already taken` abgewiesen; es bekommt nicht stillschweigend
eine zweite Kopie des Verkehrs.  Und weil die PID zum Schlüssel gehört, sind
Einträge, die sich darin unterscheiden, verschiedene Einträge:

```
listen ax25 add ui pid=text db0fhn-13 client    # ein Programm
listen ax25 add ui pid=rose db0fhn-13 client    # ein anderes, gleichzeitig
```

Jeder Rahmen geht an den Eintrag, dessen PID er trägt.  Ein Rose-Daemon und
eine Textanwendung können sich also ein Rufzeichen teilen, ohne voneinander zu
wissen; zwei Programme, die *dieselbe* PID wollen, können es nicht.

Ein erneutes `add` mit demselben Rufzeichen, derselben PID und derselben Art
legt keinen zweiten Eintrag an, sondern ersetzt den ersten.

**Damit sind wir strenger als der Kernel**, und ein Programm, das sich auf das
alte Verhalten verließ, merkt es.  Gemessen auf einer Maschine mit
Kernel-Stack: drei Prozesse banden nacheinander dasselbe Rufzeichen mit
derselben PID, und keiner wurde abgewiesen — Kernel-AX.25 behandelt einen
AX.25-Datagrammsocket wie ein rohes IP-Protokoll und nicht wie einen
UDP-Port.  Ob dann alle drei eine Kopie eines eingehenden Rahmens *bekommen*
hätten, ist ungeprüft, und genau das entscheidet, ob der Unterschied in der
Praxis zählt — es bräuchte einen Rahmen von einer zweiten Station, denn eigene
Aussendungen erreichen die eigenen Datagrammsockets nicht, auch die
digipeatete Wiederholung nicht.  Hier hält ein Programm ein Rufzeichen und
bekommt die Rahmen; ein zweites erfährt das, statt im Ungewissen zu bleiben.

---

## Neben dem Kernel-Stack, nicht an seiner Stelle

Nichts hier verlangt von einer Station, umzustellen.  **Die Wahl fällt je
Port**, und eine Maschine mit funktionierendem Kernel-AX.25 kann einen
Knoten-Port hinzunehmen und alles andere lassen, wie es ist.

Das ergibt sich daraus, wann die Entscheidung überhaupt fallen kann.
`socket()` muss einen Deskriptor zurückgeben, bevor irgendwer weiß, welcher
Port gemeint ist — das Rufzeichen und damit der Port kommen erst bei `bind()`.
Also wird dort gefragt: das Rufzeichen wird in `axports(5)` nachgeschlagen, und
gehört der Eintrag zu einem Knoten aus `wampes.conf(5)`, wechselt der Socket
genau dort den Besitzer.  Alles andere bleibt beim Kernel.

Für ein Programm heißt das:

```
call -r kernelport DL1ABC        geht an den Kernel-Stack
call -r wampes:xnet DL1ABC       geht an den Knoten
```

im selben Prozess, nacheinander, ohne dass etwas gesetzt oder umgeschaltet
wird.  Eine Mailbox lässt sich Rufzeichen für Rufzeichen umziehen und genauso
wieder zurück — eine bequeme Ausgangslage zum Testen: es gibt keinen Stichtag
und keinen Rückabwicklungsplan zu schreiben.

Das gilt fürs Lauschen wie fürs Hinauswählen: jeder Socket wird bei seinem
eigenen `bind()` entschieden, ein `ax25d` kann also gleichzeitig auf einem
Kernel-Port über den Kernel und auf `wampes:xnet` über den Knoten lauschen.

Ein Paar verträgt sich nicht, und das gehört benannt: **Kernel-Ports und
AGWPE-Ports.**  Welches von beiden ein Prozess benutzt, wird weiterhin einmal
entschieden, bei seinem ersten AX.25-Socket, und gilt dann für alle; nur
Knoten-Ports werden je Port gewählt.  Kernel und Knoten mischen sich frei,
AGWPE und Knoten mischen sich frei — Kernel gegen AGWPE muss vorerst in
getrennten Programmen leben.

---

## Wer es benutzen darf

**Das Dateisystem ist die gesamte Zugriffskontrolle.**  Es gibt keine Anmeldung
am Dienstsocket, kein Passwort, keine Konfiguration je Benutzer — wer
`/tcp/sockets/ax25` öffnen kann, darf den Knoten benutzen.

Das ist Absicht, und deshalb liegt der Socket dort, wo er liegt: ein
Unix-Socket trägt seine Rechte im Dateisystem, und das ist ein Werkzeug, das
Ihr schon kennt.

**Ab Werk ist das VERZEICHNIS das Tor, nicht der Socket:**

```
drwxr-x---  2 root staff  /tcp/sockets          0750, und das ist das Tor
srw-rw-rw-  1 root staff  /tcp/sockets/ax25     0666, absichtlich offen
```

Das wirkt verkehrt herum, bis man sieht, welches von beiden entscheidet.  Das
Verzeichnis trägt die Gruppe, ein `chgrp hams /tcp/sockets` verschiebt also den
ganzen Dienst von einer Gruppe in die andere, ohne dass über Socket-Modi
nachgedacht werden muss.  (Der Socket ist `0666` und nicht `0777`, weil
`connect()` auf einem Unix-Socket nach **Schreibrecht** fragt und nie nach
Ausführungsrecht — gemessen, und Linux sagt es in `af_unix.c`.)

Wer die Bedingungen lieber am Socket selbst nennt: `net.rc` läuft, nachdem er
existiert.

```
axsock group hams
axsock mode 0660          →  srw-rw----  1 root hams  /tcp/sockets/ax25
```

Dann dürfen Mitglieder der Gruppe den Sender benutzen und sonst niemand kann
sich auch nur verbinden.  Feinere Abstufung ist Sache des Dateisystems und
nicht unsere — eine Gruppe je Daemon, eine ACL für ein Konto, was Eure Maschine
ohnehin kann.

Diese beiden Zeilen gehören in `net.rc` und nicht in die Finger, und der Grund
ist, dass **die Socket-Datei einen Neustart nicht überlebt**.  Ein startender
Knoten findet die alte im Weg und entfernt sie vor dem Binden — vorsichtig: nur
wenn es wirklich ein Socket ist, und nur wenn niemand darauf antwortet, damit
einem laufenden Knoten nie der seine genommen wird.  Was er dann anlegt, ist
eine neue Datei mit dem Vorgabemodus.  Ein von Hand getipptes `chmod` ist in
diesem Moment weg; eine Zeile in `net.rc` wird jedes Mal wieder angewandt.

**Sagt es aus, bevor Ihr es weiter aufmacht.**  Wer den Socket öffnen kann,
kann:

* jedes Rufzeichen beanspruchen, das der Sysop mit `client` geöffnet hat, sofern
  gerade kein Programm es hält.  Wer zuerst kommt — läuft `ax25d` also nicht,
  kann ein anderes Programm `DB0FHN-9` nehmen und die Anmeldungen selbst
  beantworten.
* unter **jedem** Quellrufzeichen hinausconnecten.  `< SRC` wird gegen nichts
  geprüft; der Modus des Sockets ist es, der sagt, wer senden darf.

Was diese Person **nicht** kann:

* ein Rufzeichen nehmen, das jemand schon hält — der Knoten antwortet
  `*** DB0FHN-9 is already taken` und gibt nichts heraus.
* ein Rufzeichen beanspruchen, das niemand konfiguriert hat (`is not open for
  clients`) oder das zu einem Port gehört (`belongs to a port`).
* eine Sitzung sehen oder stören, die schon an jemand anderen übergeben wurde.
  Jede Sitzung ist ein eigenes Socket-Paar, dessen eines Ende an den einen
  Client geht, der das Rufzeichen beansprucht hat.  Es gibt keine Möglichkeit,
  eine Kopie zu erbitten, und keinen Monitorstrom zum Mithören.

Kurz: **die Grenze verläuft im Dateisystem, und sie ist echt — aber alles
innerhalb davon genießt Vertrauen.**  Behandelt die Mitgliedschaft in dieser
Gruppe so, wie Ihr das Recht behandelt, den Sender zu benutzen, denn genau das
ist es.

Lest diese Liste als das, was *möglich* ist, nicht als das, was wahrscheinlich
ist.  Ein Dienst bindet beim Hochfahren der Maschine und hält sein Rufzeichen,
solange er läuft, und was gehalten wird, kann nicht genommen werden.  Es ist
dieselbe Klasse wie die Tatsache, dass ein Unix-Benutzer den TCP-Port 443
binden darf: völlig richtig, und nicht die Art, wie jemandes Webserver
übernommen wird, denn der Webserver war zuerst da und besitzt den Socket.  Und
ein Anspruch kann immer nur auf das passen, was der Sysop ohnehin
hingeschrieben hat — das Rufzeichen, seine SSID, und wo `pid=` es sagt, das
Protokoll.

Der Knoten kann denselben Dienst auch über TCP anbieten (`axsock tcp-listen
on`, nur 127.0.0.1 und ::1; `axsock` allein zeigt, ob er an ist).  Dieser
Schalter nimmt das Dateisystem aus dem Bild: jedes Konto auf der Maschine
erreicht ihn dann, und eine TCP-Verbindung trägt keine Kennung, nach der man
abstufen könnte.  Schaltet ihn ein, wenn Ihr genau das meint.  Er ist aus,
solange `net.rc` nichts anderes sagt.

---

## Die Schalter an einem Listener

`listen ax25 add` nimmt Optionen, und die Vorgaben sind so gewählt, dass die
üblichen Fälle keine brauchen.  `listen ?` am Knoten zeigt die vollständige
Liste.

| | was er tut | Vorgabe bei `client` |
|---|---|---|
| `--binary` / `--ascii` | ob der Knoten Zeilenenden umsetzt | **binary**, und das ist das Gewünschte |
| `--silent` / `--verbose` | ob der Knoten den Anruf in die Sitzung ansagt | **silent, und erzwungen** |
| `--wait` | den Dienst nicht starten, wenn der Link steht, sondern warten, bis der Anrufer etwas sendet | aus |

`--wait` ist nützlicher, als es aussieht.  Ohne ihn startet der Dienst in dem
Moment, in dem der Link steht — was ein Login-Prompt will, aber nicht immer.
Ein eingehendes SABM kann für ein Protokoll mit einer anderen PID gedacht sein
(etwa einen Rose-Daemon im Userspace), und eine Textanmeldung kann eine sein,
bei der der Anrufer selbst entscheidet, wann er angemeldet wird — durch eine
Leerzeile.  In beiden Fällen käme die Begrüßung, bevor jemand danach gefragt
hat.

Zwei davon verdienen einen Satz, weil falsches Raten hier still bleibt:

**Binary ist bei `client`-Einträgen die Vorgabe und sollte es bleiben.**  Die
Gegenseite ist ein `libax25`-Programm, das die Packet-Radio-Konvention selbst
spricht — das CR einer Station ist auf dem ganzen Weg ein CR.  Eine Umsetzung
für sie beschädigt, ohne es zu sagen.  Die anderen Arten von Eintrag haben die
umgekehrte Vorgabe: ein aus `/pfad/programm` gestartetes Programm oder eine an
`tcp:host:port` gewählte Sitzung bekommt ascii, weil ein gewöhnliches
Unix-Programm LF will.

**`--verbose` hat bei einem `client`-Eintrag keine Wirkung.**  Der Knoten
erzwingt dort Schweigen, und aus gutem Grund: der Client erfährt vom Anruf in
*derselben* Nachricht, die den Deskriptor trägt, eine zusätzlich in die Sitzung
gesendete Ansage wäre also das Erste, was die Gegenseite als Daten liest.  Ein
Programm, das wissen will, wer gerufen hat, liest es aus der Übergabezeile —
es bekommt die rufende Station und das erreichte Rufzeichen — und dass die
Sitzung endete, erfährt es aus dem Dateiende auf seinem Deskriptor.  Ein
DX-Cluster bekommt also beide Ereignisse; nur eben nicht als Text im Datenstrom.

---

## Programme, die nie gegen libax25 gelinkt wurden

Ein Programm, das `AF_AX25` selbst öffnet, ohne je eine `libax25`-Funktion zu
rufen, lässt sich trotzdem bedienen: die Bibliothek vor der C-Bibliothek laden,
und sie beantwortet die Socket-Aufrufe.  Neu übersetzt wird nichts.

```
LD_PRELOAD=/usr/lib/libax25.so.0 programm
```

`README-linux-systemd.txt` hat das ausgearbeitete Beispiel — `conversd` aus
[conversd-saupp](https://github.com/dl9sau/conversd-saupp), mit seiner
Unit-Datei und den zwei Dingen, die man leicht falsch macht.  `axsock(7)` hat
die ganze Geschichte, einschließlich dessen, was Preloading nicht erreicht.

---

## Datagramme

UI-Rahmen gehen in beide Richtungen.  `beacon(8)` und alles andere, das sie
sendet, funktioniert: der Rahmen wird mit unversehrtem Pfad an den Knoten
gegeben und verlässt das Interface, als hätte der Knoten ihn selbst dorthin
gesendet — eine Brücke, kein Router.  Ein Programm, das ein Rufzeichen bindet,
das der Sysop mit einem `ui`-Eintrag geöffnet hat, empfängt sie, und
`recvfrom()` trägt ein, wer den Rahmen gesendet hat und über welchen Pfad.

Was einen Rahmen beendet, ist ein Zählwert und kein Zeilenende; CR, LF und NUL
in der Nutzlast sind also Inhalt und kommen an, wie sie gesendet wurden.

Der Empfang braucht den `ui`-Eintrag von oben, und die PID entscheidet, welches
Programm welchen Rahmen bekommt.  Das Senden braucht überhaupt keinen Eintrag:
jedes Programm, das den Socket öffnen darf, darf senden, unter dem Rufzeichen,
das es bindet.

---

## Was es nicht gibt

Sagt Euch das laut, denn jedes davon ist still und nicht laut:

* **Kein Monitor.**  `listen(1)`, `mheardd(8)` und alles andere, das rohe Rahmen
  beobachtet, sieht über einen WAMPES-Port nichts.  Das Dienstprotokoll hat
  keinen Monitorstrom — nichts schickt eine Kopie jedes Rahmens zurück, wie es
  der AGWPE-Weg tut.  Der Socket wird herausgegeben und bleibt still, mit einer
  Zeile auf der Standardfehlerausgabe, die es sagt.  Der Knoten schreibt auf
  seiner eigenen Konsole mit; dort ist nachzusehen.
* **Das Wiederholt-Bit reist nur in eine Richtung.**  Ein ankommender Rahmen
  bringt seinen Pfad mitsamt `*` — die Marke wird zum Bit im SSID-Byte des
  Digipeaters, wie auf dem Band.  Hinaus fällt sie weg: der Pfad wird aus der
  Adresse geschrieben, wie sie ist, und nichts trägt die Marke.  Es gibt
  Anwendungen dafür — einen Rahmen weiterleiten, oder festhalten, dass der
  erste Sprung schon erfolgt ist — es ist schlicht noch nicht gebaut.
* **Ende zu Ende, kein eigenes Digipeating.**  Was `libax25` anbietet, ist eine
  Sitzung zwischen zwei Stationen.  Es ist kein Weg zu einem seitlich
  angeschlossenen TNC, und es wiederholt für niemanden.

---

## Wenn es nicht geht

In dieser Reihenfolge, weil jeder Schritt den vorigen ausschließt:

```
AXSOCK_DEBUG=1 <programm>       welches Backend geantwortet hat, und das
                                ganze Gespraech mit dem Knoten

ax25d -l                        Verbindungsprotokoll ins syslog.  Ohne das
                                wird ein Ruf, der auf keinen Eintrag in
                                ax25d.conf passt, wortlos geschlossen

die Spur des Knotens            was er von dem Ruf hielt
```

Nach dem ersten greift man zuerst.  „Die Bibliothek ist nicht geladen", „der
Port ist kein WAMPES-Port", „der Knoten hat uns abgewiesen" und „der Knoten
läuft nicht" sehen von außen gleich aus und in dieser Ausgabe völlig
verschieden.

Zwei Fallen, die echte Abende gekostet haben:

* **`--enable-userspace-ax25`.**  Auf Linux ist die Interception *aus*, ab
  Werk.  Eine ohne sie gebaute Bibliothek enthält nichts davon, und sie
  vorzuladen bewirkt gar nichts.  Siehe
  `README-hints-for-non-kernel-AX25-hosts.txt`.
* **Die `configure`-Zeile neu tippen.**  `config.status` merkt sich die
  Argumente und `make` ruft es von selbst wieder auf; die Zeile erneut aus
  einer README abzuschreiben ist der Weg, auf dem der falsche Prefix — oder ein
  fehlendes `--enable-userspace-ax25` — hereinkommt.

---

## Stand, und was helfen würde

Proof of Concept, im echten Dienst auf DB0FHN-10, offen entwickelt.  Die bisher
gefundenen Fehler wurden durch Benutzen gefunden: eine Anmeldung, die wortlos
auflegte; eine Bake, die Erfolg meldete und ins Leere ging; ein Dienst, der
nach dem Weggehen des Benutzers mit 100 % CPU lief.  Alle drei waren alte
Fehler, die der Kernel-Stack zugedeckt hatte, und alle drei fand jemand, dem
etwas seltsam vorkam.

Also: verbindet Euch, meldet Euch an, lasst etwas Echtes dagegen laufen und
sagt, was passiert ist.  Die Konfiguration, die auf DB0FHN-10 läuft, ist die
oben beschriebene — hinter dem Vorhang steht nichts weiter.
