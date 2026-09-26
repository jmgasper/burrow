#include "App.h"
#include "CredentialsWindow.h"
#include "LogWindow.h"
#include "MainWindow.h"
#include "Messages.h"
#include "NameWindow.h"
#include "core/ResolvConf.h"
#include <Alert.h>
#include <FilePanel.h>
#include <Invoker.h>
#include <FindDirectory.h>
#include <Notification.h>
#include <Path.h>
#include <PathFinder.h>
#include <Roster.h>
#include <StringList.h>
#include <Url.h>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace burrow {

const char* const kAppSignature = "application/x-vnd.Burrow";

namespace {

constexpr size_t kMaxLogLines = 4000;
constexpr uint32 kMsgRemoveConfirmed = 'rmcf';
constexpr uint32 kMsgLogWindowClosed = 'lwcl';
constexpr uint32 kMsgForceQuit = 'fqui';

std::string Timestamp()
{
    char buffer[32];
    time_t now = time(nullptr);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", localtime(&now));
    return buffer;
}

std::string Directory(directory_which which, const char* leaf)
{
    BPath path;
    if (find_directory(which, &path, true) != B_OK)
        path.SetTo("/tmp");
    path.Append(leaf);
    return path.Path();
}

}  // namespace


BurrowApp::BurrowApp()
    :
    BApplication(kAppSignature),
    fProfiles(Directory(B_USER_SETTINGS_DIRECTORY, "Burrow/profiles")),
    fSaml([](const std::string& response) {
        BMessage message(kMsgSamlResponse);
        message.AddString("response", response.c_str());
        be_app->PostMessage(&message);
    }, kSamlPort)
{
    fSettingsDirectory = Directory(B_USER_SETTINGS_DIRECTORY, "Burrow");
    fRuntimeDirectory = Directory(B_SYSTEM_TEMP_DIRECTORY, "Burrow");
    fOpenVPNPath = _FindOpenVPN();
}


BurrowApp::~BurrowApp()
{
    // Connection destructors stop OpenVPN and join their threads.
    std::map<std::string, std::unique_ptr<Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(fLock);
        connections.swap(fConnections);
    }
    connections.clear();
    fSaml.Stop();
    RestoreDns(kResolvConfPath);
    delete fOpenPanel;
}


void BurrowApp::ReadyToRun()
{
    {
        std::lock_guard<std::mutex> lock(fLock);
        fProfiles.Load();
    }
    mkdir(fRuntimeDirectory.c_str(), 0700);
    // A previous run that crashed may have left its name servers behind.
    RestoreDns(kResolvConfPath);

    MainWindow* window = new MainWindow();
    fWindow = BMessenger(window);
    window->Show();
    fReady = true;

    std::vector<std::string> args;
    args.swap(fPendingArgs);
    if (!args.empty()) {
        std::vector<char*> argv;
        argv.push_back((char*)"Burrow");
        for (std::string& arg : args)
            argv.push_back(&arg[0]);
        ArgvReceived(argv.size(), argv.data());
    }
}


bool BurrowApp::QuitRequested()
{
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(fLock);
        for (auto& entry : fConnections)
            active |= entry.second->Active();
    }
    if (active) {
        // Disconnect first; _StateChanged quits once everything is down.
        if (!fQuitting) {
            fQuitting = true;
            std::lock_guard<std::mutex> lock(fLock);
            for (auto& entry : fConnections)
                entry.second->Stop();
            BMessage force(kMsgForceQuit);
            PostMessage(&force);
        }
        return false;
    }
    fSaml.Stop();
    RestoreDns(kResolvConfPath);
    return BApplication::QuitRequested();
}


