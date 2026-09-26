// One VPN connection: runs Burrow's OpenVPN for a profile and drives it through
// the management interface, answering credential queries (including the AWS
// single sign-on exchange) and reporting state, traffic and log lines.
#pragma once
#include "Profile.h"
#include "Protocol.h"
#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

namespace burrow {

enum class ConnectionState {
    Disconnected,
    Connecting,
    SigningIn,       // waiting for the browser sign-in or for credentials
    Connected,
    Reconnecting,
    Disconnecting,
    Failed
};

const char* StateName(ConnectionState state);

struct ConnectionInfo {
    std::string serverHost;      // from the profile
    std::string serverAddress;   // the address actually used
    int port = 0;
    std::string proto;
    std::string device;          // tun/0
    std::string localAddress;    // address the VPN assigned
    PushedOptions pushed;
    time_t connectedSince = 0;
    uint64_t received = 0;
    uint64_t sent = 0;
};

// Callbacks run on the connection's own thread.
class ConnectionListener {
public:
    virtual ~ConnectionListener() {}
    virtual void ConnectionChanged(const std::string& id, ConnectionState state,
        const std::string& detail) = 0;
    virtual void ConnectionLog(const std::string& id, const std::string& line) = 0;
    virtual void ConnectionStats(const std::string& id, uint64_t received, uint64_t sent) = 0;
    // Open url in a browser; the identity provider posts back to 127.0.0.1:35001.
    virtual void ConnectionNeedsSignIn(const std::string& id, const std::string& url) = 0;
    // Ask for user name and password (retry after a rejection), or for the
    // response to a challenge when challenge is not empty.
    virtual void ConnectionNeedsCredentials(const std::string& id, const std::string& challenge,
        bool echo, bool retry) = 0;
};

struct ConnectionSettings {
    std::string id;
    std::string name;
    std::string profileText;
    std::string openvpnPath;
    std::string runtimeDirectory;   // private scratch directory, removed afterwards
};

class Connection {
public:
    Connection(const ConnectionSettings& settings, ConnectionListener* listener);
    ~Connection();

    void Start();
    // Asks OpenVPN to exit; the state reaches Disconnected asynchronously.
    void Stop();
    void SubmitSamlResponse(const std::string& samlResponse);
    // For a challenge only the response matters.
    void SubmitCredentials(const std::string& user, const std::string& password,
        const std::string& response);

    const std::string& Id() const { return fSettings.id; }
    const std::string& Name() const { return fSettings.name; }
    ConnectionState State() const;
    ConnectionInfo Info() const;
    bool Active() const;
    bool AwaitingSaml() const;
    std::string LastError() const;

private:
    void _Run();
    bool _Resolve(std::string& error);
    bool _Launch(int managementPort, std::string& error);
    void _Serve(int listenFd);
    void _Handle(const std::string& line);
    void _HandlePassword(const PasswordPrompt& prompt);
    void _AnswerAuth();
    void _SendCredentials(const std::string& user, const std::string& password);
    bool _Send(const std::string& command);
    void _SetState(ConnectionState state, const std::string& detail = std::string());
    void _Log(const std::string& line);
    void _Wake();
    std::string _LogTail() const;
    void _RemoveRuntime();

    ConnectionSettings fSettings;
    ConnectionListener* fListener;
    ProfileConfig fProfile;

    mutable std::mutex fLock;
    ConnectionState fState = ConnectionState::Disconnected;
    ConnectionInfo fInfo;
    std::string fLastError;

    std::thread fThread;
    std::atomic<bool> fStopRequested{false};
    int fWakePipe[2] = {-1, -1};
    int fManagementFd = -1;
    pid_t fPid = -1;
    std::string fConfigPath;
    std::string fLogPath;

    // Sign-in; guarded by fLock.
    bool fAuthQueried = false;         // OpenVPN waits for 'Auth' credentials
    bool fFirstPassSent = false;       // "ACS::35001" went out for this attempt
    std::string fChallengeState;       // CRV1 state id awaiting an answer
    std::string fSamlResponse;
    std::string fUser;
    std::string fPassword;
    std::string fResponse;
    bool fHaveCredentials = false;
    bool fHaveResponse = false;
    int fRejections = 0;
};

}  // namespace burrow
