#!/usr/bin/env bash
# Proves the fake endpoint end to end with the AWS-patched Linux client
# (openvpn-aws in a second container on the burrow-fake-net network).
# Leaves the endpoint running in the default saml/split mode.
set -uo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
CTL="$BURROW_ROOT/tools/fake-endpoint.sh"
failures=0

client() {
    echo "=== client-test $*"
    docker run --rm --name burrow-fake-client --network burrow-fake-net \
        --cap-add NET_ADMIN --device /dev/net/tun -v burrow-fake-endpoint-pki:/pki:ro \
        burrow-fake-endpoint client-test "$@" || failures=$((failures + 1))
}

bash "$CTL" start saml split || exit 1
client saml
client saml --proto tcp
client saml-bad
bash "$CTL" start mtls split || exit 1
client mtls
bash "$CTL" start saml full || exit 1
client saml --full
bash "$CTL" start saml split || exit 1

echo "=== endpoint log excerpts"
docker logs burrow-fake-endpoint 2>&1 | grep -E 'auth:|idp:' | tail -5
if (( failures )); then
    echo "$failures client test(s) failed"
    exit 1
fi
echo 'All fake endpoint tests passed.'
