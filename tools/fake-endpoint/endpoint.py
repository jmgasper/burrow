#!/usr/bin/env python3
"""Burrow's stand-in AWS Client VPN endpoint.

Runs inside the burrow-fake-endpoint container:
  serve                      OpenVPN (UDP 1194 + TCP 443), SAML auth daemon, fake IdP
                             (18080), emulated VPC (10.100.0.1 web, 10.100.0.2 DNS)
  profile saml|mtls [udp|tcp] [remote] [port]
                             print an AWS-style client profile
  client-test ...            run client_test.py (AWS-patched Linux client)

Environment: MODE=saml|mtls, TUNNEL=split|full, IDP_HOST (host:port put into
the CRV1 challenge URL, default 10.0.2.2:18080, the host as seen by a QEMU guest).
"""
import base64
import datetime
import html
import http.server
import os
import pathlib
import secrets
import socket
import subprocess
import sys
import threading
import time
import urllib.parse

PKI = pathlib.Path('/pki')
MODE = os.environ.get('MODE', 'saml')
TUNNEL = os.environ.get('TUNNEL', 'split')
IDP_HOST = os.environ.get('IDP_HOST', '10.0.2.2:18080')
ACS_URL = 'http://127.0.0.1:35001/'
VPC_GREETING = 'Hello from the Burrow test VPC\n'
SESSION_LIFETIME = 600

# One instance per protocol, as AWS offers both; each gets its own client subnet.
INSTANCES = [
    {'name': 'udp', 'proto': 'udp', 'port': 1194, 'net': '10.99.0.0', 'mgmt': 7505},
    {'name': 'tcp', 'proto': 'tcp-server', 'port': 443, 'net': '10.99.1.0', 'mgmt': 7506},
]

sessions = {}  # sid -> {'created': time, 'response': base64 SAMLResponse or None}
sessions_lock = threading.Lock()


def log(*parts):
    print('[endpoint]', *parts, flush=True)


# --- PKI -------------------------------------------------------------------

def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def ensure_pki():
    if (PKI / 'client.crt').exists():
        return
    PKI.mkdir(parents=True, exist_ok=True)
    log('generating test PKI in', PKI)
    os.chdir(PKI)
    run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650',
        '-keyout', 'ca.key', '-out', 'ca.crt', '-subj', '/CN=Burrow test CA',
        '-addext', 'basicConstraints=critical,CA:TRUE',
        '-addext', 'keyUsage=critical,keyCertSign,cRLSign', capture_output=True)
    for name, usage in [('server', 'serverAuth'), ('client', 'clientAuth')]:
        cn = f'{name}.burrow.test'
        run('openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', f'{name}.key', '-out', f'{name}.csr', '-subj', f'/CN={cn}',
            capture_output=True)
        pathlib.Path(f'{name}.ext').write_text(
            'basicConstraints=CA:FALSE\n'
            'keyUsage=critical,digitalSignature,keyEncipherment\n'
            f'extendedKeyUsage={usage}\nsubjectAltName=DNS:{cn}\n')
        run('openssl', 'x509', '-req', '-in', f'{name}.csr', '-CA', 'ca.crt',
            '-CAkey', 'ca.key', '-CAcreateserial', '-days', '3650',
            '-out', f'{name}.crt', '-extfile', f'{name}.ext', capture_output=True)
    for key in PKI.glob('*.key'):
        key.chmod(0o600)


def pem(name):
    text = (PKI / name).read_text()
    if name.endswith('.crt'):
        # Only the PEM block, like the files AWS hands out.
        start = text.index('-----BEGIN CERTIFICATE-----')
        text = text[start:]
    return text.strip() + '\n'


