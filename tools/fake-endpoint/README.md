# Fake AWS Client VPN endpoint

A Docker stand-in for an AWS Client VPN endpoint, so Burrow can be developed without AWS.

```sh
bash tools/fake-endpoint.sh build                    # image burrow-fake-endpoint
bash tools/fake-endpoint.sh start [saml|mtls] [split|full]
bash tools/fake-endpoint.sh profile [saml|mtls] [udp|tcp] > test.ovpn
bash tools/fake-endpoint.sh status | logs [-f] | stop
bash tools/fake-endpoint.sh test                     # Linux AWS-patched client, all modes
```

| Item | Value |
| --- | --- |
| OpenVPN UDP / TCP | `127.0.0.1:11194` / `127.0.0.1:11443` (guest: `10.0.2.2`) |
| Fake IdP | `http://127.0.0.1:18080`, sign in with `demo` / `demo` |
| Client subnets | `10.99.0.0/24` (UDP), `10.99.1.0/24` (TCP) |
| Emulated VPC | `http://10.100.0.1/` says `Hello from the Burrow test VPC`; DNS `10.100.0.2` resolves `vpc.burrow.test` |
| Pushed | `route 10.100.0.0 255.255.0.0`, `dhcp-option DNS 10.100.0.2`, `dhcp-option DOMAIN burrow.test`, plus `redirect-gateway def1` in `full` mode |
| PKI | Docker volume `burrow-fake-endpoint-pki` (never in the repository) |

`BURROW_FAKE_IDP_HOST` (default `10.0.2.2:18080`) is the host put into the challenge URL, and
`BURROW_FAKE_REMOTE` (default `10.0.2.2`) the `remote` written into profiles.

## SAML flow (as AWS does it)

1. The client connects with username `N/A`, password `ACS::35001`.
2. The server denies with `AUTH_FAILED,CRV1:R:<sid>:Ti9B:http://10.0.2.2:18080/saml/login?sid=<sid>`
   (`Ti9B` is base64 of `N/A`). Over the management interface this arrives as
   `>PASSWORD:Verification Failed: 'Auth' ['CRV1:R:...']`.
3. The browser signs in; the IdP answers with a form that auto-POSTs `SAMLResponse`
   (~11.8 KB base64) and an empty `RelayState` to `http://127.0.0.1:35001/`.
4. The client reconnects to the same server with password
   `CRV1::<sid>::<QueryEscape(SAMLResponse)>`; the server URL-decodes it and accepts it only
   if it matches what the IdP issued for that sid (sessions live 10 minutes).

## OpenVPN builds in the image

- `openvpn-server-emu`: 2.6.13 + `openvpn/patches/server-emulation.patch` (test only).
- `openvpn-aws`: 2.6.13 + `0002-aws-client-vpn.patch` + `0003-burrow-parenthesise-aws-sizes.patch`.

## Gotchas

- The AWS client writes u32 string lengths but still reads u16 ones, and puts the total
  key-method-2 length in the leading uint32. A SAML password spans several 16 KB TLS
  records, so the server has to keep reading until that length has arrived; stock
  OpenVPN parses the first record and answers "Username or password is too long".
- `0002` enlarges `ERR_BUF_SIZE` only in the non-management branch of `error.h`; with
  management enabled it stays 10240, which truncated `>CLIENT:ENV,password=` on the server.
  The server patch enlarges both branches. The client never logs the password, so it
  does not need this.
- On this host `net.ipv4.ip_forward` is 0, so bridged containers have no outbound
  network: the image builds with `--network host`, dnsmasq cannot forward outside names,
  and `full` mode's NAT to the internet goes nowhere (routes and the VPC still work).
