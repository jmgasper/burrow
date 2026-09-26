#include "Connection.h"
#include "ResolvConf.h"
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace burrow {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void SetCloseOnExec(int fd)
{
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

std::string RandomHex(int length)
{
    static const char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string result;
    for (int i = 0; i < length; i++)
        result += kHex[device() & 15];
    return result;
}

bool IsAddress(const std::string& host)
{
    in_addr address4;
    in6_addr address6;
    return inet_pton(AF_INET, host.c_str(), &address4) == 1
        || inet_pton(AF_INET6, host.c_str(), &address6) == 1;
}

}  // namespace


const char* StateName(ConnectionState state)
{
    switch (state) {
        case ConnectionState::Disconnected: return "Disconnected";
        case ConnectionState::Connecting: return "Connecting";
        case ConnectionState::SigningIn: return "Signing in";
        case ConnectionState::Connected: return "Connected";
        case ConnectionState::Reconnecting: return "Reconnecting";
        case ConnectionState::Disconnecting: return "Disconnecting";
        case ConnectionState::Failed: return "Failed";
    }
    return "";
}


Connection::Connection(const ConnectionSettings& settings, ConnectionListener* listener)
    :
    fSettings(settings),
    fListener(listener),
    fProfile(ParseProfile(settings.profileText))
{
}


Connection::~Connection()
{
    Stop();
    if (fThread.joinable())
        fThread.join();
}


void Connection::Start()
{
    if (fThread.joinable()) {
        if (Active())
            return;
        fThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(fLock);
        fInfo = ConnectionInfo();
        fLastError.clear();
        fAuthQueried = fFirstPassSent = fHaveResponse = false;
        fChallengeState.clear();
        fSamlResponse.clear();
        fRejections = 0;
    }
    fStopRequested = false;
    _SetState(ConnectionState::Connecting, "Starting");
    fThread = std::thread(&Connection::_Run, this);
}


void Connection::Stop()
{
    fStopRequested = true;
    _Wake();
}


void Connection::SubmitSamlResponse(const std::string& samlResponse)
{
    {
        std::lock_guard<std::mutex> lock(fLock);
        if (fChallengeState.empty())
            return;
        fSamlResponse = samlResponse;
    }
    _Wake();
}


void Connection::SubmitCredentials(const std::string& user, const std::string& password,
    const std::string& response)
{
    {
        std::lock_guard<std::mutex> lock(fLock);
        if (!fChallengeState.empty()) {
            fResponse = response;
            fHaveResponse = true;
        } else {
            fUser = user;
            fPassword = password;
            fResponse = response;
            fHaveCredentials = true;
        }
    }
    _Wake();
}


ConnectionState Connection::State() const
{
    std::lock_guard<std::mutex> lock(fLock);
    return fState;
}


ConnectionInfo Connection::Info() const
{
    std::lock_guard<std::mutex> lock(fLock);
    return fInfo;
}


bool Connection::Active() const
{
    ConnectionState state = State();
    return state != ConnectionState::Disconnected && state != ConnectionState::Failed;
}


bool Connection::AwaitingSaml() const
{
    std::lock_guard<std::mutex> lock(fLock);
    return fProfile.federated && !fChallengeState.empty() && fSamlResponse.empty();
}


std::string Connection::LastError() const
{
    std::lock_guard<std::mutex> lock(fLock);
    return fLastError;
}


void Connection::_SetState(ConnectionState state, const std::string& detail)
{
    {
        std::lock_guard<std::mutex> lock(fLock);
        if (state == ConnectionState::Connected && fState != ConnectionState::Connected)
            fInfo.connectedSince = time(nullptr);
        if (state == ConnectionState::Failed)
            fLastError = detail;
        fState = state;
    }
    fListener->ConnectionChanged(fSettings.id, state, detail);
}


void Connection::_Log(const std::string& line)
{
    fListener->ConnectionLog(fSettings.id, line);
}


void Connection::_Wake()
{
    if (fWakePipe[1] >= 0) {
        char byte = 0;
        if (write(fWakePipe[1], &byte, 1) < 0) {}
    }
}


void Connection::_Run()
{
    std::string error;
    if (pipe(fWakePipe) != 0) {
        _SetState(ConnectionState::Failed, strerror(errno));
        return;
    }
    SetCloseOnExec(fWakePipe[0]);
    SetCloseOnExec(fWakePipe[1]);

    int listenFd = -1;
    do {
        if (fProfile.remotes.empty()) {
            error = "The profile names no server (remote).";
            break;
        }
        if (!_Resolve(error))
            break;
        if (fStopRequested)
            break;

        // OpenVPN connects to us (--management-client), so no other local
        // program can take over its management interface.
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (listenFd < 0 || bind(listenFd, (sockaddr*)&address, sizeof(address)) != 0
            || listen(listenFd, 1) != 0
            || getsockname(listenFd, (sockaddr*)&address, &length) != 0) {
            error = std::string("Cannot open the management socket: ") + strerror(errno);
            break;
        }
        SetCloseOnExec(listenFd);
        if (!_Launch(ntohs(address.sin_port), error))
            break;
        _Serve(listenFd);
    } while (false);

    if (listenFd >= 0)
        close(listenFd);
    if (fManagementFd >= 0) {
        close(fManagementFd);
        fManagementFd = -1;
    }

    // Reap OpenVPN, forcing it after a grace period.
    if (fPid > 0) {
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 100 && !exited; i++) {
            exited = waitpid(fPid, &status, WNOHANG) == fPid;
            if (!exited)
                usleep(100000);
        }
        if (!exited) {
            _Log("OpenVPN did not exit; killing it.");
            kill(fPid, SIGKILL);
            waitpid(fPid, &status, 0);
        }
        if (error.empty() && !fStopRequested && LastError().empty()) {
            std::string tail = _LogTail();
            if (!tail.empty())
                error = "OpenVPN stopped: " + tail;
        }
        fPid = -1;
    }
    _RemoveRuntime();
    close(fWakePipe[0]);
    close(fWakePipe[1]);
    fWakePipe[0] = fWakePipe[1] = -1;

    std::string lastError = LastError();
    if (!error.empty())
        _SetState(ConnectionState::Failed, error);
    else if (!lastError.empty() && !fStopRequested)
        _SetState(ConnectionState::Failed, lastError);
    else
        _SetState(ConnectionState::Disconnected);
}


