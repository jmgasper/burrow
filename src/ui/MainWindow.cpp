#include "MainWindow.h"
#include "Messages.h"
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <CardLayout.h>
#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Resources.h>
#include <Screen.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextView.h>
#include <arpa/inet.h>
#include <ctime>

namespace burrow {

namespace {

std::string FormatBytes(uint64_t bytes)
{
    char buffer[32];
    if (bytes < 1024)
        snprintf(buffer, sizeof(buffer), "%llu B", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buffer, sizeof(buffer), "%.1f KiB", bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024)
        snprintf(buffer, sizeof(buffer), "%.1f MiB", bytes / (1024.0 * 1024));
    else
        snprintf(buffer, sizeof(buffer), "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    return buffer;
}

std::string FormatDuration(time_t seconds)
{
    char buffer[32];
    if (seconds >= 86400)
        snprintf(buffer, sizeof(buffer), "%ldd %02ld:%02ld:%02ld", (long)(seconds / 86400),
            (long)(seconds / 3600 % 24), (long)(seconds / 60 % 60), (long)(seconds % 60));
    else
        snprintf(buffer, sizeof(buffer), "%02ld:%02ld:%02ld", (long)(seconds / 3600),
            (long)(seconds / 60 % 60), (long)(seconds % 60));
    return buffer;
}

// "10.100.0.0/255.255.0.0" -> "10.100.0.0/16"
std::string ShortRoute(const std::string& route)
{
    size_t slash = route.find('/');
    if (slash == std::string::npos)
        return route;
    in_addr mask;
    if (inet_pton(AF_INET, route.c_str() + slash + 1, &mask) != 1)
        return route;
    uint32 bits = ntohl(mask.s_addr);
    int prefix = 0;
    while (bits & 0x80000000) {
        prefix++;
        bits <<= 1;
    }
    return route.substr(0, slash) + "/" + std::to_string(prefix);
}

const char* kDash = "\xe2\x80\x94";   // em dash
constexpr uint32 kMsgImportMenu = 'imnu';

}  // namespace


rgb_color StateColor(ConnectionState state)
{
    switch (state) {
        case ConnectionState::Connected:
            return make_color(46, 157, 91);
        case ConnectionState::Connecting:
        case ConnectionState::SigningIn:
        case ConnectionState::Reconnecting:
        case ConnectionState::Disconnecting:
            return make_color(222, 142, 24);
        case ConnectionState::Failed:
            return make_color(211, 58, 44);
        case ConnectionState::Disconnected:
            break;
    }
    return tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_3_TINT);
}


std::string StateSummary(const ProfileStatus& status)
{
    switch (status.state) {
        case ConnectionState::Connected:
            return "Connected \xc2\xb7 " + status.info.localAddress;
        case ConnectionState::SigningIn:
            return status.signInPending ? "Waiting for browser sign-in" : "Signing in";
        case ConnectionState::Connecting:
            return status.detail.empty() ? "Connecting" : status.detail;
        case ConnectionState::Reconnecting:
            return "Reconnecting";
        case ConnectionState::Disconnecting:
            return "Disconnecting";
        case ConnectionState::Failed:
            return "Connection failed";
        case ConnectionState::Disconnected:
            break;
    }
    return "Not connected";
}


BBitmap* LoadAppIcon(float size)
{
    BResources* resources = BApplication::AppResources();
    if (resources == nullptr)
        return nullptr;
    size_t length = 0;
    const void* data = resources->LoadResource(B_VECTOR_ICON_TYPE, "BEOS:ICON", &length);
    if (data == nullptr)
        return nullptr;
    BBitmap* bitmap = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
    if (BIconUtils::GetVectorIcon((const uint8*)data, length, bitmap) != B_OK) {
        delete bitmap;
        return nullptr;
    }
    return bitmap;
}


// #pragma mark - ProfileItem


ProfileItem::ProfileItem(const ProfileStatus& status)
    :
    fStatus(status)
{
}


void ProfileItem::Update(BView* owner, const BFont* font)
{
    BListItem::Update(owner, font);
    font_height height;
    font->GetHeight(&height);
    float line = ceilf(height.ascent + height.descent + height.leading);
    SetHeight(ceilf(line * 2.6f + 6));
}


void ProfileItem::DrawItem(BView* owner, BRect frame, bool complete)
{
    rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR
        : B_LIST_BACKGROUND_COLOR);
    rgb_color text = ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR
        : B_LIST_ITEM_TEXT_COLOR);
    owner->SetLowColor(background);
    owner->FillRect(frame, B_SOLID_LOW);

    font_height height;
    BFont bold(be_bold_font);
    bold.GetHeight(&height);
    float line = ceilf(height.ascent + height.descent);
    float padding = ceilf(line * 0.55f);

    // Status dot
    float radius = ceilf(line * 0.33f);
    BPoint center(frame.left + padding + radius, frame.top + frame.Height() / 2);
    owner->SetDrawingMode(B_OP_ALPHA);
    owner->SetHighColor(StateColor(fStatus.state));
    owner->FillEllipse(center, radius, radius);
    owner->SetDrawingMode(B_OP_COPY);

    float left = center.x + radius + padding;
    float top = frame.top + (frame.Height() - line * 2.2f) / 2;
    owner->SetFont(&bold);
    owner->SetHighColor(text);
    BString name(fStatus.name.c_str());
    owner->TruncateString(&name, B_TRUNCATE_END, frame.right - left - padding);
    owner->DrawString(name.String(), BPoint(left, top + height.ascent));

    BFont small(be_plain_font);
    small.SetSize(be_plain_font->Size() * 0.88f);
    owner->SetFont(&small);
    owner->SetHighColor(mix_color(text, background, 150));
    BString summary(StateSummary(fStatus).c_str());
    owner->TruncateString(&summary, B_TRUNCATE_END, frame.right - left - padding);
    owner->DrawString(summary.String(), BPoint(left, top + line * 1.15f + height.ascent));
    owner->SetFont(be_plain_font);
}


