// Burrow's window: the profiles on the left, the selected one's status,
// actions and connection details on the right.
#pragma once
#include "App.h"
#include <ListItem.h>
#include <Window.h>
#include <string>

class BBitmap;
class BButton;
class BCardLayout;
class BListView;
class BMenuItem;
class BMessageRunner;
class BStringView;
class BTextView;

namespace burrow {

rgb_color StateColor(ConnectionState state);
std::string StateSummary(const ProfileStatus& status);
BBitmap* LoadAppIcon(float size);

class ProfileItem : public BListItem {
public:
    explicit ProfileItem(const ProfileStatus& status);
    void Update(BView* owner, const BFont* font) override;
    void DrawItem(BView* owner, BRect frame, bool complete) override;

    void SetStatus(const ProfileStatus& status) { fStatus = status; }
    const ProfileStatus& Status() const { return fStatus; }

private:
    ProfileStatus fStatus;
};

class IconView : public BView {
public:
    explicit IconView(float size);
    ~IconView() override;
    void Draw(BRect updateRect) override;

private:
    BBitmap* fIcon;
};

class MainWindow : public BWindow {
public:
    MainWindow();
    ~MainWindow() override;

    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
    void MenusBeginning() override;
    void Show() override;

private:
    BView* _BuildDetails();
    BView* _BuildEmpty();
    void _Reload(const std::string& select);
    void _Refresh(const std::string& id);
    void _ShowSelected();
    ProfileItem* _Selected() const;
    void _SaveFrame();
    void _LoadFrame();

    BListView* fList;
    BCardLayout* fCards;
    BStringView* fName;
    BStringView* fState;
    BTextView* fMessage;
    BButton* fPrimary;
    BButton* fSignIn;
    BButton* fLogButton;
    BStringView* fServer;
    BStringView* fAuth;
    BStringView* fAddress;
    BStringView* fDuration;
    BStringView* fTraffic;
    BStringView* fDns;
    BStringView* fRoutes;
    BMenuItem* fConnectItem;
    BMenuItem* fDisconnectItem;
    BMenuItem* fRenameItem;
    BMenuItem* fRemoveItem;
    BMenuItem* fLogItem;
    BMenuItem* fDeskbarItem;
    BMessageRunner* fTicker;
    bool fFrameApplied = false;
};

}  // namespace burrow