bool Connection::_Resolve(std::string& error)
{
    const Remote& remote = fProfile.remotes.front();
    std::string host = remote.host;
    // AWS endpoints answer for any name below them; the random label spreads
    // clients over the endpoint's addresses. Both sign-in attempts must reach
    // the same address, so resolve once here.
    if (fProfile.randomHostname && !IsAddress(host))
        host = RandomHex(12) + "." + host;
    {
        std::lock_guard<std::mutex> lock(fLock);
        fInfo.serverHost = remote.host;
        fInfo.port = remote.port;
        fInfo.proto = fProfile.ProtoFor(remote);
    }
    _SetState(ConnectionState::Connecting, "Looking up " + remote.host);

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = fProfile.ProtoFor(remote) == "tcp" ? SOCK_STREAM : SOCK_DGRAM;
    addrinfo* result = nullptr;
    int status = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (status != 0 || result == nullptr) {
        error = "Cannot find the server " + remote.host + ": " + gai_strerror(status);
        return false;
    }
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &((sockaddr_in*)result->ai_addr)->sin_addr, text, sizeof(text));
    freeaddrinfo(result);
    {
        std::lock_guard<std::mutex> lock(fLock);
        fInfo.serverAddress = text;
    }
    _Log("Server " + remote.host + " is " + text);
    return true;
}