void BurrowApp::MessageReceived(BMessage* message)
{
    const char* idText = nullptr;
    message->FindString("id", &idText);
    std::string id = idText != nullptr ? idText : "";

    switch (message->what) {
        case kMsgConnect:
            _Connect(id);
            break;
        case kMsgDisconnect:
            _Disconnect(id);
            break;
        case kMsgToggle: {
            ProfileStatus status;
            if (!Status(id, status))
                break;
            if (status.state == ConnectionState::Disconnected
                || status.state == ConnectionState::Failed)
                _Connect(id);
            else
                _Disconnect(id);
            break;
        }
        case kMsgShowImportPanel:
            if (fOpenPanel == nullptr) {
                BMessenger target(this);
                fOpenPanel = new BFilePanel(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, true);
                fOpenPanel->Window()->SetTitle("Burrow: Import VPN profile");
            }
            fOpenPanel->Show();
            break;
        case kMsgImportNamed: {
            const char* text = message->GetString("text", "");
            const char* name = message->GetString("name", "");
            const char* file = message->GetString("file", "");
            _Import(text, name, file);
            break;
        }
        case kMsgRenameProfile: {
            const StoredProfile* profile = fProfiles.Find(id);
            if (profile == nullptr)
                break;
            BMessage renamed(kMsgRenamed);
            renamed.AddString("id", id.c_str());
            (new NameWindow("Rename profile", "Name:", profile->name.c_str(), "Rename",
                renamed, BMessenger(this)))->Show();
            break;
        }
        case kMsgRenamed: {
            {
                std::lock_guard<std::mutex> lock(fLock);
                fProfiles.Rename(id, message->GetString("name", ""));
            }
            BMessage changed(kMsgProfilesChanged);
            changed.AddString("select", id.c_str());
            _Broadcast(&changed);
            break;
        }
        case kMsgRemoveProfile: {
            const StoredProfile* profile = fProfiles.Find(id);
            if (profile == nullptr)
                break;
            BString text;
            text.SetToFormat("Remove the profile \"%s\"? Burrow deletes its copy of the "
                "configuration; the file you imported stays where it is.", profile->name.c_str());
            BAlert* alert = new BAlert("Remove profile", text.String(), "Cancel", "Remove",
                nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            BMessage* confirmed = new BMessage(kMsgRemoveConfirmed);
            confirmed->AddString("id", id.c_str());
            alert->Go(new BInvoker(confirmed, this));
            break;
        }
        case kMsgRemoveConfirmed:
            if (message->GetInt32("which", 0) == 1)
                _Remove(id);
            break;
        case kMsgShowLog:
            _ShowLog(id);
            break;
        case kMsgLogWindowClosed:
            fLogWindows.erase(id);
            break;
        case kMsgShowWindow:
            if (fWindow.IsValid())
                fWindow.SendMessage(kMsgShowWindow);
            else {
                MainWindow* window = new MainWindow();
                fWindow = BMessenger(window);
                window->Show();
            }
            break;

        case kMsgConnectionChanged:
            _StateChanged(id, (ConnectionState)message->GetInt32("state", 0),
                message->GetString("detail", ""));
            break;
        case kMsgConnectionLog: {
            std::string line = Timestamp() + "  " + message->GetString("line", "");
            {
                std::lock_guard<std::mutex> lock(fLock);
                std::deque<std::string>& log = fLogs[id];
                log.push_back(line);
                while (log.size() > kMaxLogLines)
                    log.pop_front();
            }
            auto window = fLogWindows.find(id);
            if (window != fLogWindows.end()) {
                BMessage forward(kMsgLogLine);
                forward.AddString("line", line.c_str());
                window->second->PostMessage(&forward);
            }
            break;
        }
        case kMsgConnectionStats: {
            BMessage updated(kMsgConnectionUpdated);
            updated.AddString("id", id.c_str());
            _Broadcast(&updated);
            break;
        }
        case kMsgNeedsSignIn:
        case kMsgReopenSignIn: {
            std::string url = message->GetString("url", "");
            if (message->what == kMsgReopenSignIn) {
                std::lock_guard<std::mutex> lock(fLock);
                url = fSignInUrls.count(id) ? fSignInUrls[id] : "";
            }
            if (url.empty())
                break;
            std::string error;
            if (!fSaml.Start(error)) {
                _Notify("Cannot sign in", error, true);
                ConnectionLog(id, error);
                _Disconnect(id);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(fLock);
                fSignInUrls[id] = url;
            }
            fSamlOwner = id;
            BUrl target(url.c_str(), false);
            if (!target.IsValid() || target.OpenWithPreferredApplication(false) != B_OK) {
                BString text;
                text.SetToFormat("Burrow could not open a web browser. Open this address "
                    "to sign in:\n\n%s", url.c_str());
                (new BAlert("Sign in", text.String(), "OK"))->Go(nullptr);
            }
            ConnectionLog(id, "Opened the sign-in page in the web browser");
            break;
        }
        case kMsgSamlResponse: {
            std::string response = message->GetString("response", "");
            Connection* owner = nullptr;
            {
                std::lock_guard<std::mutex> lock(fLock);
                auto found = fConnections.find(fSamlOwner);
                if (found != fConnections.end() && found->second->AwaitingSaml())
                    owner = found->second.get();
            }
            if (owner == nullptr) {
                fprintf(stderr, "Burrow: ignored a sign-in response nobody waited for\n");
                break;
            }
            ConnectionLog(fSamlOwner, "Received the single sign-on response ("
                + std::to_string(response.size()) + " bytes)");
            owner->SubmitSamlResponse(response);
            fSamlOwner.clear();
            _UpdateSamlListener();
            break;
        }
        case kMsgNeedsCredentials: {
            const StoredProfile* profile = fProfiles.Find(id);
            if (profile == nullptr)
                break;
            (new CredentialsWindow(id, profile->name, message->GetString("challenge", ""),
                message->GetBool("echo", false), message->GetBool("retry", false),
                BMessenger(this)))->Show();
            break;
        }
        case kMsgCredentialsEntered: {
            std::lock_guard<std::mutex> lock(fLock);
            auto found = fConnections.find(id);
            if (found != fConnections.end())
                found->second->SubmitCredentials(message->GetString("user", ""),
                    message->GetString("password", ""), message->GetString("response", ""));
            break;
        }
        case kMsgCredentialsCancelled:
            _Disconnect(id);
            break;
        case kMsgForceQuit: {
            // Give OpenVPN time to remove its routes, then quit regardless.
            static time_t sStarted = 0;
            if (sStarted == 0)
                sStarted = time(nullptr);
            if (time(nullptr) - sStarted > 12) {
                std::lock_guard<std::mutex> lock(fLock);
                fConnections.clear();
            }
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(fLock);
                for (auto& entry : fConnections)
                    active |= entry.second->Active();
            }
            if (active) {
                snooze(250000);
                PostMessage(kMsgForceQuit);
            } else
                PostMessage(B_QUIT_REQUESTED);
            break;
        }
        default:
            BApplication::MessageReceived(message);
    }
}


