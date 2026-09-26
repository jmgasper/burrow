// Connection profiles: the parts of an OpenVPN / AWS Client VPN configuration
// Burrow needs, and the configuration it hands to OpenVPN.
#pragma once
#include <string>
#include <vector>

namespace burrow {

enum class AuthKind {
    Certificate,     // mutual TLS only
    UserPassword,    // auth-user-pass (Active Directory), maybe with a static challenge
    Federated        // auth-federate: AWS single sign-on through SAML
};

struct Remote {
    std::string host;
    int port = 1194;
    std::string proto;   // empty: the profile's proto
};

struct ProfileConfig {
    std::vector<Remote> remotes;
    std::string proto = "udp";
    bool randomHostname = false;
    bool federated = false;
    bool userPassword = false;
    bool clientCertificate = false;
    std::string staticChallenge;       // prompt text of static-challenge
    bool staticChallengeEcho = false;
    std::vector<std::string> dropped;  // directives Burrow does not pass on

    AuthKind Auth() const;
    // "udp" or "tcp" for the given remote.
    std::string ProtoFor(const Remote& remote) const;
};

// Splits one configuration line into OpenVPN tokens (quotes and backslash
// escapes as in OpenVPN's parser). Comments yield no tokens.
std::vector<std::string> TokenizeLine(const std::string& line);

ProfileConfig ParseProfile(const std::string& text);

// The configuration OpenVPN runs with. Burrow supplies the remote, the
// credentials and the retry policy itself, and never lets a profile run
// scripts, write files or drop privileges.
std::string RuntimeConfig(const std::string& text);

// A readable default name for an imported profile.
std::string SuggestProfileName(const ProfileConfig& config, const std::string& fileName);

// The AWS region in a Client VPN endpoint host name, or "".
std::string AwsRegion(const std::string& host);

std::string AuthDescription(AuthKind kind);

}  // namespace burrow
