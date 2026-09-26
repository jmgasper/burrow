#include "Profile.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

namespace burrow {

namespace {

// Directives Burrow handles itself or refuses for safety.
const std::set<std::string> kDropped = {
    // Burrow picks and resolves the server, and answers credential queries.
    "remote", "remote-random-hostname", "remote-random", "auth-federate", "auth-retry",
    "auth-user-pass", "management", "management-client", "management-hold",
    "management-query-passwords", "management-signal", "management-forget-disconnect",
    "management-up-down", "management-client-auth", "management-external-key",
    "management-external-cert", "static-challenge",
    // No scripts, plug-ins, log files, daemons or privilege changes.
    "script-security", "up", "down", "route-up", "route-pre-down", "ipchange",
    "tls-verify", "auth-user-pass-verify", "client-connect", "client-disconnect",
    "learn-address", "plugin", "daemon", "log", "log-append", "syslog", "status",
    "writepid", "cd", "chroot", "user", "group", "config", "setenv-safe",
    // Windows-only directives OpenVPN on Haiku rejects.
    "block-outside-dns", "cryptoapicert", "register-dns", "tap-sleep", "dhcp-renew",
    "dhcp-release", "ip-win32", "route-method", "win-sys", "windows-driver",
};

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string Trim(const std::string& text)
{
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

// Walks the lines of a profile, telling inline blocks (<ca>...</ca>) apart.
template<typename F>
void ForEachLine(const std::string& text, F f)
{
    std::istringstream stream(text);
    std::string line;
    std::string block;
    while (std::getline(stream, line)) {
        std::string trimmed = Trim(line);
        if (!block.empty()) {
            if (Lower(trimmed) == "</" + block + ">") {
                f(line, std::vector<std::string>(), block, true);
                block.clear();
            } else
                f(line, std::vector<std::string>(), block, false);
            continue;
        }
        if (trimmed.size() > 2 && trimmed[0] == '<' && trimmed.back() == '>'
            && trimmed[1] != '/') {
            block = Lower(trimmed.substr(1, trimmed.size() - 2));
            f(line, std::vector<std::string>(), block, true);
            continue;
        }
        f(line, TokenizeLine(line), std::string(), false);
    }
}

}  // namespace


AuthKind ProfileConfig::Auth() const
{
    if (federated)
        return AuthKind::Federated;
    if (userPassword)
        return AuthKind::UserPassword;
    return AuthKind::Certificate;
}


std::string ProfileConfig::ProtoFor(const Remote& remote) const
{
    std::string value = remote.proto.empty() ? proto : remote.proto;
    return value.compare(0, 3, "tcp") == 0 ? "tcp" : "udp";
}


std::vector<std::string> TokenizeLine(const std::string& line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inToken = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (quote) {
            if (c == quote)
                quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < line.size())
                current += line[++i];
            else
                current += c;
            continue;
        }
        if (!inToken && (c == '#' || c == ';'))
            break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (inToken) {
                tokens.push_back(current);
                current.clear();
                inToken = false;
            }
            continue;
        }
        inToken = true;
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '\\' && i + 1 < line.size())
            current += line[++i];
        else
            current += c;
    }
    if (inToken)
        tokens.push_back(current);
    // OpenVPN accepts "--directive" spelling in files too.
    if (!tokens.empty() && tokens[0].compare(0, 2, "--") == 0)
        tokens[0] = tokens[0].substr(2);
    return tokens;
}


ProfileConfig ParseProfile(const std::string& text)
{
    ProfileConfig config;
    std::set<std::string> dropped;
    ForEachLine(text, [&](const std::string&, const std::vector<std::string>& tokens,
            const std::string& block, bool blockEdge) {
        if (!block.empty()) {
            if (blockEdge && (block == "cert" || block == "key" || block == "pkcs12"))
                config.clientCertificate = true;
            return;
        }
        if (tokens.empty())
            return;
        std::string name = Lower(tokens[0]);
        if (name == "remote" && tokens.size() >= 2) {
            Remote remote;
            remote.host = tokens[1];
            if (tokens.size() >= 3)
                remote.port = std::atoi(tokens[2].c_str());
            if (tokens.size() >= 4)
                remote.proto = Lower(tokens[3]);
            if (remote.port <= 0 || remote.port > 65535)
                remote.port = 1194;
            config.remotes.push_back(remote);
        } else if (name == "proto" && tokens.size() >= 2)
            config.proto = Lower(tokens[1]);
        else if (name == "remote-random-hostname")
            config.randomHostname = true;
        else if (name == "auth-federate")
            config.federated = true;
        else if (name == "auth-user-pass")
            config.userPassword = true;
        else if (name == "cert" || name == "key" || name == "pkcs12")
            config.clientCertificate = true;
        else if (name == "static-challenge" && tokens.size() >= 2) {
            config.staticChallenge = tokens[1];
            config.staticChallengeEcho = tokens.size() >= 3 && tokens[2] == "1";
        }
        static const std::set<std::string> kSilent = {
            "remote", "remote-random-hostname", "auth-federate", "auth-retry",
            "auth-user-pass", "static-challenge"
        };
        if (kDropped.count(name) && !kSilent.count(name))
            dropped.insert(name);
    });
    config.dropped.assign(dropped.begin(), dropped.end());
    return config;
}


std::string RuntimeConfig(const std::string& text)
{
    std::string result;
    ForEachLine(text, [&](const std::string& line, const std::vector<std::string>& tokens,
            const std::string& block, bool) {
        if (block.empty() && !tokens.empty() && kDropped.count(Lower(tokens[0])))
            result += "# (handled by Burrow) " + Trim(line) + "\n";
        else
            result += line + "\n";
    });
    return result;
}


std::string AwsRegion(const std::string& host)
{
    // cvpn-endpoint-0123456789abcdef0.prod.clientvpn.us-east-1.amazonaws.com
    std::string lower = Lower(host);
    size_t marker = lower.find(".clientvpn.");
    if (marker == std::string::npos)
        return "";
    size_t start = marker + strlen(".clientvpn.");
    size_t end = lower.find('.', start);
    if (end == std::string::npos)
        return "";
    return lower.substr(start, end - start);
}


std::string SuggestProfileName(const ProfileConfig& config, const std::string& fileName)
{
    std::string stem = fileName;
    size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos)
        stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        stem = stem.substr(0, dot);

    std::string host = config.remotes.empty() ? "" : config.remotes[0].host;
    std::string region = AwsRegion(host);
    // The AWS console names every download "downloaded-client-config".
    bool generic = stem.empty() || Lower(stem).find("downloaded-client-config") == 0
        || Lower(stem) == "client" || Lower(stem) == "config";
    if (!generic)
        return stem;
    if (!region.empty())
        return "AWS Client VPN (" + region + ")";
    if (!host.empty())
        return host;
    return "VPN";
}


std::string AuthDescription(AuthKind kind)
{
    switch (kind) {
        case AuthKind::Federated:
            return "Single sign-on (SAML)";
        case AuthKind::UserPassword:
            return "User name and password";
        case AuthKind::Certificate:
            break;
    }
    return "Certificate";
}

}  // namespace burrow
