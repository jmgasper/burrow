#!/usr/bin/env python3
"""Stands in for burrow-openvpn in the core tests: connects back to the
management port like `--management-client` and replays the exchange the real
AWS-patched client produced against tools/fake-endpoint (see its README).

BURROW_FAKE_SCENARIO: saml (default), cert, userpass.
BURROW_FAKE_SAML: the SAMLResponse the test's browser will post.
"""
import os
import socket
import sys
import time
import urllib.parse

args = sys.argv[1:]


def arg(name, count=1):
    if name not in args:
        return None
    i = args.index(name)
    return args[i + 1:i + 1 + count]


scenario = os.environ.get('BURROW_FAKE_SCENARIO', 'saml')
host, port = arg('--management', 2)
config = open(arg('--config')[0]).read()
remote = arg('--remote', 3)
now = int(time.time())

sock = socket.create_connection((host, int(port)))
stream = sock.makefile('rwb', buffering=0)


def send(line):
    stream.write((line + '\r\n').encode())


def expect(prefix):
    while True:
        line = stream.readline().decode().rstrip('\r\n')
        if not line:
            sys.exit('management closed while waiting for ' + prefix)
        if line.startswith(prefix):
            send('SUCCESS: ' + line.split(' ')[0])
            return line
        send('SUCCESS: ' + line)


def value(line):
    # username "Auth" "N/A" -> N/A
    quoted = line.split(' ', 2)[2]
    return quoted[1:-1].replace('\\"', '"').replace('\\\\', '\\')


def fail(reason):
    send('>FATAL:' + reason)
    sys.exit(1)


def connected():
    send('>LOG:%d,I,TUN/TAP device /dev/tun/0 opened (created)' % now)
    send(">LOG:%d,,PUSH: Received control message: 'PUSH_REPLY,route 10.100.0.0 255.255.0.0,"
         "dhcp-option DNS 10.100.0.2,dhcp-option DOMAIN burrow.test,route-gateway 10.99.0.1,"
         "topology subnet,ping 10,ifconfig 10.99.0.2 255.255.255.0,peer-id 0'" % now)
    send('>STATE:%d,CONNECTED,SUCCESS,10.99.0.2,%s,%s,,' % (now, remote[0], remote[1]))
    send('>BYTECOUNT:4096,2048')


for line in config.splitlines():
    words = line.split()
    if words and words[0] in ('auth-federate', 'remote', 'script-security', 'up'):
        fail('runtime config still has ' + words[0])

send(">INFO:OpenVPN Management Interface Version 5 -- type 'help' for more info")
send('>HOLD:Waiting for hold release:0')
expect('hold release')
send('>STATE:%d,WAIT,,,,,,' % now)

if scenario == 'saml':
    if '--auth-user-pass' not in args:
        fail('--auth-user-pass missing')
    send(">PASSWORD:Need 'Auth' username/password")
    user = value(expect('username'))
    password = value(expect('password'))
    if (user, password) != ('N/A', 'ACS::35001'):
        fail('unexpected first credentials %r %r' % (user, password))
    send(">PASSWORD:Verification Failed: 'Auth' ['CRV1:R:fake/0123:Ti9B:"
         "http://127.0.0.1:18080/saml/login?sid=fake%2F0123']")
    send('>STATE:%d,RECONNECTING,auth-failure,,,,,' % now)
    send('>HOLD:Waiting for hold release:5')
    expect('hold release')
    send(">PASSWORD:Need 'Auth' username/password")
    user = value(expect('username'))
    password = value(expect('password'))
    wanted = 'CRV1::fake/0123::' + urllib.parse.quote_plus(os.environ['BURROW_FAKE_SAML'])
    if password != wanted:
        send(">PASSWORD:Verification Failed: 'Auth'")
        fail('SAML password mismatch (%d vs %d characters)' % (len(password), len(wanted)))
    connected()
elif scenario == 'userpass':
    send(">PASSWORD:Need 'Auth' username/password")
    user = value(expect('username'))
    password = value(expect('password'))
    if (user, password) != ('alice', 'secret"\\'):
        send(">PASSWORD:Verification Failed: 'Auth' ['bad credentials']")
        send(">PASSWORD:Need 'Auth' username/password")
        user = value(expect('username'))
        password = value(expect('password'))
        if (user, password) != ('alice', 'secret"\\'):
            fail('credentials %r %r' % (user, password))
    connected()
else:
    connected()

expect('signal SIGTERM')
send('>STATE:%d,EXITING,SIGTERM,,,,,' % now)
sock.close()
