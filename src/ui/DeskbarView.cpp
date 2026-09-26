#include "DeskbarView.h"
#include "Messages.h"
#include "core/Connection.h"
#include <AppFileInfo.h>
#include <Bitmap.h>
#include <Deskbar.h>
#include <File.h>
#include <IconUtils.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <Window.h>
#include <image.h>

using burrow::ConnectionState;

const char* BurrowDeskbarView::kName = "Burrow";

namespace {

constexpr uint32 kMsgPoll = 'poll';

rgb_color DotColor(int32 state)
{
    switch ((ConnectionState)state) {
        case ConnectionState::Connected:
            return make_color(46, 190, 100);
        case ConnectionState::Connecting:
        case ConnectionState::SigningIn:
        case ConnectionState::Reconnecting:
        case ConnectionState::Disconnecting:
            return make_color(240, 160, 30);
        case ConnectionState::Failed:
            return make_color(225, 60, 45);
        default:
            break;
    }
    return make_color(0, 0, 0, 0);
}

// The Burrow executable this code was loaded from (inside Deskbar's team).
bool OwnImagePath(BString& path)
{
    image_info info;
    int32 cookie = 0;
    addr_t self = (addr_t)&OwnImagePath;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (self >= (addr_t)info.text && self < (addr_t)info.text + info.text_size) {
            path = info.name;
            return true;
        }
    }
    return false;
}

}  // namespace


BurrowDeskbarView::BurrowDeskbarView(BRect frame)
    :
    BView(frame, kName, B_FOLLOW_LEFT | B_FOLLOW_TOP, B_WILL_DRAW)
{
    _Init();
}


BurrowDeskbarView::BurrowDeskbarView(BMessage* archive)
    :
    BView(archive)
{
    _Init();
}


BurrowDeskbarView::~BurrowDeskbarView()
{
    delete fIcon;
    delete fPoller;
}


void BurrowDeskbarView::_Init()
{
    BString path;
    if (!OwnImagePath(path))
        return;
    BFile file(path.String(), B_READ_ONLY);
    BAppFileInfo info(&file);
    float size = Bounds().Height() + 1;
    fIcon = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
    if (info.InitCheck() != B_OK || info.GetIcon(fIcon, (icon_size)size) != B_OK) {
        delete fIcon;
        fIcon = nullptr;
    }
}


BArchivable* BurrowDeskbarView::Instantiate(BMessage* archive)
{
    if (!validate_instantiation(archive, "BurrowDeskbarView"))
        return nullptr;
    return new BurrowDeskbarView(archive);
}


status_t BurrowDeskbarView::Archive(BMessage* archive, bool deep) const
{
    status_t status = BView::Archive(archive, deep);
    if (status == B_OK)
        status = archive->AddString("add_on", burrow::kAppSignature);
    if (status == B_OK)
        status = archive->AddString("class", "BurrowDeskbarView");
    return status;
}


void BurrowDeskbarView::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    if (ViewUIColor() == B_NO_COLOR)
        SetViewColor(Parent() != nullptr ? Parent()->ViewColor() : B_TRANSPARENT_COLOR);
    SetLowColor(ViewColor());
    BMessage poll(kMsgPoll);
    fPoller = new BMessageRunner(BMessenger(this), &poll, 2000000);
    _Poll();
}


void BurrowDeskbarView::DetachedFromWindow()
{
    delete fPoller;
    fPoller = nullptr;
    BView::DetachedFromWindow();
}


void BurrowDeskbarView::Draw(BRect)
{
    BRect bounds = Bounds();
    if (fIcon != nullptr) {
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(fIcon, bounds.LeftTop());
    }
    // A status dot in the lower-left corner, outlined for contrast.
    rgb_color dot = DotColor(_OverallState());
    if (dot.alpha == 0 || !fAppRunning)
        return;
    float radius = ceilf(bounds.Height() * 0.2f);
    BPoint center(bounds.left + radius + 0.5f, bounds.bottom - radius - 0.5f);
    SetDrawingMode(B_OP_OVER);
    SetHighColor(40, 40, 40);
    FillEllipse(center, radius + 1, radius + 1);
    SetHighColor(dot);
    FillEllipse(center, radius, radius);
}