def profile(mode='saml', proto='udp', remote='10.0.2.2', port=None):
    port = port or ('11194' if proto == 'udp' else '11443')
    lines = [
        'client',
        'dev tun',
        f'proto {proto}',
        f'remote {remote} {port}',
        '# remote-random-hostname is omitted: the fake endpoint is addressed by IP.',
        'resolv-retry infinite',
        'nobind',
        'remote-cert-tls server',
        'cipher AES-256-GCM',
        'verb 3',
        '<ca>', pem('ca.crt').rstrip(), '</ca>',
        '',
    ]
    if mode == 'mtls':
        lines += ['<cert>', pem('client.crt').rstrip(), '</cert>',
                  '<key>', pem('client.key').rstrip(), '</key>', '']
    else:
        lines += ['auth-user-pass', 'auth-federate', 'auth-retry interact',
                  'auth-nocache', '']
    lines += ['reneg-sec 0', '']
    return '\n'.join(lines)


# --- Emulated VPC -----------------------------------------------------------

def setup_network():
    try:
        run('ip', 'link', 'add', 'vpc0', 'type', 'dummy', capture_output=True)
        dev, prefix = 'vpc0', '16'
    except subprocess.CalledProcessError:
        log('no dummy interface support, putting the VPC addresses on lo')
        dev, prefix = 'lo', '32'
    for address in ['10.100.0.1', '10.100.0.2']:
        run('ip', 'addr', 'add', f'{address}/{prefix}', 'dev', dev)
    run('ip', 'link', 'set', dev, 'up')
    if TUNNEL == 'full':
        run('iptables', '-t', 'nat', '-A', 'POSTROUTING', '-s', '10.99.0.0/16',
            '!', '-d', '10.99.0.0/16', '-o', 'eth0', '-j', 'MASQUERADE')
        log('full tunnel: NAT from 10.99.0.0/16 out of eth0')


class VPCHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = VPC_GREETING.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Client-Address', self.client_address[0])
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        log('vpc-web', self.client_address[0], fmt % args)


def start_dnsmasq():
    return subprocess.Popen([
        'dnsmasq', '--keep-in-foreground', '--no-hosts', '--bind-interfaces',
        '--listen-address=10.100.0.2', '--address=/vpc.burrow.test/10.100.0.1',
        '--log-queries', '--log-facility=-',
        '--user=root'])


# --- Fake IdP ----------------------------------------------------------------

PAGE_STYLE = '''
body { font: 15px/1.45 "Noto Sans", "DejaVu Sans", sans-serif; background: #eef1f5;
       color: #1d2733; margin: 0; }
main { max-width: 360px; margin: 64px auto; background: #fff; border-radius: 8px;
       padding: 28px 32px; box-shadow: 0 2px 10px rgba(0,0,0,.12); }
h1 { font-size: 20px; margin: 0 0 4px; }
p { color: #52606d; margin: 0 0 18px; }
label { display: block; font-weight: 600; margin: 12px 0 4px; }
input[type=text], input[type=password] { width: 100%; box-sizing: border-box;
       padding: 8px; border: 1px solid #b8c2cc; border-radius: 4px; font-size: 15px; }
button { margin-top: 20px; width: 100%; padding: 10px; border: 0; border-radius: 4px;
       background: #2d6cdf; color: #fff; font-size: 15px; font-weight: 600; }
.note { font-size: 12px; color: #7b8794; margin-top: 16px; }
'''


def page(title, body):
    return (f'<!DOCTYPE html>\n<html><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width, initial-scale=1">'
            f'<title>{html.escape(title)}</title><style>{PAGE_STYLE}</style></head>'
            f'<body><main>{body}</main></body></html>\n')


