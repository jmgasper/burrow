#include "Protocol.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace burrow {

const char* const kFederatedUser = "N/A";
const char* const kFederatedFirstPass = "ACS::35001";

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool StartsWith(const std::string& text, const char* prefix)
{
    return text.compare(0, strlen(prefix), prefix) == 0;
}

std::vector<std::string> Split(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return parts;
}

}  // namespace


bool ParseCrv1(const std::string& reason, Crv1Challenge& challenge)
{
    // The text may itself contain colons (it is a URL for AWS).
    if (!StartsWith(reason, "CRV1:"))
        return false;
    size_t flagsEnd = reason.find(':', 5);
    if (flagsEnd == std::string::npos)
        return false;
    size_t stateEnd = reason.find(':', flagsEnd + 1);
    if (stateEnd == std::string::npos)
        return false;
    size_t userEnd = reason.find(':', stateEnd + 1);
    if (userEnd == std::string::npos)
        return false;
    challenge.flags = reason.substr(5, flagsEnd - 5);
    challenge.stateId = reason.substr(flagsEnd + 1, stateEnd - flagsEnd - 1);
    challenge.username = Base64Decode(reason.substr(stateEnd + 1, userEnd - stateEnd - 1));
    challenge.text = reason.substr(userEnd + 1);
    return !challenge.stateId.empty();
}


std::string FederatedPassword(const std::string& stateId, const std::string& samlResponse)
{
    return "CRV1::" + stateId + "::" + QueryEscape(samlResponse);
}


std::string QueryEscape(const std::string& text)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size() * 3 / 2);
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            result += c;
        else if (c == ' ')
            result += '+';
        else {
            result += '%';
            result += kHex[c >> 4];
            result += kHex[c & 15];
        }
    }
    return result;
}


std::string UrlDecode(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '+')
            result += ' ';
        else if (c == '%' && i + 2 < text.size() && isxdigit((unsigned char)text[i + 1])
            && isxdigit((unsigned char)text[i + 2])) {
            result += (char)strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            result += c;
    }
    return result;
}


std::string Base64Encode(const std::string& data)
{
    std::string result;
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = ((uint8_t)data[i] << 16) | ((uint8_t)data[i + 1] << 8) | (uint8_t)data[i + 2];
        result += kBase64[n >> 18];
        result += kBase64[(n >> 12) & 63];
        result += kBase64[(n >> 6) & 63];
        result += kBase64[n & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        uint32_t n = (uint8_t)data[i] << 16;
        result += kBase64[n >> 18];
        result += kBase64[(n >> 12) & 63];
        result += "==";
    } else if (i + 2 == data.size()) {
        uint32_t n = ((uint8_t)data[i] << 16) | ((uint8_t)data[i + 1] << 8);
        result += kBase64[n >> 18];
        result += kBase64[(n >> 12) & 63];
        result += kBase64[(n >> 6) & 63];
        result += '=';
    }
    return result;
}


std::string Base64Decode(const std::string& text)
{
    std::string result;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        const char* position = strchr(kBase64, c);
        if (c == '\0' || position == nullptr)
            continue;
        buffer = (buffer << 6) | (uint32_t)(position - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += (char)((buffer >> bits) & 0xff);
        }
    }
    return result;
}


std::string ManagementQuote(const std::string& text)
{
    std::string result = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\')
            result += '\\';
        result += c;
    }
    return result + "\"";
}


ManagementLine ParseManagementLine(const std::string& line)
{
    static const struct {
        const char* prefix;
        ManagementKind kind;
    } kPrefixes[] = {
        {">STATE:", ManagementKind::State}, {">PASSWORD:", ManagementKind::Password},
        {">BYTECOUNT:", ManagementKind::ByteCount}, {">LOG:", ManagementKind::Log},
        {">HOLD:", ManagementKind::Hold}, {">FATAL:", ManagementKind::Fatal},
        {">INFO:", ManagementKind::Info}, {">ECHO:", ManagementKind::Echo},
        {">NEED-OK:", ManagementKind::NeedOk}, {">NEED-STR:", ManagementKind::NeedStr},
        {"SUCCESS:", ManagementKind::Success}, {"ERROR:", ManagementKind::Error},
    };
    ManagementLine result;
    std::string text = line;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
    for (const auto& prefix : kPrefixes) {
        if (StartsWith(text, prefix.prefix)) {
            result.kind = prefix.kind;
            result.payload = text.substr(strlen(prefix.prefix));
            if (!result.payload.empty() && result.payload[0] == ' ')
                result.payload.erase(0, 1);
            return result;
        }
    }
    result.payload = text;
    return result;
}


