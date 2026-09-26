#include "CredentialsWindow.h"
#include "Messages.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>

namespace burrow {

namespace {
constexpr uint32 kMsgOK = 'crok';
}


CredentialsWindow::CredentialsWindow(const std::string& id, const std::string& profileName,
    const std::string& challenge, bool echo, bool retry, BMessenger target)
    :
    BWindow(BRect(0, 0, 380, 160), "Sign in", B_TITLED_WINDOW_LOOK, B_FLOATING_APP_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
    fId(id),
    fTarget(target)
{
    // Without a user name field this answers a dynamic challenge (a CRV1 prompt).
    bool challengeOnly = !challenge.empty() && !retry;
    std::string title = retry ? "The user name or password was not accepted. Try again for "
        : "Sign in to ";
    BStringView* heading = new BStringView("heading", (title + profileName).c_str());
    BFont font(be_bold_font);
    heading->SetFont(&font);

    BLayoutBuilder::Group<> builder(this, B_VERTICAL);
    builder.SetInsets(B_USE_WINDOW_SPACING).Add(heading);
    BLayoutBuilder::Grid<> fields(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
    int32 row = 0;
    if (!challengeOnly) {
        fUser = new BTextControl("User name:", "", nullptr);
        fPassword = new BTextControl("Password:", "", new BMessage(kMsgOK));
        fPassword->TextView()->HideTyping(true);
        fields.AddTextControl(fUser, 0, row);
        row++;
        fields.AddTextControl(fPassword, 0, row);
        row++;
    }
    if (!challenge.empty()) {
        fResponse = new BTextControl((challenge + ":").c_str(), "", new BMessage(kMsgOK));
        fResponse->TextView()->HideTyping(!echo);
        fields.AddTextControl(fResponse, 0, row++);
    }
    BButton* ok = new BButton("Connect", new BMessage(kMsgOK));
    BButton* cancel = new BButton("Cancel", new BMessage(B_QUIT_REQUESTED));
    builder.Add(fields.View())
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End();
    SetDefaultButton(ok);
    if (fUser != nullptr)
        fUser->MakeFocus(true);
    else if (fResponse != nullptr)
        fResponse->MakeFocus(true);
    CenterOnScreen();
}


void CredentialsWindow::MessageReceived(BMessage* message)
{
    if (message->what != kMsgOK) {
        BWindow::MessageReceived(message);
        return;
    }
    BMessage answer(kMsgCredentialsEntered);
    answer.AddString("id", fId.c_str());
    answer.AddString("user", fUser != nullptr ? fUser->Text() : "");
    answer.AddString("password", fPassword != nullptr ? fPassword->Text() : "");
    answer.AddString("response", fResponse != nullptr ? fResponse->Text() : "");
    fTarget.SendMessage(&answer);
    fAnswered = true;
    Quit();
}


bool CredentialsWindow::QuitRequested()
{
    if (!fAnswered) {
        BMessage cancelled(kMsgCredentialsCancelled);
        cancelled.AddString("id", fId.c_str());
        fTarget.SendMessage(&cancelled);
    }
    return true;
}

}  // namespace burrow