void BurrowApp::RefsReceived(BMessage* message)
{
    entry_ref ref;
    for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
        _ImportFile(ref);
}


void BurrowApp::ArgvReceived(int32 argc, char** argv)
{
    if (!fReady) {
        for (int32 i = 1; i < argc; i++)
            fPendingArgs.push_back(argv[i]);
        return;
    }
    for (int32 i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--connect" || arg == "--disconnect") && i + 1 < argc) {
            std::string name = argv[++i];
            const StoredProfile* profile = _ProfileByName(name);
            if (profile == nullptr) {
                fprintf(stderr, "Burrow: no profile named \"%s\"\n", name.c_str());
                continue;
            }
            if (arg == "--connect")
                _Connect(profile->id);
            else
                _Disconnect(profile->id);
        } else if (arg == "--disconnect-all") {
            std::lock_guard<std::mutex> lock(fLock);
            for (auto& entry : fConnections)
                entry.second->Stop();
        } else if (arg[0] != '-') {
            entry_ref ref;
            if (get_ref_for_path(arg.c_str(), &ref) == B_OK)
                _ImportFile(ref);
        }
    }
}


void BurrowApp::AboutRequested()
{
    BAlert* about = new BAlert("About Burrow", "Burrow 0.1.0 alpha\n\n"
        "A VPN client for AWS Client VPN and OpenVPN servers, with AWS single sign-on "
        "through your web browser.\n\n"
        "Burrow runs OpenVPN 2.6.13 (GNU GPL v2) with the AWS Client VPN patch and Haiku "
        "tunnel fixes; the patches are in Burrow's documentation folder.\n\n"
        "\xc2\xa9 2026 Burrow contributors", "OK");
    about->Go(nullptr);
}


