#include "CredentialVault.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>

#include <QByteArray>

#include <string>
#endif

namespace creds {

bool CredentialVault::isSupported()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

QString CredentialVault::backendName()
{
#if defined(_WIN32)
    return QStringLiteral("WindowsCredentialManager");
#else
    return QStringLiteral("Unsupported");
#endif
}

bool CredentialVault::store(const QString& target, const QString& value)
{
#if defined(_WIN32)
    if (target.isEmpty()) {
        return false;
    }

    const std::wstring wTarget = target.toStdWString();
    const QByteArray utf8 = value.toUtf8();

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(wTarget.c_str());
    cred.CredentialBlob = utf8.isEmpty()
        ? nullptr
        : reinterpret_cast<LPBYTE>(const_cast<char*>(utf8.constData()));
    cred.CredentialBlobSize = static_cast<DWORD>(utf8.size());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    return CredWriteW(&cred, 0) != FALSE;
#else
    static_cast<void>(target);
    static_cast<void>(value);
    return false;
#endif
}

QString CredentialVault::retrieve(const QString& target)
{
#if defined(_WIN32)
    if (target.isEmpty()) {
        return QString();
    }

    const std::wstring wTarget = target.toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(wTarget.c_str(), CRED_TYPE_GENERIC, 0, &credential) || credential == nullptr) {
        return QString();
    }

    if (credential->CredentialBlobSize == 0 || credential->CredentialBlob == nullptr) {
        CredFree(credential);
        return QString();
    }

    const QByteArray utf8(reinterpret_cast<const char*>(credential->CredentialBlob),
                          static_cast<int>(credential->CredentialBlobSize));
    const QString result = QString::fromUtf8(utf8);
    CredFree(credential);
    return result;
#else
    static_cast<void>(target);
    return QString();
#endif
}

bool CredentialVault::erase(const QString& target)
{
#if defined(_WIN32)
    if (target.isEmpty()) {
        return false;
    }

    const std::wstring wTarget = target.toStdWString();
    if (CredDeleteW(wTarget.c_str(), CRED_TYPE_GENERIC, 0) != FALSE) {
        return true;
    }

    return GetLastError() == ERROR_NOT_FOUND;
#else
    static_cast<void>(target);
    return true;
#endif
}

bool CredentialVault::exists(const QString& target)
{
#if defined(_WIN32)
    if (target.isEmpty()) {
        return false;
    }

    const std::wstring wTarget = target.toStdWString();
    PCREDENTIALW credential = nullptr;
    const bool found = CredReadW(wTarget.c_str(), CRED_TYPE_GENERIC, 0, &credential) && credential != nullptr;
    if (credential != nullptr) {
        CredFree(credential);
    }

    return found;
#else
    static_cast<void>(target);
    return false;
#endif
}

} // namespace creds