def saml_response(username, sid):
    """A realistic-size (~12 KB base64) unsigned-but-plausible SAML response."""
    now = datetime.datetime.now(datetime.timezone.utc)
    stamp = now.strftime('%Y-%m-%dT%H:%M:%SZ')
    later = (now + datetime.timedelta(minutes=5)).strftime('%Y-%m-%dT%H:%M:%SZ')
    cert = ''.join(pem('ca.crt').splitlines()[1:-1])
    signature = base64.b64encode(secrets.token_bytes(256)).decode()
    digest = base64.b64encode(secrets.token_bytes(32)).decode()
    groups = ''.join(
        f'<saml:AttributeValue xsi:type="xs:string">vpn-group-{i:03d}-{secrets.token_hex(12)}'
        '</saml:AttributeValue>' for i in range(90))
    rid, aid = '_' + secrets.token_hex(20), '_' + secrets.token_hex(20)
    xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<samlp:Response xmlns:samlp="urn:oasis:names:tc:SAML:2.0:protocol" xmlns:saml="urn:oasis:names:tc:SAML:2.0:assertion" ID="{rid}" Version="2.0" IssueInstant="{stamp}" Destination="{ACS_URL}"><saml:Issuer>http://{IDP_HOST}/saml</saml:Issuer><samlp:Status><samlp:StatusCode Value="urn:oasis:names:tc:SAML:2.0:status:Success"/></samlp:Status><saml:Assertion xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xs="http://www.w3.org/2001/XMLSchema" ID="{aid}" Version="2.0" IssueInstant="{stamp}"><saml:Issuer>http://{IDP_HOST}/saml</saml:Issuer><ds:Signature xmlns:ds="http://www.w3.org/2000/09/xmldsig#"><ds:SignedInfo><ds:CanonicalizationMethod Algorithm="http://www.w3.org/2001/10/xml-exc-c14n#"/><ds:SignatureMethod Algorithm="http://www.w3.org/2001/04/xmldsig-more#rsa-sha256"/><ds:Reference URI="#{aid}"><ds:Transforms><ds:Transform Algorithm="http://www.w3.org/2000/09/xmldsig#enveloped-signature"/><ds:Transform Algorithm="http://www.w3.org/2001/10/xml-exc-c14n#"/></ds:Transforms><ds:DigestMethod Algorithm="http://www.w3.org/2001/04/xmlenc#sha256"/><ds:DigestValue>{digest}</ds:DigestValue></ds:Reference></ds:SignedInfo><ds:SignatureValue>{signature}</ds:SignatureValue><ds:KeyInfo><ds:X509Data><ds:X509Certificate>{cert}</ds:X509Certificate></ds:X509Data></ds:KeyInfo></ds:Signature><saml:Subject><saml:NameID Format="urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress">{html.escape(username)}@burrow.test</saml:NameID><saml:SubjectConfirmation Method="urn:oasis:names:tc:SAML:2.0:cm:bearer"><saml:SubjectConfirmationData NotOnOrAfter="{later}" Recipient="{ACS_URL}"/></saml:SubjectConfirmation></saml:Subject><saml:Conditions NotBefore="{stamp}" NotOnOrAfter="{later}"><saml:AudienceRestriction><saml:Audience>urn:amazon:webservices:clientvpn</saml:Audience></saml:AudienceRestriction></saml:Conditions><saml:AuthnStatement AuthnInstant="{stamp}" SessionIndex="{html.escape(sid)}"><saml:AuthnContext><saml:AuthnContextClassRef>urn:oasis:names:tc:SAML:2.0:ac:classes:PasswordProtectedTransport</saml:AuthnContextClassRef></saml:AuthnContext></saml:AuthnStatement><saml:AttributeStatement><saml:Attribute Name="NameID"><saml:AttributeValue xsi:type="xs:string">{html.escape(username)}@burrow.test</saml:AttributeValue></saml:Attribute><saml:Attribute Name="FirstName"><saml:AttributeValue xsi:type="xs:string">{html.escape(username)}</saml:AttributeValue></saml:Attribute><saml:Attribute Name="memberOf">{groups}</saml:Attribute></saml:AttributeStatement></saml:Assertion></samlp:Response>'''
    return base64.b64encode(xml.encode()).decode()


