# How filecast compares to other file-transfer tools

Every tool below is good at what it was built for, and several of them do
things filecast does not (encryption, internet transfer, mobile apps). This
table exists to answer one question: **what should I use to get one file onto
many LAN machines with the least setup?**

Facts were checked against each project's own documentation and source in
July 2026 — versions and features move, so if you spot an error please
[open an issue](https://github.com/gistrec/filecast/issues).

|  | filecast | [udpcast](https://www.udpcast.linux.lu/) | [UFTP](https://uftp-multicast.sourceforge.net/) | [croc](https://github.com/schollz/croc) | [LocalSend](https://localsend.org/) | [magic-wormhole](https://github.com/magic-wormhole/magic-wormhole) |
| --- | --- | --- | --- | --- | --- | --- |
| One file → N machines in a single transfer | ✅ broadcast or multicast | ✅ multicast or broadcast | ✅ multicast | ❌ point-to-point [^p2p] | ❌ point-to-point [^p2p] [^localsend-link] | ❌ point-to-point [^p2p] [^wormhole-rs] |
| Works fully offline (no internet, no relay) | ✅ | ✅ | ✅ | ⚠️ relay rendezvous by default; LAN-only via `--local` [^croc-relay] | ✅ | ⚠️ rendezvous server by default [^wormhole-mailbox] |
| Windows / macOS / Linux | ✅ prebuilt binaries for all three | ⚠️ Linux-first [^udpcast-platforms] | ✅ | ✅ (+ BSD, Android via Termux) | ✅ (+ iOS, Android) | ✅ anywhere Python 3.10+ runs |
| Zero receiver setup (one binary, one command) | ✅ | ✅ | ❌ per-receiver daemon [^uftp-daemon] | ✅ | ⚠️ GUI app install | ⚠️ pip or distro package |
| Resume an interrupted transfer | ✅ `--resume` | ❌ [^udpcast-streaming] | ⚠️ session restart [^uftp-restart] | ✅ | ❌ open feature request since 2023 | ❌ open feature request since 2016 |
| End-to-end file integrity check | ✅ SHA-256 [^filecast-hash] | ❌ [^udpcast-integrity] | ⚠️ [^uftp-integrity] | ✅ [^croc-hash] | ✅ (TLS transport) | ✅ (encrypted transport) |
| Encryption | ❌ [not yet](https://github.com/gistrec/filecast/issues/22) | ❌ [^udpcast-pipe] | ✅ default since v5.0 [^uftp-crypto] | ✅ end-to-end (PAKE) | ✅ HTTPS by default [^localsend-tls] | ✅ end-to-end (PAKE) |
| Interface | CLI | CLI | CLI (daemon) | CLI | GUI [^localsend-cli] | CLI |
| License | MIT | GPL-2.0 | GPL-3.0 [^uftp-license] | MIT | Apache-2.0 | MIT |
| Latest release when checked (2026-07) | v1.0.0 (2026-07) | 2025-02 | 2023-12 | 2026-07 | 2025-02 [^localsend-activity] | 2026-05 |

## When you should use something else

- **croc** — the best tool for moving a file between *two* machines across the
  internet: end-to-end encrypted, resumable, works through NATs.
- **magic-wormhole** — secure ad-hoc transfer between two machines with a
  short human-pronounceable code; the design many later tools built upon.
- **LocalSend** — sharing between phones and laptops with a polished GUI;
  AirDrop-style, cross-vendor, fully offline.
- **UFTP** — encrypted multicast at serious scale (thousands of receivers,
  satellite/WAN links, proxies across subnets) when you can invest in daemon
  setup on every receiver (plus key distribution for encrypted mode).
- **udpcast** — disk imaging: its bootable CD/PXE images clone entire disks
  over multicast to machines with no OS installed, which filecast cannot do.

filecast's niche is the everyday version of the one-to-many problem: one
self-contained binary on each machine, one command on each side, and a file
lands on every receiver on the LAN in the time of a single transfer — with
packet-loss recovery, SHA-256 verification, and resume.

[^p2p]: One transfer reaches one receiver; distributing to N machines means N
    separate transfers (croc: one code phrase per receiver; LocalSend: one
    send session per device; magic-wormhole: one code per receiver).
[^localsend-link]: LocalSend's "share via link" lets several devices download
    from the sender, but each download is an independent HTTP connection —
    the sender uploads the bytes once per receiver.
[^wormhole-rs]: The magic-wormhole organization's separate Rust CLI has an
    experimental `send-many` mode that hands one file to multiple receivers
    over a shared code; each transfer is still an individual connection.
[^croc-relay]: By default croc connects through the author's public relay
    (payload end-to-end encrypted; connection metadata transits the relay).
    croc also auto-discovers peers on the local network by default, so LAN
    transfers often bypass the relay; `--local` forces LAN-only, and
    self-hosting a relay is one command.
[^wormhole-mailbox]: Key exchange goes through the project's public mailbox
    server even when both machines share a LAN (data then flows directly).
    Fully offline use requires self-hosting a mailbox server.
[^filecast-hash]: The SHA-256 is announced over the same unauthenticated
    channel as the data, so it guards against loss and corruption, not
    against a malicious LAN peer — see the Limitations section of the README.
[^uftp-integrity]: No whole-file hash ([protocol
    spec](https://uftp-multicast.sourceforge.net/protocol.txt)): integrity
    relies on NAK-based retransmission, and in encrypted mode (the default
    since v5.0) every packet is authenticated by the AEAD cipher.
[^udpcast-platforms]: Official Windows binaries exist but were last built in
    December 2021 (v20211207); on macOS it builds from source (portability
    fixes landed in the 2025 release) with no official package.
[^uftp-daemon]: Every receiver runs the pre-configured `uftpd` daemon;
    encrypted mode additionally involves key generation and fingerprint
    distribution.
[^udpcast-streaming]: udpcast's `--streaming` lets a receiver join an ongoing
    transmission mid-way, but an interrupted receiver cannot recover the part
    it missed.
[^uftp-restart]: The server can write a restart file for failed sessions and
    resume from it (`-F`); the docs do not specify sub-file granularity.
[^udpcast-integrity]: No application-level file hash; udpcast relies on its
    mature per-slice retransmission protocol (battle-tested for disk imaging)
    plus UDP checksums.
[^croc-hash]: croc's transport is authenticated encryption (tampering is
    detected); the file-comparison hash used for resume is non-cryptographic
    xxhash by default.
[^udpcast-pipe]: No built-in encryption; the payload can be piped through an
    external cipher via `--pipe`, though control traffic stays in the clear.
[^uftp-crypto]: Modern crypto (ECDH key exchange, AES-GCM) and
    encryption-on-by-default arrived in v5.0 (2020). Distro packages vary:
    Debian stable still ships 4.10.2, where encryption is off by default.
[^localsend-tls]: HTTPS with self-signed, trust-on-first-use certificates
    (SHA-256 fingerprint identity, optional PIN). Browser "share via link"
    downloads use plain HTTP because browsers reject self-signed
    certificates.
[^localsend-cli]: CLI code exists in the LocalSend monorepo but has not
    shipped in any release as of v1.17.0.
[^uftp-license]: GPL-3.0 with an OpenSSL linking exception; commercial
    licenses available from the author.
[^localsend-activity]: Latest stable release v1.17.0 (2025-02); the
    repository remained active into 2026.
