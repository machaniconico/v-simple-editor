#pragma once

#include <QString>

namespace creds {

// Windows Credential Manager (wincred.h CRED_TYPE_GENERIC) backed secure storage.
// Non-Windows builds always report unsupported and expose noop-style behavior.
class CredentialVault {
public:
    static bool isSupported();
    static QString backendName();
    static bool store(const QString& target, const QString& value);
    static QString retrieve(const QString& target);
    static bool erase(const QString& target);
    static bool exists(const QString& target);
};

} // namespace creds