// #pragma mark - IconView


IconView::IconView(float size)
    :
    BView("icon", B_WILL_DRAW),
    fIcon(LoadAppIcon(size))
{
    SetExplicitSize(BSize(size - 1, size - 1));
    SetExplicitAlignment(BAlignment(B_ALIGN_LEFT, B_ALIGN_VERTICAL_CENTER));
    SetFlags(Flags() | B_TRANSPARENT_BACKGROUND);
    SetViewColor(B_TRANSPARENT_COLOR);
}


IconView::~IconView()
{
    delete fIcon;
}


void IconView::Draw(BRect)
{
    if (fIcon == nullptr)
        return;
    SetDrawingMode(B_OP_ALPHA);
    SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
    DrawBitmap(fIcon, BPoint(0, 0));
}


// #pragma mark - MainWindow


MainWindow::MainWindow()
    :
    BWindow(BRect(0, 0, 800, 480), "Burrow", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS
        | B_QUIT_ON_WINDOW_CLOSE),
    fTicker(nullptr)
{
    BMenuBar* menuBar = new BMenuBar("menu");
    BMenu* profileMenu = new BMenu("Profile");
    profileMenu->AddItem(new BMenuItem("Import profile" B_UTF8_ELLIPSIS,
        new BMessage(kMsgImportMenu), 'O'));
    profileMenu->AddItem(fRenameItem = new BMenuItem("Rename" B_UTF8_ELLIPSIS,
        new BMessage(kMsgRenameProfile), 'R'));
    profileMenu->AddItem(fRemoveItem = new BMenuItem("Remove" B_UTF8_ELLIPSIS,
        new BMessage(kMsgRemoveProfile)));
    profileMenu->AddSeparatorItem();
    BMenuItem* about = new BMenuItem("About Burrow", new BMessage(B_ABOUT_REQUESTED));
    about->SetTarget(be_app);
    profileMenu->AddItem(about);
    profileMenu->AddSeparatorItem();
    BMenuItem* quit = new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q');
    quit->SetTarget(be_app);
    profileMenu->AddItem(quit);
    menuBar->AddItem(profileMenu);

    BMenu* connectionMenu = new BMenu("Connection");
    connectionMenu->AddItem(fConnectItem = new BMenuItem("Connect", new BMessage(kMsgConnect),
        'K'));
    connectionMenu->AddItem(fDisconnectItem = new BMenuItem("Disconnect",
        new BMessage(kMsgDisconnect), 'D'));
    connectionMenu->AddSeparatorItem();
    connectionMenu->AddItem(fLogItem = new BMenuItem("Show log", new BMessage(kMsgShowLog), 'L'));
    menuBar->AddItem(connectionMenu);

    fList = new BListView("profiles", B_SINGLE_SELECTION_LIST);
    fList->SetSelectionMessage(new BMessage(kMsgSelectionChanged));
    fList->SetInvocationMessage(new BMessage(kMsgProfileInvoked));
    BScrollView* listScroll = new BScrollView("profilesScroll", fList, 0, false, true,
        B_NO_BORDER);
    float em = be_plain_font->StringWidth("M");
    listScroll->SetExplicitSize(BSize(em * 16, B_SIZE_UNSET));

    BView* cards = new BView("cards", 0);
    fCards = new BCardLayout();
    cards->SetLayout(fCards);
    cards->AdoptSystemColors();
    fCards->AddView(_BuildEmpty());
    fCards->AddView(_BuildDetails());
    cards->SetExplicitMinSize(BSize(em * 32, B_SIZE_UNSET));
    cards->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menuBar)
        .AddGroup(B_HORIZONTAL, 0, 1.0f)
            .Add(listScroll)
            .Add(new BSeparatorView(B_VERTICAL))
            .Add(cards, 1.0f)
        .End();

    _Reload(std::string());
    fTicker = new BMessageRunner(BMessenger(this), new BMessage(kMsgTick), 1000000);
}