bool Connection::_Launch(int managementPort, std::string& error)
{
    mkdir(fSettings.runtimeDirectory.c_str(), 0700);
    chmod(fSettings.runtimeDirectory.c_str(), 0700);
    fConfigPath = fSettings.runtimeDirectory + "/profile.ovpn";
    fLogPath = fSettings.runtimeDirectory + "/openvpn.log";
    if (!WriteFileAtomic(fConfigPath, RuntimeConfig(fSettings.profileText), 0600)) {
        error = "Cannot write " + fConfigPath + ": " + strerror(errno);
        return false;
    }
    chmod(fConfigPath.c_str(), 0600);

    ConnectionInfo info = Info();
    std::vector<std::string> args = {
        fSettings.openvpnPath,
        "--config", fConfigPath,
        "--remote", info.serverAddress, std::to_string(info.port), info.proto,
        "--management", "127.0.0.1", std::to_string(managementPort),
        "--management-client", "--management-hold", "--management-query-passwords",
        "--auth-retry", "interact",
        "--script-security", "1",
        "--verb", "3",
    };
    if (fProfile.federated || fProfile.userPassword)
        args.push_back("--auth-user-pass");
    if (fProfile.federated)
        args.push_back("--auth-nocache");
    if (!fProfile.staticChallenge.empty()) {
        args.push_back("--static-challenge");
        args.push_back(fProfile.staticChallenge);
        args.push_back(fProfile.staticChallengeEcho ? "1" : "0");
    }

    std::vector<char*> argv;
    for (std::string& arg : args)
        argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, fLogPath.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = -1;
    int status = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) {
        error = "Cannot start " + fSettings.openvpnPath + ": " + strerror(status);
        return false;
    }
    fPid = pid;
    _Log("Started OpenVPN (" + std::to_string(pid) + ") for " + info.serverAddress + ":"
        + std::to_string(info.port) + "/" + info.proto);
    return true;
}


void Connection::_Serve(int listenFd)
{
    // Wait for OpenVPN to connect to the management socket.
    time_t deadline = time(nullptr) + 20;
    while (fManagementFd < 0) {
        pollfd fds[2] = {{listenFd, POLLIN, 0}, {fWakePipe[0], POLLIN, 0}};
        int ready = poll(fds, 2, 250);
        if (fds[1].revents & POLLIN) {
            char buffer[64];
            if (read(fWakePipe[0], buffer, sizeof(buffer)) < 0) {}
        }
        if (fStopRequested) {
            kill(fPid, SIGTERM);
            return;
        }
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            fManagementFd = accept(listenFd, nullptr, nullptr);
            if (fManagementFd >= 0)
                SetCloseOnExec(fManagementFd);
            break;
        }
        int status;
        if (waitpid(fPid, &status, WNOHANG) == fPid) {
            // Exited before connecting: typically a profile OpenVPN rejects.
            fPid = -1;
            std::string tail = _LogTail();
            std::lock_guard<std::mutex> lock(fLock);
            fLastError = tail.empty() ? "OpenVPN exited during start-up." : tail;
            return;
        }
        if (time(nullptr) > deadline) {
            std::lock_guard<std::mutex> lock(fLock);
            fLastError = "OpenVPN did not open its management connection.";
            kill(fPid, SIGTERM);
            return;
        }
    }

    _Send("state on");
    _Send("bytecount 2");
    _Send("log on all");
    _Send("hold release");

    std::string buffer;
    bool stopSent = false;
    time_t stopTime = 0;
    while (true) {
        if (fStopRequested && !stopSent) {
            _SetState(ConnectionState::Disconnecting);
            _Send("signal SIGTERM");
            stopSent = true;
            stopTime = time(nullptr);
        }
        if (stopSent && time(nullptr) > stopTime + 8)
            return;   // _Run kills it

        pollfd fds[2] = {{fManagementFd, POLLIN, 0}, {fWakePipe[0], POLLIN, 0}};
        int ready = poll(fds, 2, 1000);
        if (ready < 0 && errno != EINTR)
            return;
        if (fds[1].revents & POLLIN) {
            char drain[64];
            if (read(fWakePipe[0], drain, sizeof(drain)) < 0) {}
            _AnswerAuth();
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char chunk[8192];
            ssize_t received = recv(fManagementFd, chunk, sizeof(chunk), 0);
            if (received <= 0)
                return;   // OpenVPN exited
            buffer.append(chunk, received);
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                _Handle(line);
            }
        }
    }
}


