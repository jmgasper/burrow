// The Burrow application: owns the profiles and running connections, the
// single sign-on listener, the DNS changes and the windows. Connection events
// arrive on connection threads and are turned into messages here.
#pragma once
#include "core/Connection.h"
#include "core/ProfileStore.h"
#include "core/SamlListener.h"
#include <Application.h>
#include <Entry.h>
#include <Messenger.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BFilePanel;

namespace burrow {

class MainWindow;
class LogWindow;

// What the windows show about one profile.
struct ProfileStatus {
    std::string id;
    std::string name;
    ProfileConfig config;
    ConnectionState state = ConnectionState::Disconnected;
    std::string detail;
    ConnectionInfo info;
    bool signInPending = false;   // waiting for the browser sign-in
};

class BurrowApp : public BApplication, public ConnectionListener {
public:
    BurrowApp();
    ~BurrowApp() override;

    void ReadyToRun() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage* message) override;
    void RefsReceived(BMessage* message) override;
    void ArgvReceived(int32 argc, char** argv) override;
    void AboutRequested() override;

    // ConnectionListener; these run on connection threads.
    void ConnectionChanged(const std::string& id, ConnectionState state,
        const std::string& detail) override;
    void ConnectionLog(const std::string& id, const std::string& line) override;
    void ConnectionStats(const std::string& id, uint64_t received, uint64_t sent) override;
    void ConnectionNeedsSignIn(const std::string& id, const std::string& url) override;
    void ConnectionNeedsCredentials(const std::string& id, const std::string& challenge,
        bool echo, bool retry) override;

    // Thread-safe snapshots for the windows.
    std::vector<ProfileStatus> Statuses() const;
    bool Status(const std::string& id, ProfileStatus& status) const;
    std::vector<std::string> Log(const std::string& id) const;
    std::string OpenVPNPath() const { return fOpenVPNPath; }
    bool Quitting() const { return fQuitRequested; }
    bool HasActiveConnection() const;
    bool InDeskbar() const;
    // Shows or removes Burrow's Deskbar icon and remembers the choice.
    void SetInDeskbar(bool show);

    static BurrowApp* Instance() { return (BurrowApp*)be_app; }

private:
    void _Connect(const std::string& id);
    void _Disconnect(const std::string& id);
    void _ImportFile(const entry_ref& ref);
    void _Import(const std::string& text, const std::string& name, const std::string& file);
    void _Remove(const std::string& id);
    void _StateChanged(const std::string& id, ConnectionState state, const std::string& detail);
    void _UpdateDns();
    void _UpdateSamlListener();
    void _ShowLog(const std::string& id);
    void _Notify(const std::string& title, const std::string& text, bool error = false);
    std::string _FindOpenVPN() const;
    const StoredProfile* _ProfileByName(const std::string& name) const;
    void _Broadcast(BMessage* message);

    mutable std::mutex fLock;   // guards the maps below for the window threads
    ProfileStore fProfiles;
    std::map<std::string, std::unique_ptr<Connection>> fConnections;
    std::map<std::string, std::string> fDetails;
    std::map<std::string, std::deque<std::string>> fLogs;
    std::map<std::string, std::string> fSignInUrls;

    std::map<std::string, LogWindow*> fLogWindows;
    SamlListener fSaml;
    std::string fSamlOwner;
    std::string fDnsOwner;
    BMessenger fWindow;
    bool fReady = false;
    BFilePanel* fOpenPanel = nullptr;
    std::string fSettingsDirectory;
    std::string fRuntimeDirectory;
    std::string fOpenVPNPath;
    std::vector<std::string> fPendingArgs;
    bool fQuitting = false;
    bool fQuitRequested = false;
    void _LoadSettings();
    void _SaveSettings();
    bool fDeskbarWanted = true;
};

}  // namespace burrow
