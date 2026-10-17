# WAMPES Sessions in netd – Ergänzung §11.14–§11.15 (Deutsch)

Dieser Anhang enthält die Ergänzungen zu `wampes-sessions-in-netd.md`
(§11.14 Laufzeit-Beleg, §11.15 offene IP-Frage), die vorübergehend
in `libax25.git/doc/` abgelegt werden, bis sie ins Englische übersetzt
und an den richtigen Platz eingearbeitet sind.

## §11.14 Laufzeit-Beleg auf db0fhn (2026-10-17)

Mit dem Fix (§11.14, SOCK_PACKET-Bind als `struct sockaddr` mit
Gerätenamen) wurde das Verhalten auf db0fhn überprüft:

- `listen -a -p bpq0` zeigt `bpq0: fm TE1ST to IDENT ctl UI^ pid=F0(Text) len 4 ... test`
  (kein „invalid port name“).
- `listen -p bpq0` ohne `-a` (RX only) zeigt nichts (korrekt, eigener TX
  kommt nicht als RX zurück).
- `axparms -setcall bpq0 te1st-1` → Port gilt als Userspace-Port,
  `beacon -s bpq0` → „no backend serves the port 'bpq0'“ (Folge der
  Rufzeichen-Identifikation, keine neue Regression).

Die Kernel-Port-Erkennung („Kernel-Port nachweislich gefunden“) bestätigt
sich: bei aktivem Kernel-Interface mit passendem Rufzeichen wird der
Port als Kernel-Port bedient, bei Rufzeichen-Mismatch als Userspace-Port
behandelt (Diagnose präzisiert).

## §11.15 Offene Frage: IP-Nutzlast über den AGWPE-Port (2026-10-17)

**Frage:** Was gilt, wenn ein Anwender über den AGWPE-Port IP nach außen
sprechen bzw. von außen über ihn erreichbar sein will?

Hintergrund:
- Kernel-AX.25 kann IP-Nutzlast tragen (IP-over-AX.25).
- Mit WAMPES wird IP dem `net`-Programm überlassen (klassische
  WAMPES-Trennung: AX.25-Framing/Forwarding vs. Netz-Layer via `net`).
- Über den AGWPE-Port (Shim/Bridge) ist unklar, wie IP-Verkehr behandelt
  wird: Soll IP über AGWPE durchgereicht/geroutet werden, oder bleibt
  IP ausschließlich beim WAMPES-`net`-Programm?

Diese Frage bleibt offen (§11.10 „Noch offen“ verweist darauf).

**Siehe auch:**
- `ax25-non-kernel-stack.sh`, `build-unix-ax25-stack.sh`
- AX25-WITHOUT-KERNEL-SYSOP*.md, AX25-WITHOUT-KERNEL-DEVELOPER*.md