void Connection::_Handle(const std::string& text)
{
    ManagementLine line = ParseManagementLine(text);
    switch (line.kind) {
        case ManagementKind::Log: {
            std::string message = LogMessage(line.payload);
            _Log(message);
            PushedOptions pushed;
            {
                std::lock_guard<std::mutex> lock(fLock);
                pushed = fInfo.pushed;
            }
            if (message.find("PUSH_REPLY,") != std::string::npos
                && ParsePushReply(message, pushed)) {
                std::lock_guard<std::mutex> lock(fLock);
                fInfo.pushed = pushed;
            }
            size_t device = message.find("TUN/TAP device /dev/");
            if (device != std::string::npos) {
                std::string name = message.substr(device + 20);
                name = name.substr(0, name.find(' '));
                std::lock_guard<std::mutex> lock(fLock);
                fInfo.device = name;
            }
            break;
        }
        case ManagementKind::State: {
            StateInfo state;
            if (!ParseState(line.payload, state))
                break;
            ConnectionState current = State();
            if (state.name == "CONNECTED") {
                {
                    std::lock_guard<std::mutex> lock(fLock);
                    fInfo.localAddress = state.localAddress;
                    fRejections = 0;
                }
                _SetState(ConnectionState::Connected,
                    state.description == "SUCCESS" ? "" : "Connected with errors; see the log");
            } else if (state.name == "RECONNECTING") {
                {
                    std::lock_guard<std::mutex> lock(fLock);
                    fInfo.pushed.Clear();
                }
                if (current == ConnectionState::Connected)
                    _SetState(ConnectionState::Reconnecting, state.description);
            } else if (state.name == "EXITING") {
                _SetState(ConnectionState::Disconnecting);
            } else if (state.name == "AUTH_PENDING") {
                _SetState(ConnectionState::SigningIn, "Waiting for sign-in");
            } else if (current != ConnectionState::SigningIn
                && current != ConnectionState::Reconnecting) {
                std::string detail = state.name;
                if (state.name == "WAIT")
                    detail = "Waiting for the server";
                else if (state.name == "AUTH")
                    detail = "Authenticating";
                else if (state.name == "GET_CONFIG")
                    detail = "Getting the configuration";
                else if (state.name == "ASSIGN_IP")
                    detail = "Assigning the address";
                else if (state.name == "ADD_ROUTES")
                    detail = "Adding routes";
                else if (state.name == "CONNECTING" || state.name == "TCP_CONNECT"
                    || state.name == "RESOLVE")
                    detail = "Contacting the server";
                _SetState(ConnectionState::Connecting, detail);
            }
            break;
        }
        case ManagementKind::ByteCount: {
            uint64_t received, sent;
            if (ParseByteCount(line.payload, received, sent)) {
                {
                    std::lock_guard<std::mutex> lock(fLock);
                    fInfo.received = received;
                    fInfo.sent = sent;
                }
                fListener->ConnectionStats(fSettings.id, received, sent);
            }
            break;
        }
        case ManagementKind::Password: {
            PasswordPrompt prompt;
            if (ParsePassword(line.payload, prompt))
                _HandlePassword(prompt);
            break;
        }
        case ManagementKind::Hold:
            // After a refused attempt OpenVPN restarts and holds again.
            // Release at once; a pending sign-in waits at the credential query.
            _Send("hold release");
            break;
        case ManagementKind::Fatal: {
            std::lock_guard<std::mutex> lock(fLock);
            fLastError = line.payload;
            break;
        }
        case ManagementKind::Error:
            _Log("Management: ERROR: " + line.payload);
            break;
        default:
            break;
    }
}


void Connection::_HandlePassword(const PasswordPrompt& prompt)
{
    if (prompt.type != "Auth") {
        std::lock_guard<std::mutex> lock(fLock);
        fLastError = "The profile needs a '" + prompt.type
            + "' password, which Burrow does not support yet.";
        fStopRequested = true;
        return;
    }

    if (prompt.kind == PasswordPrompt::Need) {
        {
            std::lock_guard<std::mutex> lock(fLock);
            fAuthQueried = true;
        }
        _AnswerAuth();
        return;
    }
    if (prompt.kind != PasswordPrompt::Failed)
        return;

    Crv1Challenge challenge;
    if (ParseCrv1(prompt.reason, challenge)) {
        bool federated = fProfile.federated;
        int rejections;
        {
            std::lock_guard<std::mutex> lock(fLock);
            fChallengeState = challenge.stateId;
            fSamlResponse.clear();
            fHaveResponse = false;
            fFirstPassSent = false;
            rejections = ++fRejections;
        }
        if (rejections > 3) {
            std::lock_guard<std::mutex> lock(fLock);
            fLastError = "The server keeps asking to sign in again.";
            fStopRequested = true;
            return;
        }
        if (federated) {
            _SetState(ConnectionState::SigningIn, "Sign in with your browser");
            fListener->ConnectionNeedsSignIn(fSettings.id, challenge.text);
        } else {
            _SetState(ConnectionState::SigningIn, "Waiting for your response");
            fListener->ConnectionNeedsCredentials(fSettings.id, challenge.text,
                challenge.flags.find('E') != std::string::npos, false);
        }
        return;
    }

    // A plain refusal.
    if (fProfile.federated) {
        std::lock_guard<std::mutex> lock(fLock);
        fLastError = "The server refused the sign-in"
            + (prompt.reason.empty() ? std::string(".") : ": " + prompt.reason);
        fStopRequested = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(fLock);
        fHaveCredentials = false;
        fPassword.clear();
    }
    _SetState(ConnectionState::SigningIn, "The user name or password was not accepted");
    fListener->ConnectionNeedsCredentials(fSettings.id, std::string(), false, true);
}


