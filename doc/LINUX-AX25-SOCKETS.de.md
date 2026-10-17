# Die Socket-Strukturen des Linux-AX.25-Kernels — Referenz

Nachschlagewerk für die Brücke zwischen dem Linux-Kernel-AX.25 und dem
WAMPES-Knoten.  Es beantwortet die Fragen, die beim Bau von §11.8
(rohes Senden) und beim Verständnis von `datagram`, `monitor` und
`SO_BINDTODEVICE` aufkamen — und die man sonst jedes Mal neu im Kernel
nachlesen muss.

**Quellen (verifiziert am 2026-10-17, Linux v6.6):**
`net/ax25/af_ax25.c`, `net/ax25/ax25_dev.c`, `net/ax25/ax25_in.c`,
`net/ax25/ax25_addr.c`, `net/socket.c`, `net/core/sock.c`.
Im Baum: `ax25-tools.git/kiss/net2kiss.c`,
`ax25-apps.git/listen/listen.c`, `libax25.git/axmon.c`.

**Status:** Referenz, keine Entscheidung.  Die Entscheidung zu §11.8
steht in der Design-Note; dieses Dokument ist ihre Begründung.

---

## 1. Welcher Socket kann was

| Socket | Transport | Adressierung | Interface | PID |
|---|---|---|---|---|
| `AF_AX25`,`SOCK_DGRAM` | UI | Zielruf + Pfad je `sendto()` | **pro Socket** (aus `bind`/`SO_BINDTODEVICE`) | Senden: `protocol`; Empfangen: nur `AX25_P_TEXT` |
| `AF_AX25`,`SOCK_SEQPACKET` | I-Frames | Verbindung | pro Socket | `protocol`, mit `AX25_PIDINCL` sichtbar |
| `AF_AX25`,`SOCK_RAW` | UI | Zielruf + Pfad + PID | pro Socket | `sk_protocol`, adressierend |
| `AF_PACKET`,`ETH_P_AX25` | roh | **je Paket** (ifindex/Gerätename) | **je Paket** | jede; liegt im Rahmen |

`AF_PACKET` ist die einzige Familie, bei der das Interface zum *Paket*
gehört statt zum Socket — in beide Richtungen.

---

## 2. `bind()` auf einem AF_AX25-Socket (`ax25_bind`)

`bind(fd, &addr, len)`:

1. **Rufzeichen wählen.**  Hat die effektive UID einen Eintrag in der
   UID↔Rufzeichen-Tabelle (`SIOCAX25ADDUID`, von `ax25d` gesetzt), dann
   **gewinnt dieser** und das Rufzeichen im `sockaddr` wird ignoriert.
   Sonst das Rufzeichen aus dem `sockaddr`; ist die UID-Policy scharf und
   kein `CAP_NET_ADMIN` da, `EACCES`.
2. `SOCK_ZAPPED` erforderlich — zweimal `bind` → `EINVAL`.
3. **Gerät wählen** (siehe 3.): `SO_BINDTODEVICE` gesetzt → das Gerät
   steht schon, keine Suche.  Sonst bei **genau einem** Digipeater
   (`ndigis == 1`, nicht die Null-Adresse) wird der **Digipeater-Slot
   als Portwähler** missbraucht (`ax25_addr_ax25dev(&digi[0])`), sonst
   das **Quellrufzeichen**.  Kein Treffer → **`EADDRNOTAVAIL`**.
4. `ax25_fillin_cb()` kopiert die Port-Vorgaben in das Verbindungsobjekt:
   T1/T2/T3, N2, Paclen, Window bzw. Extended-Window, Idle, Backoff.

`ax25_addr_ax25dev(addr)` (`ax25_dev.c`) läuft die Liste der
`ARPHRD_AX25`-Geräte ab und vergleicht `ax25cmp(addr, dev->dev_addr)`.
Die Liste füllt sich bei `NETDEV_UP` (`ax25_dev_device_up`), leert sich
bei `NETDEV_DOWN`.

**Daraus folgt:** Ein Port ist über das **Rufzeichen des Netdev**
identifiziert, nicht über den Gerätenamen.  Das ist genau das, was
`kissattach`/`axports` setzen — und der Grund für „port with call '%s'
is not active" im alten libax25 und für „no backend serves" bei
Rufzeichen-Mismatch (siehe §11.14 der Design-Note).

Beim `NETDEV_DOWN` räumt `ax25_kill_by_device()` auf: alle Sockets auf
dem Gerät bekommen `ENETUNREACH` und `ax25->ax25_dev = NULL`.

---

## 3. `SO_BINDTODEVICE` — zwei Bedeutungen, ein Zahlenwert