void BurrowApp::ConnectionChanged(const std::string& id, ConnectionState state,
    const std::string& detail)
{
    BMessage message(kMsgConnectionChanged);
    message.AddString("id", id.c_str());
    message.AddInt32("state", (int32)state);
    message.AddString("detail", detail.c_str());
    PostMessage(&message);
}


void BurrowApp::ConnectionLog(const std::string& id, const std::string& line)
{
    BMessage message(kMsgConnectionLog);
    message.AddString("id", id.c_str());
    message.AddString("line", line.c_str());
    PostMessage(&message);
}


void BurrowApp::ConnectionStats(const std::string& id, uint64_t, uint64_t)
{
    BMessage message(kMsgConnectionStats);
    message.AddString("id", id.c_str());
    PostMessage(&message);
}


void BurrowApp::ConnectionNeedsSignIn(const std::string& id, const std::string& url)
{
    BMessage message(kMsgNeedsSignIn);
    message.AddString("id", id.c_str());
    message.AddString("url", url.c_str());
    PostMessage(&message);
}


void BurrowApp::ConnectionNeedsCredentials(const std::string& id, const std::string& challenge,
    bool echo, bool retry)
{
    BMessage message(kMsgNeedsCredentials);
    message.AddString("id", id.c_str());
    message.AddString("challenge", challenge.c_str());
    message.AddBool("echo", echo);
    message.AddBool("retry", retry);
    PostMessage(&message);
}


std::vector<ProfileStatus> BurrowApp::Statuses() const
{
    std::vector<ProfileStatus> result;
    std::lock_guard<std::mutex> lock(fLock);
    for (const StoredProfile& profile : fProfiles.Profiles()) {
        ProfileStatus status;
        status.id = profile.id;
        status.name = profile.name;
        status.config = profile.config;
        auto connection = fConnections.find(profile.id);
        if (connection != fConnections.end()) {
            status.state = connection->second->State();
            status.info = connection->second->Info();
            status.signInPending = connection->second->AwaitingSaml();
        }
        auto detail = fDetails.find(profile.id);
        if (detail != fDetails.end())
            status.detail = detail->second;
        result.push_back(status);
    }
    return result;
}


bool BurrowApp::Status(const std::string& id, ProfileStatus& status) const
{
    for (const ProfileStatus& candidate : Statuses()) {
        if (candidate.id == id) {
            status = candidate;
            return true;
        }
    }
    return false;
}


std::vector<std::string> BurrowApp::Log(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(fLock);
    auto found = fLogs.find(id);
    if (found == fLogs.end())
        return {};
    return std::vector<std::string>(found->second.begin(), found->second.end());
}


