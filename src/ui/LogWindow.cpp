#include "LogWindow.h"
#include "Messages.h"
#include <Button.h>
#include <Clipboard.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <TextView.h>

namespace burrow {

namespace {
constexpr uint32 kMsgCopyAll = 'cpal';
constexpr int32 kMaxLength = 512 * 1024;
}


LogWindow::LogWindow(const std::string& profileName, const std::vector<std::string>& lines,
    const BMessage& closed, BMessenger target)
    :
    BWindow(BRect(80, 80, 780, 480), (profileName + " log").c_str(), B_DOCUMENT_WINDOW,
        B_AUTO_UPDATE_SIZE_LIMITS),
    fClosed(closed),
    fTarget(target)
{
    fText = new BTextView("log");
    fText->MakeEditable(false);
    fText->SetStylable(false);
    fText->SetWordWrap(true);
    BFont font(be_fixed_font);
    fText->SetFontAndColor(&font);
    BScrollView* scroll = new BScrollView("scroll", fText, 0, false, true, B_NO_BORDER);

    BButton* copy = new BButton("Copy all", new BMessage(kMsgCopyAll));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(scroll)
        .AddGroup(B_HORIZONTAL)
            .SetInsets(B_USE_SMALL_SPACING)
            .AddGlue()
            .Add(copy)
        .End();

    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";
    fText->SetText(text.c_str());
    fText->ScrollToOffset(fText->TextLength());
}


void LogWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgLogLine:
            _Append(message->GetString("line", ""));
            break;
        case kMsgCopyAll:
            if (be_clipboard->Lock()) {
                be_clipboard->Clear();
                be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE, fText->Text(),
                    fText->TextLength());
                be_clipboard->Commit();
                be_clipboard->Unlock();
            }
            break;
        default:
            BWindow::MessageReceived(message);
    }
}


bool LogWindow::QuitRequested()
{
    fTarget.SendMessage(&fClosed);
    return true;
}


void LogWindow::_Append(const std::string& line)
{
    // Follow the end only when the reader is already there.
    int32 start, end;
    fText->GetSelection(&start, &end);
    bool atEnd = start == end && start >= fText->TextLength() - 1;
    if (fText->TextLength() > kMaxLength)
        fText->Delete(0, fText->TextLength() - kMaxLength / 2);
    fText->Insert(fText->TextLength(), (line + "\n").c_str(), line.size() + 1);
    if (atEnd)
        fText->ScrollToOffset(fText->TextLength());
}

}  // namespace burrow
