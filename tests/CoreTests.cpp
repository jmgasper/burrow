// Core tests; run with `make check` on Haiku or `make BUILD=build-host check-host` on Linux.
#include "core/Connection.h"
#include "core/ProfileStore.h"
#include "core/Protocol.h"
#include "core/ResolvConf.h"
#include "core/SamlListener.h"
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace burrow;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition) do { sChecks++; if (!(condition)) { sFailures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_EQ(a, b) do { sChecks++; auto _a = (a); auto _b = (b); if (!(_a == _b)) { \
    sFailures++; fprintf(stderr, "%s:%d: CHECK_EQ failed: %s\n", __FILE__, __LINE__, #a); } \
    } while (0)

static const char kAwsProfile[] =
    "client\n"
    "dev tun\n"
    "proto udp\n"
    "remote cvpn-endpoint-0123456789abcdef0.prod.clientvpn.eu-west-2.amazonaws.com 443\n"
    "remote-random-hostname\n"
    "resolv-retry infinite\n"
    "nobind\n"
    "remote-cert-tls server\n"
    "cipher AES-256-GCM\n"
    "verb 3\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "remote in a block is not a directive\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "auth-user-pass\n"
    "auth-federate\n"
    "auth-retry interact\n"
    "auth-nocache\n"
    "reneg-sec 0\n"
    "block-outside-dns\n";


static void TestProfile()
{
    std::vector<std::string> tokens = TokenizeLine("  setenv  \"a b\" 'c d' e\\ f # comment");
    CHECK_EQ(tokens.size(), (size_t)4);
    CHECK_EQ(tokens[1], std::string("a b"));
    CHECK_EQ(tokens[2], std::string("c d"));
    CHECK_EQ(tokens[3], std::string("e f"));
    CHECK(TokenizeLine("# remote x").empty());
    CHECK(TokenizeLine("; remote x").empty());
    CHECK_EQ(TokenizeLine("--remote x")[0], std::string("remote"));

    ProfileConfig config = ParseProfile(kAwsProfile);
    CHECK_EQ(config.remotes.size(), (size_t)1);
    CHECK_EQ(config.remotes[0].port, 443);
    CHECK(config.randomHostname);
    CHECK(config.federated);
    CHECK(config.Auth() == AuthKind::Federated);
    CHECK_EQ(config.ProtoFor(config.remotes[0]), std::string("udp"));
    CHECK_EQ(AwsRegion(config.remotes[0].host), std::string("eu-west-2"));
    CHECK_EQ(SuggestProfileName(config, "/x/downloaded-client-config.ovpn"),
        std::string("AWS Client VPN (eu-west-2)"));
    CHECK_EQ(SuggestProfileName(config, "Staging.ovpn"), std::string("Staging"));
    CHECK_EQ(config.dropped.size(), (size_t)1);   // block-outside-dns

    std::string runtime = RuntimeConfig(kAwsProfile);
    CHECK(runtime.find("\nauth-federate") == std::string::npos);
    CHECK(runtime.find("\nremote cvpn") == std::string::npos);
    CHECK(runtime.find("\nblock-outside-dns") == std::string::npos);
    CHECK(runtime.find("remote in a block is not a directive") != std::string::npos);
    CHECK(runtime.find("\nauth-nocache\n") != std::string::npos);
    CHECK(runtime.find("\ncipher AES-256-GCM\n") != std::string::npos);

    ProfileConfig mtls = ParseProfile("client\nremote 10.0.2.2 11194\n<cert>\nx\n</cert>\n<key>\ny\n</key>\n");
    CHECK(mtls.Auth() == AuthKind::Certificate);
    CHECK(mtls.clientCertificate);
    ProfileConfig tcp = ParseProfile("client\nproto tcp-client\nremote a 1 \nremote b 2 udp\n");
    CHECK_EQ(tcp.ProtoFor(tcp.remotes[0]), std::string("tcp"));
    CHECK_EQ(tcp.ProtoFor(tcp.remotes[1]), std::string("udp"));
    ProfileConfig challenge = ParseProfile("remote a\nauth-user-pass\nstatic-challenge \"Enter PIN\" 1\n");
    CHECK(challenge.Auth() == AuthKind::UserPassword);
    CHECK_EQ(challenge.staticChallenge, std::string("Enter PIN"));
    CHECK(challenge.staticChallengeEcho);
}


static void TestProtocol()
{
    Crv1Challenge challenge;
    CHECK(ParseCrv1("CRV1:R:instance-1/a/b:Ti9B:https://idp.example.com/saml?x=1:2", challenge));
    CHECK_EQ(challenge.flags, std::string("R"));
    CHECK_EQ(challenge.stateId, std::string("instance-1/a/b"));
    CHECK_EQ(challenge.username, std::string("N/A"));
    CHECK_EQ(challenge.text, std::string("https://idp.example.com/saml?x=1:2"));
    CHECK(!ParseCrv1("Username or password is too long", challenge));

    CHECK_EQ(QueryEscape("aZ09-_.~"), std::string("aZ09-_.~"));
    CHECK_EQ(QueryEscape("a+b/c=d e"), std::string("a%2Bb%2Fc%3Dd+e"));
    CHECK_EQ(UrlDecode("a%2Bb%2fc%3Dd+e"), std::string("a+b/c=d e"));
    CHECK_EQ(FederatedPassword("s", "PD94=/+"), std::string("CRV1::s::PD94%3D%2F%2B"));
    for (const char* text : {"", "N", "N/", "N/A", "hello world!"})
        CHECK_EQ(Base64Decode(Base64Encode(text)), std::string(text));
    CHECK_EQ(Base64Encode("N/A"), std::string("Ti9B"));
    CHECK_EQ(ManagementQuote("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));

    ManagementLine line = ParseManagementLine(">STATE:1727330000,CONNECTED,SUCCESS,10.99.0.2,10.0.2.2,11194,,\r\n");
    CHECK(line.kind == ManagementKind::State);
    StateInfo state;
    CHECK(ParseState(line.payload, state));
    CHECK_EQ(state.name, std::string("CONNECTED"));
    CHECK_EQ(state.localAddress, std::string("10.99.0.2"));
    CHECK_EQ(state.remotePort, std::string("11194"));
    CHECK(ParseManagementLine("SUCCESS: hold release succeeded").kind == ManagementKind::Success);

    PasswordPrompt prompt;
    CHECK(ParsePassword("Need 'Auth' username/password", prompt));
    CHECK(prompt.kind == PasswordPrompt::Need);
    CHECK_EQ(prompt.type, std::string("Auth"));
    CHECK(!prompt.staticChallenge);
    CHECK(ParsePassword("Need 'Auth' username/password SC:1,Enter PIN", prompt));
    CHECK(prompt.staticChallenge);
    CHECK(prompt.staticEcho);
    CHECK_EQ(prompt.staticText, std::string("Enter PIN"));
    CHECK(ParsePassword("Verification Failed: 'Auth' ['CRV1:R:s:Ti9B:http://h:1/p?a=b']", prompt));
    CHECK(prompt.kind == PasswordPrompt::Failed);
    CHECK_EQ(prompt.reason, std::string("CRV1:R:s:Ti9B:http://h:1/p?a=b"));
    CHECK(ParsePassword("Verification Failed: 'Auth'", prompt));
    CHECK(prompt.reason.empty());

    CHECK_EQ(LogMessage("1727330000,I,TUN/TAP device /dev/tun/0 opened, really"),
        std::string("TUN/TAP device /dev/tun/0 opened, really"));
    PushedOptions pushed;
    CHECK(ParsePushReply("PUSH: Received control message: 'PUSH_REPLY,route 10.100.0.0 "
        "255.255.0.0,dhcp-option DNS 10.100.0.2,dhcp-option DOMAIN burrow.test,"
        "redirect-gateway def1,ifconfig 10.99.0.2 255.255.255.0,peer-id 0'", pushed));
    CHECK_EQ(pushed.dnsServers.size(), (size_t)1);
    CHECK_EQ(pushed.dnsServers[0], std::string("10.100.0.2"));
    CHECK_EQ(pushed.domains[0], std::string("burrow.test"));
    CHECK_EQ(pushed.routes[0], std::string("10.100.0.0/255.255.0.0"));
    CHECK(pushed.redirectGateway);
    CHECK_EQ(pushed.ifconfig, std::string("10.99.0.2 255.255.255.0"));
    CHECK(!ParsePushReply("PUSH: Received control message: 'AUTH_FAILED'", pushed));

    uint64_t received = 0, sent = 0;
    CHECK(ParseByteCount("123,4567", received, sent));
    CHECK_EQ(received, (uint64_t)123);
    CHECK_EQ(sent, (uint64_t)4567);
}


static void TestResolvConf()
{
    std::string dhcp = "# Added automatically by DHCP\nnameserver 10.0.2.3\n"
        "# End of automatic DHCP additions\n";
    std::string added = AddVpnDns(dhcp, {"10.100.0.2"}, {"burrow.test"}, "Test");
    CHECK(HasVpnDns(added));
    CHECK(added.find("nameserver 10.100.0.2\nsearch burrow.test\n") != std::string::npos);
    // VPN first, then the marker that makes net_server keep it on renewals.
    CHECK(added.find("10.100.0.2") < added.find("# Dynamic DNS entries"));
    CHECK(added.find("# Dynamic DNS entries") < added.find("10.0.2.3"));
    CHECK_EQ(RemoveVpnDns(added), dhcp);
    // Applying twice replaces the block.
    std::string twice = AddVpnDns(added, {"10.1.1.1"}, {}, "Other");
    CHECK(twice.find("10.100.0.2") == std::string::npos);
    CHECK_EQ(RemoveVpnDns(twice), dhcp);

    std::string user = "nameserver 9.9.9.9\n# Dynamic DNS entries\n# Added automatically by DHCP\n"
        "nameserver 10.0.2.3\n";
    std::string withUser = AddVpnDns(user, {"10.100.0.2"}, {}, "Test");
    CHECK(withUser.find("9.9.9.9") < withUser.find("10.100.0.2"));
    CHECK(withUser.find("10.100.0.2") < withUser.find("# Dynamic DNS entries"));
    CHECK_EQ(RemoveVpnDns(withUser), user);
    CHECK_EQ(AddVpnDns(dhcp, {}, {}, "Test"), dhcp);

    char path[] = "/tmp/burrow-resolv-XXXXXX";
    int fd = mkstemp(path);
    CHECK(write(fd, dhcp.data(), dhcp.size()) == (ssize_t)dhcp.size());
    close(fd);
    CHECK(ApplyVpnDns(path, {"10.100.0.2"}, {"burrow.test"}, "Test"));
    std::string content;
    CHECK(ReadFile(path, content));
    CHECK(HasVpnDns(content));
    CHECK(RestoreDns(path));
    CHECK(ReadFile(path, content));
    CHECK_EQ(content, dhcp);
    unlink(path);
}


static void TestProfileStore()
{
    char directory[] = "/tmp/burrow-store-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    std::string base = std::string(directory) + "/profiles";
    ProfileStore store(base);
    CHECK(store.Load());
    CHECK(store.Profiles().empty());
    std::string id, error;
    CHECK(!store.Import("just some text\n", "Bad", id, error));
    CHECK(!error.empty());
    CHECK(store.Import(kAwsProfile, "Work", id, error));
    CHECK_EQ(store.Profiles().size(), (size_t)1);
    struct stat info;
    CHECK(stat((base + "/" + id + ".ovpn").c_str(), &info) == 0 && (info.st_mode & 077) == 0);
    std::string second;
    CHECK(store.Import(kAwsProfile, "Work", second, error));
    CHECK_EQ(store.Find(second)->name, std::string("Work 2"));
    CHECK(store.Rename(second, "Alpha"));
    CHECK_EQ(store.Profiles()[0].name, std::string("Alpha"));
    ProfileStore reloaded(base);
    CHECK(reloaded.Load());
    CHECK_EQ(reloaded.Profiles().size(), (size_t)2);
    CHECK(reloaded.Find(id)->config.federated);
    CHECK(reloaded.Remove(id));
    CHECK(reloaded.Remove(second));
    CHECK(reloaded.Profiles().empty());
    rmdir(base.c_str());
    rmdir(directory);
}


static std::string HttpPost(int port, const std::string& body)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&address, sizeof(address)) != 0) {
        close(fd);
        return "";
    }
    std::string request = "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
        "application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size())
        + "\r\n\r\n" + body;
    for (size_t sent = 0; sent < request.size(); ) {
        ssize_t result = send(fd, request.data() + sent, request.size() - sent, 0);
        if (result <= 0)
            break;
        sent += result;
    }
    std::string response;
    char buffer[4096];
    ssize_t received;
    while ((received = recv(fd, buffer, sizeof(buffer), 0)) > 0)
        response.append(buffer, received);
    close(fd);
    return response;
}


// A SAMLResponse of realistic size, with every character that needs escaping.
static std::string FakeSaml()
{
    std::string xml = "<samlp:Response ID=\"_x\">";
    while (xml.size() < 9000)
        xml += "<saml:Attribute Name=\"memberOf\">engineering &amp; ops</saml:Attribute>";
    return Base64Encode(xml + "</samlp:Response>");
}


static void TestSamlListener()
{
    CHECK_EQ(SamlListener::FormValue("RelayState=&SAMLResponse=PD94%2B%2F%3D", "SAMLResponse"),
        std::string("PD94+/="));
    CHECK_EQ(SamlListener::FormValue("a=1&b=2", "c"), std::string());

    std::mutex lock;
    std::condition_variable changed;
    std::string got;
    SamlListener listener([&](const std::string& response) {
        std::lock_guard<std::mutex> guard(lock);
        got = response;
        changed.notify_all();
    }, 35911);
    std::string error;
    CHECK(listener.Start(error));
    SamlListener second([](const std::string&) {}, 35911);
    CHECK(!second.Start(error));
    CHECK(error.find("35911") != std::string::npos);
    std::string saml = FakeSaml();
    std::string response = HttpPost(35911, "SAMLResponse=" + QueryEscape(saml) + "&RelayState=");
    CHECK(response.find("200 OK") != std::string::npos);
    CHECK(response.find("You are signed in") != std::string::npos);
    {
        std::unique_lock<std::mutex> guard(lock);
        changed.wait_for(guard, std::chrono::seconds(5), [&] { return !got.empty(); });
        CHECK_EQ(got, saml);
    }
    listener.Stop();
    CHECK(!listener.Running());
}


class TestListener : public ConnectionListener {
public:
    std::mutex lock;
    std::condition_variable changed;
    std::vector<ConnectionState> states;
    std::vector<std::string> logs;
    std::string url;
    int credentialRequests = 0;
    bool retried = false;
    Connection* connection = nullptr;

    void ConnectionChanged(const std::string&, ConnectionState state, const std::string& detail) override
    {
        std::lock_guard<std::mutex> guard(lock);
        states.push_back(state);
        if (state == ConnectionState::Failed)
            fprintf(stderr, "  connection failed: %s\n", detail.c_str());
        changed.notify_all();
    }
    void ConnectionLog(const std::string&, const std::string& line) override
    {
        std::lock_guard<std::mutex> guard(lock);
        logs.push_back(line);
    }
    void ConnectionStats(const std::string&, uint64_t, uint64_t) override {}
    void ConnectionNeedsSignIn(const std::string&, const std::string& signInUrl) override
    {
        std::lock_guard<std::mutex> guard(lock);
        url = signInUrl;
        changed.notify_all();
    }
    void ConnectionNeedsCredentials(const std::string&, const std::string&, bool, bool retry) override
    {
        credentialRequests++;
        retried |= retry;
        connection->SubmitCredentials("alice", retry ? "secret\"\\" : "wrong", "");
    }
    bool WaitFor(ConnectionState state, int seconds = 10)
    {
        std::unique_lock<std::mutex> guard(lock);
        return changed.wait_for(guard, std::chrono::seconds(seconds), [&] {
            return !states.empty() && states.back() == state;
        });
    }
};


static std::string sFakeOpenVPN;

static void RunConnection(const char* scenario, const std::string& profile)
{
    setenv("BURROW_FAKE_SCENARIO", scenario, 1);
    std::string saml = FakeSaml();
    setenv("BURROW_FAKE_SAML", saml.c_str(), 1);
    char directory[] = "/tmp/burrow-run-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);

    TestListener events;
    ConnectionSettings settings;
    settings.id = "test";
    settings.name = "Test";
    settings.profileText = profile;
    settings.openvpnPath = sFakeOpenVPN;
    settings.runtimeDirectory = std::string(directory) + "/run";
    Connection connection(settings, &events);
    events.connection = &connection;

    SamlListener saml35001([&](const std::string& response) {
        connection.SubmitSamlResponse(response);
    }, 35912);
    std::string error;
    CHECK(saml35001.Start(error));

    connection.Start();
    if (strcmp(scenario, "saml") == 0) {
        {
            std::unique_lock<std::mutex> guard(events.lock);
            events.changed.wait_for(guard, std::chrono::seconds(10), [&] { return !events.url.empty(); });
        }
        CHECK_EQ(events.url, std::string("http://127.0.0.1:18080/saml/login?sid=fake%2F0123"));
        CHECK(connection.State() == ConnectionState::SigningIn);
        CHECK(connection.AwaitingSaml());
        HttpPost(35912, "SAMLResponse=" + QueryEscape(saml) + "&RelayState=");
    }
    CHECK(events.WaitFor(ConnectionState::Connected));
    ConnectionInfo info = connection.Info();
    CHECK_EQ(info.localAddress, std::string("10.99.0.2"));
    CHECK_EQ(info.device, std::string("tun/0"));
    CHECK_EQ(info.pushed.dnsServers.size(), (size_t)1);
    CHECK_EQ(info.pushed.domains.size(), (size_t)1);
    CHECK_EQ(info.serverAddress, std::string("127.0.0.1"));
    if (strcmp(scenario, "userpass") == 0) {
        CHECK_EQ(events.credentialRequests, 2);
        CHECK(events.retried);
    }
    connection.Stop();
    CHECK(events.WaitFor(ConnectionState::Disconnected));
    struct stat status;
    CHECK(stat(settings.runtimeDirectory.c_str(), &status) != 0);   // removed
    unlink((settings.runtimeDirectory + ".log").c_str());
    rmdir(directory);
    saml35001.Stop();
}


static void TestConnection()
{
    RunConnection("saml", "client\ndev tun\nremote 127.0.0.1 11194\nremote-random-hostname\n"
        "auth-user-pass\nauth-federate\nauth-retry interact\n");
    RunConnection("userpass", "client\nremote 127.0.0.1 11194\nauth-user-pass\n");
    RunConnection("cert", "client\nremote 127.0.0.1 11194\n<cert>\nx\n</cert>\n");

    // A missing OpenVPN fails with a message instead of hanging.
    TestListener events;
    ConnectionSettings settings;
    settings.id = "broken";
    settings.profileText = "client\nremote 127.0.0.1 1\n";
    settings.openvpnPath = "/nonexistent/burrow-openvpn";
    settings.runtimeDirectory = "/tmp/burrow-broken-run";
    Connection connection(settings, &events);
    connection.Start();
    CHECK(events.WaitFor(ConnectionState::Failed));
    CHECK(!connection.LastError().empty());
    unlink("/tmp/burrow-broken-run.log");
}


int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    std::string self = argv[0];
    sFakeOpenVPN = argc > 1 ? argv[1] : "tests/fake-openvpn.py";
    TestProfile();
    TestProtocol();
    TestResolvConf();
    TestProfileStore();
    TestSamlListener();
    TestConnection();
    printf("%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