MainWindow::~MainWindow()
{
    delete fTicker;
}


BView* MainWindow::_BuildEmpty()
{
    BView* view = new BView("empty", 0);
    view->AdoptSystemColors();
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    BStringView* heading = new BStringView("heading", "No VPN profiles yet");
    BFont font(be_bold_font);
    font.SetSize(be_bold_font->Size() * 1.3f);
    heading->SetFont(&font);
    BTextView* text = new BTextView("text");
    text->SetText("Import the .ovpn profile your administrator gave you. In the AWS console "
        "it is the Client VPN endpoint's \"client configuration\" download. You can also drop "
        "the file on this window.");
    text->MakeEditable(false);
    text->MakeSelectable(false);
    text->SetWordWrap(true);
    text->AdoptSystemColors();
    text->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 22, B_SIZE_UNSET));
    BButton* import = new BButton("Import profile" B_UTF8_ELLIPSIS, new BMessage(kMsgImportMenu));
    BLayoutBuilder::Group<>(view, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGlue()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(new IconView(ceilf(be_plain_font->Size() * 64 / 12)))
            .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(heading)
            .AddGlue()
        .End()
        .Add(text)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(import)
            .AddGlue()
        .End()
        .AddGlue(2.0f);
    return view;
}


BView* MainWindow::_BuildDetails()
{
    BView* view = new BView("details", 0);
    view->AdoptSystemColors();
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    fName = new BStringView("name", "");
    BFont big(be_bold_font);
    big.SetSize(be_bold_font->Size() * 1.45f);
    fName->SetFont(&big);
    fName->SetExplicitMinSize(BSize(1, B_SIZE_UNSET));
    fName->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fName->SetTruncation(B_TRUNCATE_END);
    fState = new BStringView("state", "");
    BFont stateFont(be_bold_font);
    fState->SetFont(&stateFont);
    fState->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    fMessage = new BTextView("message");
    fMessage->MakeEditable(false);
    fMessage->SetWordWrap(true);
    fMessage->AdoptSystemColors();
    fMessage->SetExplicitMinSize(BSize(1, B_SIZE_UNSET));

    fPrimary = new BButton("primary", "Connect", new BMessage(kMsgToggle));
    fPrimary->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 8, B_SIZE_UNSET));
    fSignIn = new BButton("signIn", "Open sign-in page", new BMessage(kMsgReopenSignIn));
    fLogButton = new BButton("log", "Show log", new BMessage(kMsgShowLog));

    auto value = [](const char* name) {
        BStringView* view = new BStringView(name, kDash);
        view->SetExplicitMinSize(BSize(1, B_SIZE_UNSET));
        view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        view->SetTruncation(B_TRUNCATE_MIDDLE);
        return view;
    };
    fServer = value("server");
    fAuth = value("auth");
    fAddress = value("address");
    fDuration = value("duration");
    fTraffic = value("traffic");
    fDns = value("dns");
    fRoutes = value("routes");

    auto label = [](const char* text) {
        BStringView* view = new BStringView(text, text);
        view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DISABLED_MARK_TINT);
        view->SetAlignment(B_ALIGN_RIGHT);
        return view;
    };

    float iconSize = ceilf(be_plain_font->Size() * 56 / 12);
    BLayoutBuilder::Group<>(view, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
            .Add(new IconView(iconSize), 0.0f)
            .AddGroup(B_VERTICAL, 0, 1.0f)
                .AddGlue()
                .Add(fName)
                .Add(fState)
                .AddGlue()
            .End()
        .End()
        .Add(fMessage)
        .AddGroup(B_HORIZONTAL)
            .Add(fPrimary)
            .Add(fSignIn)
            .AddGlue()
            .Add(fLogButton)
        .End()
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
            .Add(label("Server"), 0, 0).Add(fServer, 1, 0)
            .Add(label("Sign-in"), 0, 1).Add(fAuth, 1, 1)
            .Add(label("VPN address"), 0, 2).Add(fAddress, 1, 2)
            .Add(label("Connected for"), 0, 3).Add(fDuration, 1, 3)
            .Add(label("Traffic"), 0, 4).Add(fTraffic, 1, 4)
            .Add(label("DNS"), 0, 5).Add(fDns, 1, 5)
            .Add(label("Routes"), 0, 6).Add(fRoutes, 1, 6)
            .SetColumnWeight(0, 0.0f)
            .SetColumnWeight(1, 1.0f)
        .End()
        .AddGlue();
    return view;
}


void MainWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgProfilesChanged:
            _Reload(message->GetString("select", ""));
            break;
        case kMsgConnectionUpdated:
            _Refresh(message->GetString("id", ""));
            break;
        case kMsgTick: {
            ProfileItem* item = _Selected();
            if (item != nullptr && item->Status().state == ConnectionState::Connected)
                _Refresh(item->Status().id);
            break;
        }
        case kMsgSelectionChanged:
            _ShowSelected();
            break;
        case kMsgProfileInvoked:
        case kMsgConnect:
        case kMsgDisconnect:
        case kMsgToggle:
        case kMsgShowLog:
        case kMsgRemoveProfile:
        case kMsgRenameProfile:
        case kMsgReopenSignIn: {
            ProfileItem* item = _Selected();
            if (item == nullptr)
                break;
            BMessage forward(*message);
            if (forward.what == kMsgProfileInvoked)
                forward.what = kMsgToggle;
            forward.RemoveName("id");
            forward.AddString("id", item->Status().id.c_str());
            be_app->PostMessage(&forward);
            break;
        }
        case kMsgImportMenu:
            be_app->PostMessage(kMsgShowImportPanel);
            break;
        case kMsgShowWindow:
            if (IsHidden())
                Show();
            Activate();
            break;
        case B_SIMPLE_DATA:
            if (message->HasRef("refs")) {
                BMessage refs(*message);
                refs.what = B_REFS_RECEIVED;
                be_app->PostMessage(&refs);
            }
            break;
        default:
            BWindow::MessageReceived(message);
    }
}