void BurrowApp::_Connect(const std::string& id)
{
    const StoredProfile* profile = fProfiles.Find(id);
    if (profile == nullptr)
        return;
    if (fOpenVPNPath.empty())
        fOpenVPNPath = _FindOpenVPN();
    if (fOpenVPNPath.empty()) {
        (new BAlert("Burrow", "Burrow's OpenVPN (burrow-openvpn) is missing. Reinstall the "
            "Burrow package.", "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go(nullptr);
        return;
    }
    Connection* connection = nullptr;
    {
        std::lock_guard<std::mutex> lock(fLock);
        auto found = fConnections.find(id);
        if (found != fConnections.end() && found->second->Active())
            return;
        ConnectionSettings settings;
        settings.id = id;
        settings.name = profile->name;
        settings.profileText = profile->text;
        settings.openvpnPath = fOpenVPNPath;
        settings.runtimeDirectory = fRuntimeDirectory + "/" + id;
        fConnections[id] = std::make_unique<Connection>(settings, this);
        connection = fConnections[id].get();
        fSignInUrls.erase(id);
        std::deque<std::string>& log = fLogs[id];
        log.push_back("");
        log.push_back(Timestamp() + "  Connecting to " + profile->name);
    }
    for (const std::string& directive : profile->config.dropped)
        ConnectionLog(id, "Ignoring the directive \"" + directive + "\", which Burrow handles "
            "itself or does not support on Haiku");
    connection->Start();
}


void BurrowApp::_Disconnect(const std::string& id)
{
    std::lock_guard<std::mutex> lock(fLock);
    auto found = fConnections.find(id);
    if (found != fConnections.end())
        found->second->Stop();
}


void BurrowApp::_ImportFile(const entry_ref& ref)
{
    BPath path(&ref);
    std::string text;
    if (path.InitCheck() != B_OK || !ReadFile(path.Path(), text)) {
        _Notify("Cannot import", std::string("Cannot read ") + ref.name, true);
        return;
    }
    std::string error;
    if (!ProfileStore::Validate(text, error)) {
        BString message;
        message.SetToFormat("\"%s\" is not a VPN profile Burrow can use.\n\n%s", ref.name,
            error.c_str());
        (new BAlert("Import profile", message.String(), "OK", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go(nullptr);
        return;
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(fLock);
        name = fProfiles.UniqueName(SuggestProfileName(ParseProfile(text), ref.name));
    }
    BMessage named(kMsgImportNamed);
    named.AddString("text", text.c_str());
    named.AddString("file", ref.name);
    (new NameWindow("Import VPN profile", "Profile name:", name.c_str(), "Import", named,
        BMessenger(this)))->Show();
}


void BurrowApp::_Import(const std::string& text, const std::string& name, const std::string& file)
{
    std::string id;
    std::string error;
    bool imported;
    {
        std::lock_guard<std::mutex> lock(fLock);
        imported = fProfiles.Import(text, name, id, error);
    }
    if (!imported) {
        (new BAlert("Import profile", error.c_str(), "OK", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go(nullptr);
        return;
    }
    BMessage changed(kMsgProfilesChanged);
    changed.AddString("select", id.c_str());
    _Broadcast(&changed);
}


void BurrowApp::_Remove(const std::string& id)
{
    std::unique_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(fLock);
        auto found = fConnections.find(id);
        if (found != fConnections.end()) {
            connection = std::move(found->second);
            fConnections.erase(found);
        }
        fProfiles.Remove(id);
        fLogs.erase(id);
        fDetails.erase(id);
    }
    connection.reset();   // stops OpenVPN and waits for it
    auto window = fLogWindows.find(id);
    if (window != fLogWindows.end())
        window->second->PostMessage(B_QUIT_REQUESTED);
    _UpdateDns();
    BMessage changed(kMsgProfilesChanged);
    _Broadcast(&changed);
}


void BurrowApp::_StateChanged(const std::string& id, ConnectionState state,
    const std::string& detail)
{
    static std::map<std::string, ConnectionState> sPrevious;
    ConnectionState previous = sPrevious.count(id) ? sPrevious[id] : ConnectionState::Disconnected;
    sPrevious[id] = state;
    {
        std::lock_guard<std::mutex> lock(fLock);
        fDetails[id] = detail;
    }
    if (state != previous || !detail.empty()) {
        std::string line = std::string("Status: ") + StateName(state);
        if (!detail.empty())
            line += " (" + detail + ")";
        ConnectionLog(id, line);
    }

    ProfileStatus status;
    Status(id, status);
    if (state == ConnectionState::Connected && previous != ConnectionState::Connected) {
        _Notify("Connected to " + status.name, "Your address in the VPN is "
            + status.info.localAddress + ".");
    } else if (state == ConnectionState::Failed) {
        _Notify("Cannot connect to " + status.name, detail, true);
    } else if (state == ConnectionState::Disconnected
        && (previous == ConnectionState::Disconnecting || previous == ConnectionState::Connected
            || previous == ConnectionState::Reconnecting)) {
        _Notify("Disconnected from " + status.name, "The VPN connection has ended.");
    }

    _UpdateDns();
    _UpdateSamlListener();

    BMessage updated(kMsgConnectionUpdated);
    updated.AddString("id", id.c_str());
    _Broadcast(&updated);

    if (fQuitting && (state == ConnectionState::Disconnected || state == ConnectionState::Failed))
        PostMessage(B_QUIT_REQUESTED);
}


void BurrowApp::_UpdateDns()
{
    std::vector<ProfileStatus> statuses = Statuses();
    auto usable = [](const ProfileStatus& status) {
        return (status.state == ConnectionState::Connected
                || status.state == ConnectionState::Reconnecting)
            && (!status.info.pushed.dnsServers.empty() || !status.info.pushed.domains.empty());
    };
    const ProfileStatus* owner = nullptr;
    for (const ProfileStatus& status : statuses) {
        if (status.id == fDnsOwner && usable(status))
            owner = &status;
    }
    if (owner != nullptr)
        return;   // unchanged
    for (const ProfileStatus& status : statuses) {
        if (owner == nullptr && usable(status))
            owner = &status;
    }
    if (owner == nullptr) {
        if (!fDnsOwner.empty()) {
            std::string previous = fDnsOwner;
            fDnsOwner.clear();
            if (RestoreDns(kResolvConfPath))
                ConnectionLog(previous, "DNS: restored the previous name servers");
            else
                ConnectionLog(previous, "DNS: could not restore resolv.conf");
        }
        return;
    }
    fDnsOwner = owner->id;
    const PushedOptions& pushed = owner->info.pushed;
    std::string servers;
    for (const std::string& server : pushed.dnsServers)
        servers += (servers.empty() ? "" : ", ") + server;
    for (const std::string& domain : pushed.domains)
        servers += " (" + domain + ")";
    if (ApplyVpnDns(kResolvConfPath, pushed.dnsServers, pushed.domains, owner->name))
        ConnectionLog(owner->id, "DNS: using " + servers + "; programs started before "
            "connecting keep their earlier name servers until restarted");
    else
        ConnectionLog(owner->id, "DNS: could not update resolv.conf");
}


void BurrowApp::_UpdateSamlListener()
{
    if (!fSaml.Running())
        return;
    bool waiting = false;
    {
        std::lock_guard<std::mutex> lock(fLock);
        for (auto& entry : fConnections)
            waiting |= entry.second->AwaitingSaml();
    }
    if (!waiting) {
        fSaml.Stop();
        fSamlOwner.clear();
    }
}


void BurrowApp::_ShowLog(const std::string& id)
{
    auto found = fLogWindows.find(id);
    if (found != fLogWindows.end()) {
        found->second->Activate();
        return;
    }
    const StoredProfile* profile = fProfiles.Find(id);
    if (profile == nullptr)
        return;
    BMessage closed(kMsgLogWindowClosed);
    closed.AddString("id", id.c_str());
    LogWindow* window = new LogWindow(profile->name, Log(id), closed, BMessenger(this));
    fLogWindows[id] = window;
    window->Show();
}


void BurrowApp::_Notify(const std::string& title, const std::string& text, bool error)
{
    BNotification notification(error ? B_ERROR_NOTIFICATION : B_INFORMATION_NOTIFICATION);
    notification.SetGroup("Burrow");
    notification.SetTitle(title.c_str());
    notification.SetContent(text.c_str());
    notification.Send();
}


std::string BurrowApp::_FindOpenVPN() const
{
    if (const char* override = getenv("BURROW_OPENVPN")) {
        if (access(override, X_OK) == 0)
            return override;
    }
    BStringList paths;
    if (BPathFinder::FindPaths(B_FIND_PATH_BIN_DIRECTORY, "burrow-openvpn",
            B_FIND_PATH_EXISTING_ONLY, paths) == B_OK) {
        for (int32 i = 0; i < paths.CountStrings(); i++) {
            if (access(paths.StringAt(i).String(), X_OK) == 0)
                return paths.StringAt(i).String();
        }
    }
    // Development builds: build-haiku/Burrow next to build-haiku/openvpn/.
    app_info info;
    if (GetAppInfo(&info) == B_OK) {
        BPath path(&info.ref);
        BPath directory;
        if (path.GetParent(&directory) == B_OK) {
            for (const char* leaf : {"openvpn/burrow-openvpn", "burrow-openvpn"}) {
                BPath candidate(directory.Path(), leaf);
                if (access(candidate.Path(), X_OK) == 0)
                    return candidate.Path();
            }
        }
    }
    return "";
}


const StoredProfile* BurrowApp::_ProfileByName(const std::string& name) const
{
    for (const StoredProfile& profile : fProfiles.Profiles()) {
        if (profile.name == name || profile.id == name)
            return &profile;
    }
    return nullptr;
}


void BurrowApp::_Broadcast(BMessage* message)
{
    fWindow.SendMessage(message);
}

}  // namespace burrow
