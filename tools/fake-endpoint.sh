#!/usr/bin/env bash
# Burrow's stand-in AWS Client VPN endpoint (Docker). See tools/fake-endpoint/README.md.
#   build | start [saml|mtls] [split|full] | stop | status | logs [-f] |
#   profile [saml|mtls] [udp|tcp] | test
set -euo pipefail
BURROW_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=burrow-fake-endpoint
NAME=burrow-fake-endpoint
NETWORK=burrow-fake-net
VOLUME=burrow-fake-endpoint-pki
IDP_HOST=${BURROW_FAKE_IDP_HOST:-10.0.2.2:18080}
REMOTE=${BURROW_FAKE_REMOTE:-10.0.2.2}

wait_ready() {
    for _ in $(seq 1 60); do
        if [[ $(docker logs "$NAME" 2>&1 | grep -c 'Initialization Sequence Completed') -ge 2 ]] &&
            curl -fsS --max-time 2 http://127.0.0.1:18080/health >/dev/null 2>&1; then
            return 0
        fi
        if [[ $(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null) != true ]]; then
            docker logs --tail 30 "$NAME" >&2
            echo 'The fake endpoint stopped during startup.' >&2
            return 1
        fi
        sleep 0.5
    done
    echo 'Timed out waiting for the fake endpoint.' >&2
    return 1
}

case ${1:-} in
build)
    # The host has net.ipv4.ip_forward=0, so bridged build containers have no
    # network; RUN steps use the host network instead.
    docker build --network host --build-context patches="$BURROW_ROOT/openvpn/patches" \
        -t "$IMAGE" -f "$BURROW_ROOT/tools/fake-endpoint/Dockerfile" \
        "$BURROW_ROOT/tools/fake-endpoint"
    ;;
start)
    mode=${2:-saml} tunnel=${3:-split}
    [[ $mode == saml || $mode == mtls ]] || { echo "mode must be saml or mtls" >&2; exit 2; }
    [[ $tunnel == split || $tunnel == full ]] || { echo "tunnel must be split or full" >&2; exit 2; }
    docker network inspect "$NETWORK" >/dev/null 2>&1 || docker network create "$NETWORK" >/dev/null
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    docker run -d --name "$NAME" --network "$NETWORK" \
        --cap-add NET_ADMIN --device /dev/net/tun --sysctl net.ipv4.ip_forward=1 \
        -p 127.0.0.1:11194:1194/udp -p 127.0.0.1:11443:443/tcp -p 127.0.0.1:18080:18080/tcp \
        -v "$VOLUME":/pki -e MODE="$mode" -e TUNNEL="$tunnel" -e IDP_HOST="$IDP_HOST" \
        "$IMAGE" >/dev/null
    wait_ready
    echo "Fake endpoint up: mode=$mode tunnel=$tunnel, UDP 127.0.0.1:11194, TCP 127.0.0.1:11443," \
        "IdP http://127.0.0.1:18080 (challenge URLs use $IDP_HOST)"
    ;;
stop)
    docker rm -f "$NAME" >/dev/null 2>&1 && echo 'Fake endpoint stopped.' || echo 'Not running.'
    ;;
status)
    if [[ $(docker inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null) == true ]]; then
        docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$NAME" |
            grep -E '^(MODE|TUNNEL|IDP_HOST)=' | paste -sd' '
        curl -fsS --max-time 2 http://127.0.0.1:18080/health >/dev/null && echo 'IdP: ok' || echo 'IdP: not answering'
    else
        echo 'Not running.'
        exit 1
    fi
    ;;
logs)
    shift
    docker logs --tail 200 "$@" "$NAME"
    ;;
profile)
    docker run --rm --network none -v "$VOLUME":/pki "$IMAGE" profile "${2:-saml}" "${3:-udp}" "$REMOTE"
    ;;
test)
    exec bash "$BURROW_ROOT/tools/fake-endpoint/test-linux-client.sh"
    ;;
*)
    sed -n '2,4p' "$0" | sed 's/^# \{0,3\}//'
    exit 2
    ;;
esac