void MainWindow::Show()
{
    // The layout sizes the window to its preferred size when first shown;
    // apply the remembered (or default) frame afterwards.
    bool first = IsHidden() && !fFrameApplied;
    BWindow::Show();
    if (first && Lock()) {
        fFrameApplied = true;
        _LoadFrame();
        Unlock();
    }
}


bool MainWindow::QuitRequested()
{
    _SaveFrame();
    return true;
}


void MainWindow::MenusBeginning()
{
    ProfileItem* item = _Selected();
    bool selected = item != nullptr;
    ConnectionState state = selected ? item->Status().state : ConnectionState::Disconnected;
    bool idle = state == ConnectionState::Disconnected || state == ConnectionState::Failed;
    fConnectItem->SetEnabled(selected && idle);
    fDisconnectItem->SetEnabled(selected && !idle && state != ConnectionState::Disconnecting);
    fRenameItem->SetEnabled(selected);
    fRemoveItem->SetEnabled(selected);
    fLogItem->SetEnabled(selected);
}


void MainWindow::_Reload(const std::string& select)
{
    std::string current = select;
    if (current.empty() && _Selected() != nullptr)
        current = _Selected()->Status().id;
    while (fList->CountItems() > 0)
        delete fList->RemoveItem(fList->CountItems() - 1);
    int32 index = 0;
    for (const ProfileStatus& status : BurrowApp::Instance()->Statuses()) {
        fList->AddItem(new ProfileItem(status));
        if (status.id == current)
            index = fList->CountItems() - 1;
    }
    if (fList->CountItems() > 0)
        fList->Select(index);
    _ShowSelected();
}


void MainWindow::_Refresh(const std::string& id)
{
    for (int32 i = 0; i < fList->CountItems(); i++) {
        ProfileItem* item = (ProfileItem*)fList->ItemAt(i);
        if (item->Status().id != id)
            continue;
        ProfileStatus status;
        if (BurrowApp::Instance()->Status(id, status)) {
            item->SetStatus(status);
            fList->InvalidateItem(i);
        }
        if (item == _Selected())
            _ShowSelected();
    }
}


