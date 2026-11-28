#ifndef NEUTRINO_CREDENTIALS_H
#define NEUTRINO_CREDENTIALS_H

#include "neutrino.h"

enum { PASSWORD_SALT = 16, PASSWORD_HASH = 32 };
typedef struct {
    unsigned char salt[PASSWORD_SALT], hash[PASSWORD_HASH];
    unsigned char cache_salt[PASSWORD_SALT], cache[PASSWORD_HASH];
    bool enabled, cached;
    unsigned failures;
    uint64_t retry_at;
} PasswordVerifier;
typedef struct {
    PasswordVerifier admin, connection;
    bool saved;
} LocalCredentials;
typedef enum { CREDENTIALS_MISSING, CREDENTIALS_READY, CREDENTIALS_INVALID } CredentialStatus;

CredentialStatus credentials_load(LocalCredentials *auth, const char *path);
bool credentials_prepare(LocalCredentials *auth, const char *admin, const char *connection);
bool credentials_save(const LocalCredentials *auth, const char *path);
bool password_verify(PasswordVerifier *verifier, const char *password);
bool password_cached(const PasswordVerifier *verifier, const char *password);
bool credential_token(Text *token);

#endif
