#!/usr/bin/env python3
"""End-to-end check of the fake endpoint with the AWS-patched Linux client.

Drives openvpn-aws through its management interface the way Burrow does:
ACS::35001 first, parse the CRV1 challenge, sign in at the fake IdP, capture the
SAMLResponse from the auto-submit form, answer CRV1::<sid>::<QueryEscape(resp)>.

usage: client_test.py saml|saml-bad|mtls [--proto udp|tcp] [--full]
                      [--remote burrow-fake-endpoint] [--idp burrow-fake-endpoint:18080]
"""
import argparse
import html.parser
import re
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request

sys.path.insert(0, '/opt/burrow')
import endpoint  # noqa: E402

GREETING = endpoint.VPC_GREETING
results = []


def check(name, ok, detail=''):
    results.append(ok)
    print(f'[{"PASS" if ok else "FAIL"}] {name}' + (f': {detail}' if detail else ''), flush=True)
    return ok


class FormParser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.forms = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'form':
            self.forms.append({'action': attrs.get('action', ''), 'fields': {}})
        elif tag == 'input' and self.forms and attrs.get('name'):
            self.forms[-1]['fields'][attrs['name']] = attrs.get('value', '')


def parse_form(text):
    parser = FormParser()
    parser.feed(text)
    return parser.forms[0]


def idp_sign_in(url, idp_host):
    """Sign in like a browser; returns the SAMLResponse the page would POST."""
    parts = urllib.parse.urlsplit(url)
    reachable = urllib.parse.urlunsplit(parts._replace(netloc=idp_host))
    login = urllib.request.urlopen(reachable, timeout=10).read().decode()
    form = parse_form(login)
    post_url = urllib.parse.urljoin(reachable, form['action'])
    data = urllib.parse.urlencode(form['fields']).encode()
    signed = urllib.request.urlopen(post_url, data=data, timeout=10).read().decode()
    form = parse_form(signed)
    check('IdP auto-submit form targets the AWS ACS listener',
          form['action'] == 'http://127.0.0.1:35001/', form['action'])
    check('page auto-submits and has a Continue button',
          'document.forms[0].submit()' in signed and '>Continue</button>' in signed)
    return form['fields']['SAMLResponse']


def quote(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['saml', 'saml-bad', 'mtls'])
    parser.add_argument('--proto', default='udp', choices=['udp', 'tcp'])
    parser.add_argument('--full', action='store_true', help='expect redirect-gateway def1')
    parser.add_argument('--remote', default='burrow-fake-endpoint')
    parser.add_argument('--idp', default='burrow-fake-endpoint:18080')
    args = parser.parse_args()

    port = '1194' if args.proto == 'udp' else '443'
    text = endpoint.profile('mtls' if args.mode == 'mtls' else 'saml', args.proto,
                            args.remote, port)
    # AWS's client strips its private keyword before handing the file to OpenVPN.
    text = '\n'.join(l for l in text.splitlines() if l.strip() != 'auth-federate') + '\n'
    open('/tmp/test.ovpn', 'w').write(text)
    log_file = open('/tmp/openvpn.log', 'w')
    proc = subprocess.Popen(['openvpn-aws', '--config', '/tmp/test.ovpn',
                             '--management', '127.0.0.1', '7505',
                             '--management-query-passwords', '--management-hold'],
                            stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            sock = socket.create_connection(('127.0.0.1', 7505))
            break
        except OSError:
            time.sleep(0.1)
    reader = sock.makefile('rb')

    def send(command):
        shown = command if len(command) < 120 else command[:100] + f'... ({len(command)} chars)'
        print(f'  mgmt <- {shown}', flush=True)
        sock.sendall(command.encode() + b'\n')

    send('state on')
    send('log on')
    saml_password = None
    challenge = None
    connected = False
    rejected = False
    pushed = ''
    sock.settimeout(90)
    deadline = time.time() + 90
    try:
        for raw in reader:
            line = raw.decode(errors='replace').rstrip('\r\n')
            if 'PUSH_REPLY' in line:
                pushed = line
            if line.startswith('>HOLD:'):
                send('hold release')
            elif line.startswith(">PASSWORD:Need 'Auth'"):
                send(f'username "Auth" {quote("N/A")}')
                send(f'password "Auth" {quote(saml_password or "ACS::35001")}')
            elif line.startswith(">PASSWORD:Verification Failed: 'Auth'"):
                match = re.search(r"\['(CRV1:[^']*)'\]", line)
                if match and not saml_password:
                    challenge = match.group(1)
                    print(f'  challenge: AUTH_FAILED,{challenge}', flush=True)
                    flags, sid, user64, url = re.match(
                        r'CRV1:([^:]*):([^:]*):([^:]*):(.*)', challenge).groups()
                    check('CRV1 challenge has flags R, a sid, base64 "N/A" and the IdP URL',
                          flags == 'R' and sid and user64 == 'Ti9B'
                          and url.startswith('http://10.0.2.2:18080/saml/login?sid='), url)
                    response = idp_sign_in(url, args.idp)
                    check('SAMLResponse is realistically large', len(response) > 10000,
                          f'{len(response)} bytes base64')
                    if args.mode == 'saml-bad':
                        response = response[:-8] + ('A' if response[-8] != 'A' else 'B') + response[-7:]
                    # Go's url.QueryEscape, as used by AWS-compatible clients.
                    saml_password = f'CRV1::{sid}::{urllib.parse.quote_plus(response, safe="")}'
                else:
                    rejected = True
                    print(f'  rejected: {line}', flush=True)
                    break
            elif line.startswith('>STATE:') and ',CONNECTED,SUCCESS,' in line:
                connected = True
                print(f'  {line}', flush=True)
                break
            elif line.startswith('>FATAL') or (line.startswith('>STATE:') and ',EXITING,' in line):
                print(f'  {line}', flush=True)
                break
            if time.time() > deadline:
                break
    except socket.timeout:
        pass

    if args.mode == 'saml-bad':
        check('tampered SAMLResponse is rejected', rejected and not connected)
    else:
        check('tunnel connected', connected)
    if connected:
        routes = subprocess.run(['ip', '-4', 'route'], capture_output=True, text=True).stdout
        check('pushed VPC route 10.100.0.0/16 via tun0', '10.100.0.0/16' in routes
              and 'tun0' in routes.split('10.100.0.0/16', 1)[1].split('\n', 1)[0])
        check('pushed DNS 10.100.0.2 and domain burrow.test',
              'dhcp-option DNS 10.100.0.2' in pushed and 'dhcp-option DOMAIN burrow.test' in pushed,
              pushed.split('PUSH_REPLY,', 1)[-1][:160])
        if args.full:
            check('full tunnel: 0.0.0.0/1 and 128.0.0.0/1 via tun0',
                  '0.0.0.0/1' in routes and '128.0.0.0/1' in routes)
        body = subprocess.run(['curl', '-s', '--max-time', '5', 'http://10.100.0.1/'],
                              capture_output=True, text=True).stdout
        check('curl http://10.100.0.1/ through the tunnel', body == GREETING, body.strip())
        dig = subprocess.run(['dig', '+short', '+time=3', '+tries=1', '@10.100.0.2',
                              'vpc.burrow.test'], capture_output=True, text=True).stdout.strip()
        check('dig vpc.burrow.test @10.100.0.2 through the tunnel', dig == '10.100.0.1', dig)
    send('signal SIGTERM')
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    if not all(results) or not results:
        print('--- openvpn log (tail) ---')
        print(''.join(open('/tmp/openvpn.log').readlines()[-40:]))
        sys.exit(1)


if __name__ == '__main__':
    main()