void MainWindow::_ShowSelected()
{
    ProfileItem* item = _Selected();
    if (item == nullptr) {
        fCards->SetVisibleItem((int32)0);
        return;
    }
    fCards->SetVisibleItem((int32)1);
    const ProfileStatus& status = item->Status();
    const ConnectionInfo& info = status.info;
    bool connected = status.state == ConnectionState::Connected
        || status.state == ConnectionState::Reconnecting;

    fName->SetText(status.name.c_str());
    fState->SetText(StateName(status.state));
    fState->SetHighColor(StateColor(status.state));
    fState->Invalidate();

    std::string message;
    if (status.state == ConnectionState::Failed)
        message = status.detail;
    else if (status.signInPending)
        message = "Finish signing in with the web browser window Burrow opened. "
            "If you closed it, open the sign-in page again.";
    else if (status.state == ConnectionState::Connecting
        || status.state == ConnectionState::SigningIn
        || status.state == ConnectionState::Reconnecting)
        message = status.detail.empty() ? "" : status.detail + B_UTF8_ELLIPSIS;
    fMessage->SetText(message.c_str());
    if (status.state == ConnectionState::Failed) {
        rgb_color red = StateColor(ConnectionState::Failed);
        fMessage->SetFontAndColor(be_plain_font, B_FONT_ALL, &red);
    } else {
        rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
        fMessage->SetFontAndColor(be_plain_font, B_FONT_ALL, &text);
    }
    if (message.empty() != fMessage->IsHidden()) {
        if (message.empty())
            fMessage->Hide();
        else
            fMessage->Show();
    }

    switch (status.state) {
        case ConnectionState::Disconnected:
        case ConnectionState::Failed:
            fPrimary->SetLabel("Connect");
            fPrimary->SetEnabled(true);
            break;
        case ConnectionState::Connected:
            fPrimary->SetLabel("Disconnect");
            fPrimary->SetEnabled(true);
            break;
        case ConnectionState::Disconnecting:
            fPrimary->SetLabel("Disconnecting" B_UTF8_ELLIPSIS);
            fPrimary->SetEnabled(false);
            break;
        default:
            fPrimary->SetLabel("Cancel");
            fPrimary->SetEnabled(true);
    }
    if (status.signInPending == fSignIn->IsHidden()) {
        if (status.signInPending)
            fSignIn->Show();
        else
            fSignIn->Hide();
    }
    SetDefaultButton(status.state == ConnectionState::Disconnected
        || status.state == ConnectionState::Failed ? fPrimary : nullptr);

    std::string server = kDash;
    if (!status.config.remotes.empty()) {
        const Remote& remote = status.config.remotes[0];
        server = remote.host + "  (" + std::to_string(remote.port) + "/"
            + status.config.ProtoFor(remote) + ")";
        if (!info.serverAddress.empty() && info.serverAddress != remote.host)
            server += "  " + info.serverAddress;
    }
    fServer->SetText(server.c_str());
    fAuth->SetText(AuthDescription(status.config.Auth()).c_str());
    std::string address = connected && !info.localAddress.empty() ? info.localAddress : kDash;
    if (connected && !info.device.empty())
        address += "  on " + info.device;
    fAddress->SetText(address.c_str());
    fDuration->SetText(connected && info.connectedSince > 0
        ? FormatDuration(time(nullptr) - info.connectedSince).c_str() : kDash);
    fTraffic->SetText(connected ? ("\xe2\x86\x93 " + FormatBytes(info.received) + "    \xe2\x86\x91 "
        + FormatBytes(info.sent)).c_str() : kDash);

    std::string dns;
    for (const std::string& server : info.pushed.dnsServers)
        dns += (dns.empty() ? "" : ", ") + server;
    for (const std::string& domain : info.pushed.domains)
        dns += (dns.empty() ? "" : "  ") + std::string("search ") + domain;
    fDns->SetText(connected && !dns.empty() ? dns.c_str() : kDash);

    std::string routes = kDash;
    if (connected) {
        if (info.pushed.redirectGateway)
            routes = "All traffic goes through the VPN";
        else if (!info.pushed.routes.empty()) {
            routes.clear();
            for (const std::string& route : info.pushed.routes)
                routes += (routes.empty() ? "" : ", ") + ShortRoute(route);
        }
    }
    fRoutes->SetText(routes.c_str());
}


ProfileItem* MainWindow::_Selected() const
{
    int32 index = fList->CurrentSelection();
    return index >= 0 ? (ProfileItem*)fList->ItemAt(index) : nullptr;
}


void MainWindow::_SaveFrame()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return;
    path.Append("Burrow");
    create_directory(path.Path(), 0700);
    path.Append("window");
    BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    BMessage settings;
    settings.AddRect("frame", Frame());
    settings.Flatten(&file);
}


void MainWindow::_LoadFrame()
{
    BPath path;
    BMessage settings;
    BRect frame;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
        && path.Append("Burrow/window") == B_OK) {
        BFile file(path.Path(), B_READ_ONLY);
        if (file.InitCheck() == B_OK && settings.Unflatten(&file) == B_OK
            && settings.FindRect("frame", &frame) == B_OK
            && BScreen(this).Frame().Contains(frame.LeftTop() + BPoint(20, 20))) {
            MoveTo(frame.LeftTop());
            ResizeTo(frame.Width(), frame.Height());
            return;
        }
    }
    float size = be_plain_font->Size();
    ResizeTo(ceilf(size * 68), ceilf(size * 38));
    CenterOnScreen();
}

}  // namespace burrow