bool ParseState(const std::string& payload, StateInfo& state)
{
    // <time>,<state>,<description>,<local ip>,<remote ip>,<remote port>,...
    std::vector<std::string> fields = Split(payload, ',');
    if (fields.size() < 2)
        return false;
    state = StateInfo();
    state.name = fields[1];
    if (fields.size() > 2)
        state.description = fields[2];
    if (fields.size() > 3)
        state.localAddress = fields[3];
    if (fields.size() > 4)
        state.remoteAddress = fields[4];
    if (fields.size() > 5)
        state.remotePort = fields[5];
    return true;
}


bool ParsePassword(const std::string& payload, PasswordPrompt& prompt)
{
    prompt = PasswordPrompt();
    auto quoted = [&](size_t from) {
        size_t start = payload.find('\'', from);
        if (start == std::string::npos)
            return std::string();
        size_t end = payload.find('\'', start + 1);
        if (end == std::string::npos)
            return std::string();
        return payload.substr(start + 1, end - start - 1);
    };
    if (StartsWith(payload, "Need ")) {
        prompt.kind = PasswordPrompt::Need;
        prompt.type = quoted(0);
        size_t sc = payload.find(" SC:");
        if (sc != std::string::npos) {
            prompt.staticChallenge = true;
            prompt.staticEcho = payload.compare(sc + 4, 1, "1") == 0;
            size_t comma = payload.find(',', sc);
            if (comma != std::string::npos)
                prompt.staticText = payload.substr(comma + 1);
        }
        return true;
    }
    if (StartsWith(payload, "Verification Failed")) {
        prompt.kind = PasswordPrompt::Failed;
        prompt.type = quoted(0);
        // Verification Failed: 'Auth' ['CRV1:R:...']
        size_t open = payload.find("['");
        size_t close = payload.rfind("']");
        if (open != std::string::npos && close != std::string::npos && close > open)
            prompt.reason = payload.substr(open + 2, close - open - 2);
        return true;
    }
    return false;
}


std::string LogMessage(const std::string& payload)
{
    size_t first = payload.find(',');
    if (first == std::string::npos)
        return payload;
    size_t second = payload.find(',', first + 1);
    if (second == std::string::npos)
        return payload.substr(first + 1);
    return payload.substr(second + 1);
}


bool ParsePushReply(const std::string& message, PushedOptions& options)
{
    size_t start = message.find("PUSH_REPLY,");
    if (start == std::string::npos)
        return false;
    std::string body = message.substr(start + strlen("PUSH_REPLY,"));
    // The log wraps the reply in quotes: '...'
    size_t quote = body.rfind('\'');
    if (quote != std::string::npos)
        body.erase(quote);
    for (const std::string& option : Split(body, ',')) {
        std::vector<std::string> words;
        for (const std::string& word : Split(option, ' ')) {
            if (!word.empty())
                words.push_back(word);
        }
        if (words.empty())
            continue;
        if (words[0] == "dhcp-option" && words.size() >= 3) {
            if (words[1] == "DNS" || words[1] == "DNS6")
                options.dnsServers.push_back(words[2]);
            else if (words[1] == "DOMAIN" || words[1] == "DOMAIN-SEARCH"
                || words[1] == "ADAPTER_DOMAIN_SUFFIX")
                options.domains.push_back(words[2]);
        } else if (words[0] == "route" && words.size() >= 2)
            options.routes.push_back(words[1] + "/" + (words.size() >= 3 ? words[2] : "255.255.255.255"));
        else if (words[0] == "redirect-gateway")
            options.redirectGateway = true;
        else if (words[0] == "ifconfig" && words.size() >= 3)
            options.ifconfig = words[1] + " " + words[2];
    }
    return true;
}


bool ParseByteCount(const std::string& payload, uint64_t& received, uint64_t& sent)
{
    size_t comma = payload.find(',');
    if (comma == std::string::npos)
        return false;
    received = strtoull(payload.substr(0, comma).c_str(), nullptr, 10);
    sent = strtoull(payload.substr(comma + 1).c_str(), nullptr, 10);
    return true;
}

}  // namespace burrow