void BurrowDeskbarView::MouseDown(BPoint where)
{
    _ShowMenu(ConvertToScreen(where));
}


void BurrowDeskbarView::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgPoll:
            _Poll();
            break;
        case burrow::kMsgStatusReply: {
            fEntries.clear();
            const char* id;
            for (int32 i = 0; message->FindString("id", i, &id) == B_OK; i++) {
                Entry entry;
                entry.id = id;
                entry.name = message->GetString("name", i, "");
                entry.state = message->GetInt32("state", i, 0);
                fEntries.push_back(entry);
            }
            fAppRunning = true;
            Invalidate();
            break;
        }
        default:
            BView::MessageReceived(message);
    }
}


void BurrowDeskbarView::_Poll()
{
    BMessenger app(burrow::kAppSignature);
    bool running = app.IsValid();
    if (running) {
        BMessage request(burrow::kMsgGetStatus);
        app.SendMessage(&request, BMessenger(this), 500000);
    } else if (fAppRunning) {
        fAppRunning = false;
        fEntries.clear();
        Invalidate();
    }
}


int32 BurrowDeskbarView::_OverallState() const
{
    // The most noteworthy state wins: failed, in progress, connected.
    int32 result = (int32)ConnectionState::Disconnected;
    for (const Entry& entry : fEntries) {
        ConnectionState state = (ConnectionState)entry.state;
        if (state == ConnectionState::Failed)
            return entry.state;
        if (state != ConnectionState::Disconnected && state != ConnectionState::Connected)
            result = entry.state;
        else if (state == ConnectionState::Connected
            && result == (int32)ConnectionState::Disconnected)
            result = entry.state;
    }
    return result;
}


void BurrowDeskbarView::_ShowMenu(BPoint where)
{
    BPopUpMenu* menu = new BPopUpMenu("Burrow", false, false);
    menu->SetFont(be_plain_font);
    for (const Entry& entry : fEntries) {
        ConnectionState state = (ConnectionState)entry.state;
        bool idle = state == ConnectionState::Disconnected || state == ConnectionState::Failed;
        BString label(entry.name.c_str());
        if (!idle || state == ConnectionState::Failed)
            label << " \xe2\x80\x94 " << burrow::StateName(state);
        BMessage* message = new BMessage(burrow::kMsgToggle);
        message->AddString("id", entry.id.c_str());
        BMenuItem* item = new BMenuItem(label.String(), message);
        item->SetMarked(state == ConnectionState::Connected);
        menu->AddItem(item);
    }
    if (!fEntries.empty())
        menu->AddSeparatorItem();
    menu->AddItem(new BMenuItem("Open Burrow" B_UTF8_ELLIPSIS, new BMessage(burrow::kMsgShowWindow)));
    menu->AddItem(new BMenuItem("Remove from Deskbar", new BMessage(B_QUIT_REQUESTED)));

    BMenuItem* chosen = menu->Go(where, false, true);
    if (chosen != nullptr) {
        BMessage* message = chosen->Message();
        if (message->what == B_QUIT_REQUESTED) {
            BDeskbar().RemoveItem(kName);
        } else {
            BMessenger app(burrow::kAppSignature);
            if (app.IsValid())
                app.SendMessage(message);
            else if (message->what == burrow::kMsgShowWindow)
                be_roster->Launch(burrow::kAppSignature);
        }
    }
    delete menu;
}


extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth, float maxHeight)
{
    float size = floorf(maxHeight > 0 ? maxHeight : 16);
    return new BurrowDeskbarView(BRect(0, 0, size - 1, size - 1));
}
