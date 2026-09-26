// Message codes shared by Burrow's application, windows and Deskbar replicant.
#pragma once
#include <SupportDefs.h>

namespace burrow {

extern const char* const kAppSignature;   // application/x-vnd.Burrow

enum {
    // "id": profile id
    kMsgConnect = 'cnct',
    kMsgDisconnect = 'dcnt',
    kMsgToggle = 'tgle',
    kMsgShowLog = 'slog',
    kMsgRemoveProfile = 'rmpf',
    kMsgRenameProfile = 'rnpf',
    kMsgShowImportPanel = 'impt',
    kMsgShowWindow = 'shwn',
    kMsgReopenSignIn = 'rsgn',

    // App -> windows
    kMsgProfilesChanged = 'pchg',     // "select": id to select
    kMsgConnectionUpdated = 'cupd',   // "id"
    kMsgLogLine = 'logl',             // "id", "line"

    // Connection threads -> App
    kMsgConnectionChanged = 'cchg',   // "id", "state" (int32), "detail"
    kMsgConnectionLog = 'clog',       // "id", "line"
    kMsgConnectionStats = 'csta',     // "id"
    kMsgNeedsSignIn = 'nsgn',         // "id", "url"
    kMsgNeedsCredentials = 'ncrd',    // "id", "challenge", "echo", "retry"
    kMsgSamlResponse = 'saml',        // "response"

    // Dialogs -> App
    kMsgImportNamed = 'impn',         // "text", "name", "file"
    kMsgRenamed = 'rnmd',             // "id", "name"
    kMsgCredentialsEntered = 'cred',  // "id", "user", "password", "response"
    kMsgCredentialsCancelled = 'ccnl',// "id"

    kMsgTick = 'tick',
    kMsgSelectionChanged = 'slch',
    kMsgProfileInvoked = 'pinv',
};

}  // namespace burrow
