#include "NameWindow.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <TextControl.h>

namespace burrow {

namespace {
constexpr uint32 kMsgOK = 'nmok';
constexpr uint32 kMsgNameChanged = 'nmch';
}


NameWindow::NameWindow(const char* title, const char* label, const char* name,
    const char* action, const BMessage& message, BMessenger target)
    :
    BWindow(BRect(0, 0, 360, 100), title, B_TITLED_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
            | B_CLOSE_ON_ESCAPE),
    fMessage(message),
    fTarget(target)
{
    fName = new BTextControl(label, name, new BMessage(kMsgOK));
    fName->SetModificationMessage(new BMessage(kMsgNameChanged));
    fName->TextView()->SetExplicitMinSize(BSize(260, B_SIZE_UNSET));
    fOK = new BButton(action, new BMessage(kMsgOK));
    fOK->SetEnabled(BString(name).Trim().Length() > 0);
    BButton* cancel = new BButton("Cancel", new BMessage(B_QUIT_REQUESTED));

    BLayoutBuilder::Group<>(this, B_VERTICAL)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(fName)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(fOK)
        .End();
    SetDefaultButton(fOK);
    fName->MakeFocus(true);
    fName->TextView()->SelectAll();
    CenterOnScreen();
}


void NameWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgNameChanged:
            fOK->SetEnabled(BString(fName->Text()).Trim().Length() > 0);
            break;
        case kMsgOK: {
            BString name(fName->Text());
            if (name.Trim().IsEmpty())
                break;
            fMessage.AddString("name", name.String());
            fTarget.SendMessage(&fMessage);
            Quit();
            break;
        }
        default:
            BWindow::MessageReceived(message);
    }
}

}  // namespace burrow
