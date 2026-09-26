# Burrow architecture

## Pieces

| Part | Where | Notes |
| --- | --- | --- |
| `Burrow` app | `src/ui` | BApplication, main window, dialogs, log window, notifications, Deskbar tray icon (`DeskbarView`, loaded by Deskbar from the Burrow executable, which is linked with `--export-dynamic`). |
| Core | `src/core` | Plain C++/POSIX with no Haiku API, so it builds and is tested on Linux too: profile parsing (`Profile`), OpenVPN management protocol and AWS sign-in (`Protocol`), one running VPN (`Connection`), the SAML receiver (`SamlListener`), `resolv.conf` editing (`ResolvConf`), profile storage (`ProfileStore`). |
| `burrow-openvpn` | `openvpn/` | OpenVPN 2.6.13 + HaikuPorts patches + AWS patch + Burrow's fixes; see `openvpn/SOURCE.md`. |
| Test endpoint | `tools/fake-endpoint` | Docker stand-in for AWS Client VPN: server-side patched OpenVPN, SAML challenge, fake IdP, emulated VPC with DNS. |

Profiles are private copies in `~/config/settings/Burrow/profiles/<id>.ovpn` (mode 600,
since they often embed keys) with the display name in `<id>.name`. The window frame is in
`~/config/settings/Burrow/window` and app settings in `.../settings`.

## A connection

`Connection` runs on its own thread:

1. Resolves the profile's first `remote` once. With `remote-random-hostname` it
   prefixes 12 random hex digits, as AWS's client does. Both sign-in attempts must
   reach the same endpoint address.
2. Writes a runtime copy of the profile to `/boot/system/cache/tmp/Burrow/<id>/`. The
   copy drops directives Burrow handles itself (`remote`, `auth-federate`, `auth-retry`,
   ...), anything that runs scripts, writes files or changes privileges, and Windows-only
   directives (`block-outside-dns`).
3. Listens on an ephemeral loopback port and starts `burrow-openvpn --management
   127.0.0.1 <port> --management-client --management-hold --management-query-passwords
   --auth-retry interact`. OpenVPN connects back to Burrow, so no other program can take
   over the management interface, and OpenVPN exits if Burrow goes away.
4. Drives the management interface: `state on`, `bytecount 2`, `log on all`, `hold
   release`, then it answers `>PASSWORD:` queries and follows `>STATE:`. It collects the
   pushed options (DNS, domains, routes, redirect-gateway) from the `PUSH_REPLY` log line.
5. Stops with `signal SIGTERM` and kills OpenVPN after 8 s if needed. The runtime
   profile is deleted and the last log is kept as `<id>.log`.

The application object applies DNS for one connected profile at a time, restores it
when that profile disconnects (and at start-up after a crash), and routes the SAML
response to the connection waiting for it.

## AWS single sign-on

1. OpenVPN asks for credentials. Burrow answers user `N/A`, password `ACS::35001`.
2. The endpoint refuses with a dynamic challenge:
   `>PASSWORD:Verification Failed: 'Auth' ['CRV1:R:<state id>:<base64 "N/A">:<IdP URL>']`.
3. Burrow starts the listener on `127.0.0.1:35001` and opens the URL with the preferred
   browser (`BUrl::OpenWithPreferredApplication`, the URL passed unmodified).
4. The identity provider's page POSTs `SAMLResponse` (and `RelayState`) to
   `http://127.0.0.1:35001/`. Burrow answers with a "signed in" page and stops listening.
5. OpenVPN restarts after the refusal (`auth-retry interact`), holds, and asks again.
   Burrow answers `N/A` and `CRV1::<state id>::<QueryEscape(SAMLResponse)>` (Go's
   `url.QueryEscape`, as AWS's client produces). That is ~12 KB, which is why the AWS
   OpenVPN patch enlarges the password and management buffers.

The AWS patch also makes the client write 32-bit string lengths and put the key-method-2
message length in its first four bytes. AWS's servers expect that, but stock OpenVPN
servers do not, so `burrow-openvpn` is for AWS endpoints. The test endpoint uses the
mirror patch (`openvpn/patches/server-emulation.patch`).

## Haiku platform notes

- **TUN:** `src/add-ons/kernel/network/devices/tunnel` (in every image) publishes
  `/dev/tun/<n>` once an interface `tun/<n>` exists, for up to 10 units. Burrow's OpenVPN
  patch creates the interface with `SIOCAIFADDR` and removes it with `SIOCDIFADDR` on
  exit, so no manual `ifconfig tun/0 up` is needed.
- **Deferred unit removal:** after the interface is removed, the stack keeps the device
  (and `/dev/tun/<n>`) until sockets that used its address are gone. TCP TIME_WAIT keeps
  it for about two minutes. OpenVPN therefore skips units whose interface no longer
  exists; otherwise a quick reconnect failed with "Could not add interface".
- **Routes:** Haiku names the interface of every route. The HaikuPorts patch put every
  route on the default gateway's interface, so pushed routes left through Ethernet.
  Burrow's patch puts VPN routes on the tunnel and programs them with `SIOCADDRT`.
- **`route(8)` bug (fixed in the fork, branch `route-zero-network`):** since upstream
  commit 796bd00681, `route add … 0.0.0.0 netmask 128.0.0.0` adds a *default* route and
  drops the mask. With OpenVPN's `redirect-gateway def1` everything in
  0.0.0.0–127.255.255.255 then bypassed the tunnel. Burrow's ioctls avoid the tool. The
  fork fix helps HaikuPorts' OpenVPN and anything else that calls `route`.
- **DNS:** net_server rewrites `resolv.conf` on DHCP renewals and keeps only what stands
  above a `# Dynamic DNS entries` line. Burrow therefore puts its block above that marker,
  adding the marker if needed, and removes both on disconnect.
- **Resolver caching (not yet fixed):** libnetwork's resolver
  (`src/system/libnetwork/netresolv/resolv/res_state.c`) pools initialised resolver
  states. `res_check()`, which reloads `resolv.conf` when it changes, is compiled out for
  Haiku. So a program that has already resolved a name keeps its name servers until it
  restarts. A fork fix: in `__res_get_state()`, stat `_PATH_RESCONF` (at most once a
  second) and re-initialise pooled states whose `res_conf_time` is older.
- **Privileges:** Haiku runs everything as root, so Burrow needs no helper service.
  (AWS's clients install one on other systems.)

## Testing

- `make check` (Haiku) / `make BUILD=build-host check-host` (Linux) runs 160 checks. They
  include full connections against `tests/fake-openvpn.py`, which replays the
  management exchange the real AWS-patched client produced against the test endpoint.
- `bash tools/fake-endpoint.sh test` checks the endpoint itself with a Linux
  AWS-patched client.
- Manual end-to-end tests in the VM (see `docs/VM.md`): import, SAML sign-in in
  WebPositive, split and full tunnel (traffic counted on `tun/<n>`), certificate auth,
  DNS by name and search domain, immediate reconnects, Deskbar menu, window hiding.

## Next steps

1. Connect to a real AWS Client VPN endpoint (SAML with the real IdP in WebPositive,
   `remote-random-hostname`, AWS's pushed options).
2. Fix resolver reloading in the fork (see above) and try IPv6 routes.
3. Store the last-used profile, auto-reconnect, and optionally keep user names.
