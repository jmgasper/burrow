# Burrow's stand-in AWS Client VPN endpoint (and AWS-patched Linux test client).
# Build with tools/fake-endpoint.sh build; the `patches` build context is
# openvpn/patches in the Burrow repository.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential autoconf automake libtool pkg-config ca-certificates curl \
        libssl-dev liblzo2-dev liblz4-dev libcap-ng-dev python3-docutils patch \
    && rm -rf /var/lib/apt/lists/*
ARG OPENVPN_VERSION=2.6.13
ARG OPENVPN_SHA256=730ad13ab0d8efda24b7e8e0df660e4c358ad89105aca47f2bf26da44d8ab020
WORKDIR /src
RUN curl -fsSL -o openvpn.tar.gz \
        https://github.com/OpenVPN/openvpn/archive/refs/tags/v${OPENVPN_VERSION}.tar.gz \
    && echo "${OPENVPN_SHA256}  openvpn.tar.gz" | sha256sum -c - \
    && mkdir server client \
    && tar xzf openvpn.tar.gz -C server --strip-components=1 \
    && tar xzf openvpn.tar.gz -C client --strip-components=1
COPY --from=patches server-emulation.patch 0002-aws-client-vpn.patch \
    0003-burrow-parenthesise-aws-sizes.patch /patches/
# Server: AWS buffer sizes + u32 string reads. Client: the AWS client patch
# with Burrow's parenthesised sizes, as shipped on Haiku.
RUN cd server && patch -p1 < /patches/server-emulation.patch \
    && cd ../client && patch -p1 < /patches/0002-aws-client-vpn.patch \
    && patch -p1 < /patches/0003-burrow-parenthesise-aws-sizes.patch
RUN for tree in server client; do \
        (cd $tree && autoreconf -fi >/dev/null \
         && ./configure --disable-dco --disable-plugin-auth-pam --disable-plugin-down-root \
                --disable-pkcs11 --disable-systemd >/dev/null \
         && make -j"$(nproc)" >/dev/null) || exit 1; \
    done \
    && install -m 755 server/src/openvpn/openvpn /usr/local/sbin/openvpn-server-emu \
    && install -m 755 client/src/openvpn/openvpn /usr/local/sbin/openvpn-aws

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 liblzo2-2 liblz4-1 libcap-ng0 openssl ca-certificates \
        iproute2 iptables dnsmasq python3 curl bind9-dnsutils procps \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/sbin/openvpn-server-emu /usr/local/sbin/openvpn-aws /usr/local/sbin/
COPY endpoint.py client_test.py /opt/burrow/
VOLUME /pki
EXPOSE 1194/udp 443/tcp 18080/tcp
ENTRYPOINT ["python3", "-u", "/opt/burrow/endpoint.py"]
CMD ["serve"]