`SO_BINDTODEVICE` (25) existiert in **zwei** Ebenen, und für AX.25 ist
nur die zweite von Belang:

* **`level == SOL_SOCKET` (1):** der generische `sock_setsockopt()`
  (`net/socket.c`: `if (level == SOL_SOCKET && !sock_use_custom_sol_socket())`)
  setzt `sk->sk_bound_dev_if`.  `af_ax25.c` liest `sk_bound_dev_if`
  **nirgends** — für AF_AX25 bleibt das ohne Wirkung.

* **`level == SOL_AX25` (257):** erreicht `ax25_setsockopt()`, das mit
  `if (level != SOL_AX25) return -ENOPROTOOPT;` beginnt und deshalb in
  seinem `switch` nur SOL_AX25-Aufrufe sieht.  Dort:
  * Gerät per **Namen** (`__dev_get_by_name`);
  * muss ein AX.25-Gerät sein (`ax25_dev_ax25dev`), sonst `ENODEV`;
  * setzt `ax25->ax25_dev` und füllt die Vorgaben;
  * verweigert bei einem **lauschenden/verbundenen `SOCK_SEQPACKET`**
    mit `EADDRNOTAVAIL`;
  * `getsockopt(SOL_AX25, SO_BINDTODEVICE)` liefert den Portnamen.

