# Source of burrow-openvpn

`burrow-openvpn` is OpenVPN 2.6.13 (GNU GPL v2), built by `build.sh` from the
upstream release plus the patches in `patches/`, applied in order:

| Patch | From | Purpose |
| --- | --- | --- |
| `0001-haikuports-openvpn-2.6.13.patchset` | HaikuPorts `net-vpn/openvpn` | Haiku platform, tunnel and routing support |
| `0002-aws-client-vpn.patch` | samm-git/aws-vpn-client, based on AWS's published OpenVPN changes | Larger password and management buffers and 32-bit string lengths, needed for AWS single sign-on |
| `0003-burrow-parenthesise-aws-sizes.patch` | Burrow | Fixes the AWS patch's unparenthesised size macros (`OPTION_PARM_SIZE + 16` became `1 << 33`) |
| `0004-burrow-haiku-tunnel-routes.patch` | Burrow | Creates and removes tunnel units, routes VPN traffic through the tunnel with route ioctls, point-to-point peers and IPv6 |

`server-emulation.patch` is only used by `tools/fake-endpoint` (a test server) and
is not part of `burrow-openvpn`.

Upstream source: https://github.com/OpenVPN/openvpn/archive/refs/tags/v2.6.13.tar.gz
(SHA-256 `730ad13ab0d8efda24b7e8e0df660e4c358ad89105aca47f2bf26da44d8ab020`).

The AWS patch changes how the client encodes credentials, so `burrow-openvpn`
talks to AWS Client VPN endpoints (and other servers that accept AWS clients);
use a stock OpenVPN for ordinary servers that reject it.