class IdPHandler(http.server.BaseHTTPRequestHandler):
    def send_page(self, status, text):
        body = text.encode()
        self.send_response(status)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def session(self, sid):
        with sessions_lock:
            entry = sessions.get(sid)
            if entry and time.time() - entry['created'] > SESSION_LIFETIME:
                del sessions[sid]
                entry = None
            return entry

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == '/health':
            return self.send_page(200, 'ok\n')
        if url.path != '/saml/login':
            return self.send_page(200, page('Burrow test IdP', '<h1>Burrow test IdP</h1>'
                '<p>Sign-in pages are reached from the VPN client.</p>'))
        sid = query.get('sid', [''])[0]
        if not self.session(sid):
            return self.send_page(404, page('Burrow test IdP', '<h1>Session expired</h1>'
                '<p>Unknown or expired sign-in session. Connect again from the VPN client.</p>'))
        self.send_page(200, page('Burrow test IdP', f'''
<h1>Burrow test IdP</h1><p>Sign in to connect to the test VPN.</p>
<form method="post" action="/saml/login">
<input type="hidden" name="sid" value="{html.escape(sid)}">
<label for="username">Username</label><input type="text" id="username" name="username" value="demo">
<label for="password">Password</label><input type="password" id="password" name="password" value="demo">
<button type="submit">Sign in</button>
</form><p class="note">Stand-in identity provider for Burrow development.</p>'''))

    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        form = urllib.parse.parse_qs(self.rfile.read(length).decode())
        sid = form.get('sid', [''])[0]
        username = form.get('username', [''])[0]
        password = form.get('password', [''])[0]
        entry = self.session(sid)
        if not entry:
            return self.send_page(404, page('Burrow test IdP', '<h1>Session expired</h1>'
                '<p>Unknown or expired sign-in session.</p>'))
        if username != 'demo' or password != 'demo':
            return self.send_page(403, page('Burrow test IdP', '<h1>Sign-in failed</h1>'
                '<p>Use demo / demo.</p>'))
        response = saml_response(username, sid)
        with sessions_lock:
            entry['response'] = response
        log(f'idp: issued SAMLResponse for {sid} ({len(response)} bytes base64)')
        self.send_page(200, page('Burrow test IdP', f'''
<h1>Signed in</h1><p>Returning you to the VPN client&hellip;</p>
<form method="post" action="{ACS_URL}">
<input type="hidden" name="SAMLResponse" value="{response}">
<input type="hidden" name="RelayState" value="">
<button type="submit">Continue</button>
</form>
<script>document.forms[0].submit();</script>'''))

    def log_message(self, fmt, *args):
        log('idp', self.client_address[0], fmt % args)


# --- SAML auth through the management interface ------------------------------