**Nutzen:** `bind()` überspringt dann die Rufzeichen-Suche.  Damit lässt
sich ein **beliebiges** Rufzeichen auf einen **namentlich gewählten
Port** binden — das ist das alte `AX25_BIND_ANY_DEVICE` aus
`ax25-tools.git/ax25/ax25d.c` („`[CALL-X VIA *]`").  `AX25_BIND` als
Konstante gibt es weder im Kernel noch in `netax25/*.h`; gemeint ist
immer die Funktion `ax25_bind()` bzw. eben jenes historische Define.

---

## 4. Der PID

* **Senden:** der `protocol`-Parameter von
  `socket(AF_AX25, SOCK_DGRAM, protocol)` ist der PID (0 bzw. `PF_AX25`
  → `AX25_P_TEXT`, 0xF0).
  `ax25_sendmsg()`:
  ```c
  if (!ax25->pidincl)
      *(u8 *)skb_push(skb, 1) = sk->sk_protocol;
  ```
  Ohne `pidincl` setzt der Kernel `sk_protocol` als PID davor; mit
  `pidincl` muss das Programm den PID **selbst** als erstes Byte
  mitgeben — so kann ein Socket Frames mit **verschiedenen** PIDs
  senden.
* **Empfangen:** `ax25_recvmsg()` schneidet den PID ab, wenn `pidincl`
  nicht gesetzt ist; mit `pidincl` bleibt er als erstes Byte stehen.
* **`AX25_PIDINCL`** = 8 im SOL_AX25-Optionsraum:
  `setsockopt(fd, SOL_AX25, AX25_PIDINCL, &on, sizeof on)`.
  Es ist **kein Filter**, sondern genau das „PID mitgeben/mitliefern".
  Im Empfangspfad greift es nur **verbindungsorientiert**
  (`ax25_rx_iframe`) und in `ax25_recvmsg`; bei **UI-Datagrammen** wird
  `ctl+pid` in `ax25_rcv` bedingungslos abgeschnitten.
* **Es gibt keinen PID-Empfangsfilter.**  Auswahl ist Programmsache —
  bei `AF_PACKET` ohnehin, weil der PID im Rahmen steht.

---

## 5. Empfang: wer bekommt was (`ax25_rcv`, `ax25_in.c`)

* **UI, PID = `AX25_P_TEXT`** → Datagramm-Sockets, gesucht über
  `ax25_get_socket(&dest, &src, SOCK_DGRAM)`.  Das vergleicht
  `source_addr == dest` **und** `dest_addr == src` und **nicht**
  `sk_protocol`.  Ein Datagramm-Socket empfängt also nur Frames, die an
  sein gebundenes Rufzeichen gehen und deren Absender sein zuletzt
  gesetztes Gegenüber ist — faktisch „ein Gegenüber pro Socket".
* **Broadcast** (Ziel = `dev->broadcast`, Vorgabe `QST-0`) wird auch
  ohne „mine" angenommen, aber ebenfalls über
  `ax25_get_socket(&dest, &src)` zugestellt.  „Alle Broadcasts für eine
  PID" ist damit keine Socket-Fähigkeit.
* **`AX25_P_IP` / `AX25_P_ARP`** → `netif_rx()`, also an den IP-/ARP-
  Stack (so trägt der Kernel IP über AX.25 selbst).
* **NET/ROM / ROSE** → `ax25_protocol_function(pid)`.
* **alles Übrige** → `ax25_send_to_raw(&dest, skb, pid)`: Zustellung an
  `AF_AX25`-`SOCK_RAW`-Sockets, die auf Zielruf **und** `sk_protocol`
  passen (braucht `CAP_NET_RAW`).  Das ist die einzige Stelle, an der
  ein PID den Empfänger auswählt — und sie adressiert per Rufzeichen,
  nicht per Interface.
* **I-Frames** → LAPB (`ax25_find_cb` / `ax25_find_listener`), also
  `SOCK_SEQPACKET`.
* **Kein Interface im `recvfrom()`**: man bekommt `sockaddr_ax25`
  (Quellruf + Digis).  Ein `IP_PKTINFO`-Äquivalent gibt es nicht.

---

## 6. `AF_PACKET`: roh, pro Interface

* **Empfangen:** `socket(PF_PACKET, SOCK_RAW, htons(ETH_P_AX25))`.
  `recvfrom()` liefert `sll_ifindex` (bzw. bei altem `SOCK_PACKET` den
  Gerätenamen in `sa_data`).  `ETH_P_ALL` liefert zusätzlich das eigene
  Senden (`PACKET_OUTGOING`) — das ist `listen -a`.
* **Senden:** `sendto(fd, frame, len, 0, &sa, sizeof sa)` mit
  Gerätename/`sll_ifindex` — **ein Socket kann pro Paket ein anderes
  Interface nehmen**.  Vorbild: `ax25-tools.git/kiss/net2kiss.c`,
  `kiss_packet()`.
* **Den Rahmen baut man selbst**: Adressen + Ctl + PID + Nutzlast,
  ohne FCS.  Kein `ax25_cb`, keine LAPB-Zustandsmaschine, kein
  Rufzeichen-Lookup; der Weg geht direkt über `dev_queue_xmit`.
* **Jede PID**: der Rahmen kommt vollständig an, der PID liegt im Byte.
  Vorfilterung ist Programmsache — und genau deshalb sinnvoll im Server
  statt in jedem Client: ungefiltert zahlen alle die Parse-Last.

---

## 7. Anforderung → Socket

| Ich will … | Socket |
|---|---|
| UI an einen Zielruf senden, Quelle aus `axports` | `AF_AX25`+`SOCK_DGRAM` |
| … und den Port namentlich festnageln | dazu `SO_BINDTODEVICE` (`SOL_AX25`) |
| eine Verbindung (I-Frames) | `AF_AX25`+`SOCK_SEQPACKET` |
| fremdes Rufzeichen auf meinen Port binden | `SO_BINDTODEVICE` (`SOL_AX25`) vor `bind` |
| alles auf einem Interface hören, jede PID | `AF_PACKET` RX |
| selbstgebaute Rahmen auf einem Interface senden | `AF_PACKET` TX |
| das Interface **je Paket** angeben/erhalten | `AF_PACKET` |
| den PID sehen / wählen | `AF_PACKET` (roh) oder `AX25_PIDINCL` |

---

## 8. Was das für WAMPES heißt

* **`monitor`** (`sockets/monitor`) ist bereits die **Empfangsseite**
  von `AF_PACKET` — bis hin zu `ETH_P_ALL` = `listen -a`.
* **§11.8 (roh senden)** ist folgerichtig die **Sendeseite desselben
  Socket-Typs**: Interface je Paket, Rahmen selbst gebaut, jede PID.
  Es ist **nicht** über `datagram`/`AF_AX25` nachbildbar, ohne die
  Kernel-Bedeutung zu verbiegen.
* **`datagram`** bleibt, was es im Kernel ist: rufzeichen-orientiertes
  UI-Senden/Empfangen, ohne Interface und ohne PID-Filter.
* **Parität-Lücken** in unserem Shim: `SO_BINDTODEVICE` (`SOL_AX25`)
  und `AX25_PIDINCL`.  Das WAMPES-Pendant zu Ersterem ist die
  Portwahl per Namen (`wampes:<iface>`).
* **AGWPE mitdenken.**  Jede Fähigkeit, die wir der WAMPES-Seite geben,
  einmal daraufhin ansehen, ob sie auch einem AGWPE-Anwender nützt.
  AGWPE kennt Ports, aber kein Interface je Paket: dort liegt der
  Gewinn bei selbstgebauten AX.25-Rahmen, der Sicht auf PIDs und der
  Transparenz — nicht im Nachbau einer syscall-semantischen Kopie.