void Connection::_AnswerAuth()
{
    std::string user;
    std::string password;
    bool askCredentials = false;
    {
        std::lock_guard<std::mutex> lock(fLock);
        if (!fAuthQueried)
            return;
        if (fProfile.federated) {
            if (!fChallengeState.empty()) {
                if (fSamlResponse.empty())
                    return;   // still signing in
                user = kFederatedUser;
                password = FederatedPassword(fChallengeState, fSamlResponse);
                fChallengeState.clear();
                fSamlResponse.clear();
            } else {
                user = kFederatedUser;
                password = kFederatedFirstPass;
                fFirstPassSent = true;
            }
        } else if (fProfile.userPassword) {
            if (!fChallengeState.empty()) {
                if (!fHaveResponse)
                    return;
                user = fUser;
                password = "CRV1::" + fChallengeState + "::" + fResponse;
                fChallengeState.clear();
                fHaveResponse = false;
            } else if (fHaveCredentials) {
                user = fUser;
                password = fPassword;
                if (!fProfile.staticChallenge.empty())
                    password = "SCRV1:" + Base64Encode(fPassword) + ":" + Base64Encode(fResponse);
            } else
                askCredentials = true;
        } else {
            fLastError = "The server asked for a password the profile does not use.";
            fStopRequested = true;
            return;
        }
        if (!askCredentials)
            fAuthQueried = false;
    }
    if (askCredentials) {
        _SetState(ConnectionState::SigningIn, "Enter your user name and password");
        fListener->ConnectionNeedsCredentials(fSettings.id, fProfile.staticChallenge,
            fProfile.staticChallengeEcho, false);
        return;
    }
    if (password != kFederatedFirstPass)
        _SetState(ConnectionState::Connecting, "Signing in");
    _SendCredentials(user, password);
}


void Connection::_SendCredentials(const std::string& user, const std::string& password)
{
    _Send("username \"Auth\" " + ManagementQuote(user));
    _Send("password \"Auth\" " + ManagementQuote(password));
}


bool Connection::_Send(const std::string& command)
{
    if (fManagementFd < 0)
        return false;
    std::string line = command + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        ssize_t result = send(fManagementFd, line.data() + sent, line.size() - sent, kSendFlags);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        sent += result;
    }
    return true;
}


std::string Connection::_LogTail() const
{
    std::string content;
    if (fLogPath.empty() || !ReadFile(fLogPath, content))
        return "";
    // The last meaningful line, without OpenVPN's time stamp.
    size_t end = content.find_last_not_of("\r\n");
    while (end != std::string::npos) {
        size_t start = content.rfind('\n', end);
        start = start == std::string::npos ? 0 : start + 1;
        std::string line = content.substr(start, end - start + 1);
        if (line.size() > 20 && line[4] == '-' && line[10] == ' ')
            line = line.substr(20);
        if (line.find("Exiting due to fatal error") == std::string::npos
            && line.find("SIGTERM") == std::string::npos && !line.empty())
            return line;
        if (start == 0)
            break;
        end = content.find_last_not_of("\r\n", start - 1);
    }
    return "";
}


void Connection::_RemoveRuntime()
{
    if (!fConfigPath.empty())
        unlink(fConfigPath.c_str());
    if (!fLogPath.empty()) {
        // Keep the last log next to the profile's runtime directory for support.
        std::string kept = fSettings.runtimeDirectory + ".log";
        rename(fLogPath.c_str(), kept.c_str());
    }
    rmdir(fSettings.runtimeDirectory.c_str());
}

}  // namespace burrow