def quote(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


class AuthDaemon(threading.Thread):
    """Answers >CLIENT:CONNECT/REAUTH with the AWS SAML challenge-response."""

    def __init__(self, instance):
        super().__init__(daemon=True)
        self.instance = instance

    def send(self, command):
        shown = command if len(command) < 200 else command[:160] + f'... ({len(command)} chars)'
        log(f'mgmt[{self.instance["name"]}] <- {shown}')
        self.sock.sendall(command.encode() + b'\n')

    def run(self):
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', self.instance['mgmt']))
                break
            except OSError:
                time.sleep(0.2)
        reader = self.sock.makefile('rb')
        event, env = None, {}
        for raw in reader:
            line = raw.decode(errors='replace').rstrip('\r\n')
            if line.startswith('>CLIENT:ENV,'):
                item = line[len('>CLIENT:ENV,'):]
                if item != 'END':
                    key, _, value = item.partition('=')
                    env[key] = value
                elif event and event[0] in ('CONNECT', 'REAUTH'):
                    self.decide(event, env)
                continue
            if line.startswith('>CLIENT:'):
                parts = line[len('>CLIENT:'):].split(',')
                event, env = parts, {}
                continue
            if line.startswith('>'):
                log(f'mgmt[{self.instance["name"]}] -> {line[:200]}')

    def decide(self, event, env):
        kind, cid, kid = event[0], event[1], event[2]
        username = env.get('username', '')
        password = env.get('password', '')
        address = env.get('untrusted_ip', '?')
        if password.startswith('ACS::'):
            sid = f'burrow-fake/{secrets.token_hex(8)}/{secrets.token_hex(8)}'
            with sessions_lock:
                sessions[sid] = {'created': time.time(), 'response': None}
            url = f'http://{IDP_HOST}/saml/login?sid={urllib.parse.quote(sid, safe="")}'
            user64 = base64.b64encode(username.encode()).decode()
            log(f'auth: {address} asked for SAML ({password}, user {username!r}) -> sid {sid}')
            self.send(f'client-deny {cid} {kid} {quote("SAML authentication required")} '
                      f'{quote(f"CRV1:R:{sid}:{user64}:{url}")}')
            return
        if password.startswith('CRV1::'):
            sid, _, encoded = password[len('CRV1::'):].partition('::')
            with sessions_lock:
                entry = sessions.get(sid)
                expected = entry and entry['response']
                fresh = entry and time.time() - entry['created'] <= SESSION_LIFETIME
            decoded = urllib.parse.unquote_plus(encoded)
            ok = bool(expected) and fresh and decoded == expected
            log(f'auth: {address} CRV1 {kind} for {sid}: password {len(password)} chars, '
                f'SAMLResponse {len(decoded)} chars -> {"accept" if ok else "reject"}')
            if username != 'N/A':
                log(f'auth: note, username is {username!r}, AWS clients send "N/A"')
            if ok:
                self.send(f'client-auth-nt {cid} {kid}')
            else:
                self.send(f'client-deny {cid} {kid} {quote("SAML response rejected")} '
                          f'{quote("SAML response rejected")}')
            return
        log(f'auth: {address} {kind} with unexpected credentials (user {username!r}) -> reject')
        self.send(f'client-deny {cid} {kid} {quote("unsupported credentials")}')


# --- OpenVPN -----------------------------------------------------------------

def server_config(instance):
    lines = [
        f'port {instance["port"]}', f'proto {instance["proto"]}', 'dev tun',
        'topology subnet', f'server {instance["net"]} 255.255.255.0',
        f'ca {PKI}/ca.crt', f'cert {PKI}/server.crt', f'key {PKI}/server.key', 'dh none',
        'data-ciphers AES-256-GCM:AES-128-GCM:CHACHA20-POLY1305', 'tls-version-min 1.2',
        'keepalive 10 60', 'reneg-sec 0', 'verb 3', 'persist-key', 'persist-tun',
        'push "route 10.100.0.0 255.255.0.0"',
        'push "dhcp-option DNS 10.100.0.2"',
        'push "dhcp-option DOMAIN burrow.test"',
    ]
    if instance['proto'] == 'udp':
        lines.append('explicit-exit-notify 1')
    if TUNNEL == 'full':
        lines.append('push "redirect-gateway def1"')
    if MODE == 'saml':
        lines += ['verify-client-cert none', 'username-as-common-name', 'duplicate-cn',
                  f'management 127.0.0.1 {instance["mgmt"]}', 'management-client-auth']
    path = pathlib.Path(f'/run/openvpn-{instance["name"]}.conf')
    path.write_text('\n'.join(lines) + '\n')
    return path


def serve():
    ensure_pki()
    setup_network()
    log(f'mode={MODE} tunnel={TUNNEL} idp={IDP_HOST}')
    procs = [start_dnsmasq()]
    for server, handler, address in [
            (http.server.ThreadingHTTPServer, VPCHandler, ('10.100.0.1', 80)),
            (http.server.ThreadingHTTPServer, IdPHandler, ('0.0.0.0', 18080))]:
        threading.Thread(target=server(address, handler).serve_forever, daemon=True).start()
    for instance in INSTANCES:
        procs.append(subprocess.Popen(['openvpn-server-emu', '--config',
                                       str(server_config(instance))]))
        if MODE == 'saml':
            AuthDaemon(instance).start()
    while True:
        for proc in procs:
            if proc.poll() is not None:
                log(f'{proc.args[0]} exited with {proc.returncode}; stopping')
                for other in procs:
                    if other.poll() is None:
                        other.terminate()
                sys.exit(1)
        time.sleep(1)


def main(argv):
    command = argv[1] if len(argv) > 1 else 'serve'
    if command == 'serve':
        serve()
    elif command == 'profile':
        ensure_pki()
        args = argv[2:]
        sys.stdout.write(profile(*(args[:4] or ['saml'])))
    elif command == 'client-test':
        ensure_pki()
        os.execvp('python3', ['python3', '-u', '/opt/burrow/client_test.py'] + argv[2:])
    else:
        sys.exit(f'unknown command {command}')


if __name__ == '__main__':
    main(sys.argv)
