// OpenVPN management interface messages and the AWS Client VPN sign-in exchange.
//
// AWS single sign-on: the client first authenticates as "N/A" with password
// "ACS::35001". The server refuses with a CRV1 dynamic challenge whose text is
// the identity provider's URL; the browser signs in there and POSTs a
// SAMLResponse to http://127.0.0.1:35001/. The client then reconnects to the
// same server with password "CRV1::<state id>::<url-escaped SAMLResponse>".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace burrow {

constexpr int kSamlPort = 35001;
extern const char* const kFederatedUser;        // "N/A"
extern const char* const kFederatedFirstPass;   // "ACS::35001"

struct Crv1Challenge {
    std::string flags;      // "R" (response required), "E" (echo), ...
    std::string stateId;
    std::string username;   // decoded
    std::string text;       // for AWS: the identity provider URL
};

// Parses "CRV1:<flags>:<state id>:<base64 user>:<text>".
bool ParseCrv1(const std::string& reason, Crv1Challenge& challenge);
std::string FederatedPassword(const std::string& stateId, const std::string& samlResponse);

// Go's url.QueryEscape, which the AWS client's password format follows.
std::string QueryEscape(const std::string& text);
std::string UrlDecode(const std::string& text);
std::string Base64Encode(const std::string& data);
std::string Base64Decode(const std::string& text);

// Quotes a management command parameter.
std::string ManagementQuote(const std::string& text);

enum class ManagementKind {
    State, Password, ByteCount, Log, Hold, Fatal, Info, Echo, NeedOk, NeedStr,
    Success, Error, Other
};

struct ManagementLine {
    ManagementKind kind = ManagementKind::Other;
    std::string payload;   // text after ">KIND:" or "SUCCESS: "
};

ManagementLine ParseManagementLine(const std::string& line);

struct StateInfo {
    std::string name;         // CONNECTING, WAIT, AUTH, GET_CONFIG, CONNECTED, ...
    std::string description;  // SUCCESS, ERROR, reason for RECONNECTING, ...
    std::string localAddress;
    std::string remoteAddress;
    std::string remotePort;
};

bool ParseState(const std::string& payload, StateInfo& state);

struct PasswordPrompt {
    enum Kind { Need, Failed, Other } kind = Other;
    std::string type;             // "Auth", "Private Key", ...
    std::string reason;           // text of ['...'] after a failure
    bool staticChallenge = false; // "SC:<echo>,<text>" after "Need 'Auth' username/password"
    bool staticEcho = false;
    std::string staticText;
};

bool ParsePassword(const std::string& payload, PasswordPrompt& prompt);

// ">LOG:<time>,<flags>,<message>" payload's message.
std::string LogMessage(const std::string& payload);

// Options the server pushed, gathered from "PUSH_REPLY" log lines.
struct PushedOptions {
    std::vector<std::string> dnsServers;
    std::vector<std::string> domains;
    std::vector<std::string> routes;      // "network/netmask"
    bool redirectGateway = false;
    std::string ifconfig;                 // "local netmask-or-peer"

    void Clear() { *this = PushedOptions(); }
};

// Adds the options of a "PUSH: Received control message: 'PUSH_REPLY,...'"
// message; false when the message is not a push reply.
bool ParsePushReply(const std::string& message, PushedOptions& options);

bool ParseByteCount(const std::string& payload, uint64_t& received, uint64_t& sent);

}  // namespace burrow
