#include "credentials.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonRandom.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

enum { PASSWORD_ROUNDS = 600000 }; /* Credential file version 1. */

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool random_bytes(unsigned char *bytes, size_t size)
{
#ifdef _WIN32
    return BCryptGenRandom(nullptr,bytes,(ULONG)size,BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    return CCRandomGenerateBytes(bytes,size) == kCCSuccess;
#else
    return RAND_bytes(bytes,(int)size) == 1;
#endif
}

static bool derive(const char *password, const unsigned char salt[PASSWORD_SALT], unsigned rounds,
                   unsigned char output[PASSWORD_HASH])
{
    size_t length = strlen(password);
    if (length < 16 || length > 256) return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return false;
    bool ok = !BCryptDeriveKeyPBKDF2(algorithm,(PUCHAR)password,(ULONG)length,(PUCHAR)salt,
                                    PASSWORD_SALT,rounds,output,PASSWORD_HASH,0);
    BCryptCloseAlgorithmProvider(algorithm,0);
    return ok;
#elif defined(__APPLE__)
    return CCKeyDerivationPBKDF(kCCPBKDF2,password,length,salt,PASSWORD_SALT,kCCPRFHmacAlgSHA256,
                                rounds,output,PASSWORD_HASH) == 0;
#else
    return PKCS5_PBKDF2_HMAC(password,(int)length,salt,PASSWORD_SALT,(int)rounds,
                             EVP_sha256(),PASSWORD_HASH,output) == 1;
#endif
}

static bool matching_hash(const unsigned char *left, const unsigned char *right)
{
    unsigned diff = 0;
    for (size_t i = 0; i < PASSWORD_HASH; ++i) diff |= left[i]^right[i];
    return !diff;
}

static void hex_encode(const unsigned char *data, size_t size, char *out)
{
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2*i] = hex[data[i]>>4];
        out[2*i+1] = hex[data[i]&15];
    }
    out[2*size] = 0;
}

static bool hex_decode(const char *input, unsigned char *out, size_t size)
{
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        const char *high = strchr(hex,input[i*2]), *low = strchr(hex,input[i*2+1]);
        if (!high || !low || high == hex+16 || low == hex+16) return false;
        out[i] = (unsigned char)(((high-hex)<<4) | (low-hex));
    }
    return true;
}

static bool create_verifier(PasswordVerifier *verifier, const char *password)
{
    if (!*password) return true;
    verifier->enabled = true;
    return random_bytes(verifier->salt,PASSWORD_SALT) &&
           derive(password,verifier->salt,PASSWORD_ROUNDS,verifier->hash) &&
           random_bytes(verifier->cache_salt,PASSWORD_SALT) &&
           (verifier->cached = derive(password,verifier->cache_salt,1,verifier->cache));
}

bool credentials_prepare(LocalCredentials *auth, const char *admin, const char *connection)
{
    *auth = (LocalCredentials){0};
    return *admin && create_verifier(&auth->admin,admin) && create_verifier(&auth->connection,connection);
}

static bool load_verifier(PasswordVerifier *verifier, const Json *root, const char *name, bool optional)
{
    if (!cJSON_IsString(at(root,name))) return false;
    const char *encoded = js(root,name);
    if (!*encoded) return optional;
    verifier->enabled = true;
    return strlen(encoded) == 2*(PASSWORD_SALT+PASSWORD_HASH) &&
           hex_decode(encoded,verifier->salt,PASSWORD_SALT) &&
           hex_decode(encoded+2*PASSWORD_SALT,verifier->hash,PASSWORD_HASH) &&
           random_bytes(verifier->cache_salt,PASSWORD_SALT);
}

CredentialStatus credentials_load(LocalCredentials *auth, const char *path)
{
    char *data = read_file(path);
    if (!data) return errno == ENOENT ? CREDENTIALS_MISSING : CREDENTIALS_INVALID;
    Json *root = strlen(data) < 512 ? cJSON_ParseWithOpts(data,nullptr,true) : nullptr;
    free(data);
    LocalCredentials loaded = {0};
    const Json *version = at(root,"version");
    const bool ok = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 3 &&
        cJSON_IsNumber(version) && version->valuedouble == 1 &&
        load_verifier(&loaded.admin,root,"admin",false) &&
        load_verifier(&loaded.connection,root,"connection",true);
    cJSON_Delete(root);
    if (!ok) return CREDENTIALS_INVALID;
    loaded.saved = true;
    *auth = loaded;
    return CREDENTIALS_READY;
}

static void store_verifier(Json *root, const char *name, const PasswordVerifier *verifier)
{
    char encoded[2*(PASSWORD_SALT+PASSWORD_HASH)+1] = "";
    if (verifier->enabled) {
        hex_encode(verifier->salt,PASSWORD_SALT,encoded);
        hex_encode(verifier->hash,PASSWORD_HASH,encoded+2*PASSWORD_SALT);
    }
    set(root,name,encoded);
}

static bool write_credentials(const LocalCredentials *auth, const char *path, bool replace)
{
    Json *root = cJSON_CreateObject();
    integer(root,"version",1);
    store_verifier(root,"admin",&auth->admin);
    store_verifier(root,"connection",&auth->connection);
    char *data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    const bool ok = data && write_file(path,data,replace);
    cJSON_free(data);
    return ok;
}

bool credentials_save(const LocalCredentials *auth, const char *path)
{
    return write_credentials(auth,path,false);
}

static bool password_matches(const PasswordVerifier *verifier, const char *password)
{
    if (!verifier->enabled || !*password) return false;
    if (password_cached(verifier,password)) return true;
    unsigned char hash[PASSWORD_HASH];
    const bool equal = derive(password,verifier->salt,PASSWORD_ROUNDS,hash) &&
                       matching_hash(hash,verifier->hash);
    wipe(hash,sizeof hash);
    return equal;
}

const char *credentials_rotate(LocalCredentials *auth, const char *path, const char *admin, const char *connection)
{
    if (!auth->saved || (!admin && !connection)) return "Enter a new password or remove the connection password.";
    if ((admin && !*admin) || (admin && connection && !strcmp(admin,connection)))
        return "The admin and connection passwords must differ.";
    if ((admin && (password_matches(&auth->admin,admin) || password_matches(&auth->connection,admin))) ||
        (connection && (password_matches(&auth->admin,connection) || password_matches(&auth->connection,connection))))
        return "Choose new passwords that differ from both current passwords.";
    LocalCredentials next = *auth;
    const char *error = nullptr;
    if (admin) next.admin = (PasswordVerifier){0};
    if (connection) next.connection = (PasswordVerifier){0};
    if ((admin && !create_verifier(&next.admin,admin)) ||
        (connection && !create_verifier(&next.connection,connection)))
        error = "Could not prepare secure password verifiers.";
    else if (!write_credentials(&next,path,true))
        error = "Could not save the new passwords. Your existing passwords still apply.";
    else *auth = next;
    wipe(&next,sizeof next);
    return error;
}

bool password_cached(const PasswordVerifier *verifier, const char *password)
{
    /* The cache uses a separate process-local salt and is never serialized. */
    unsigned char hash[PASSWORD_HASH];
    const bool ok = verifier->cached && derive(password,verifier->cache_salt,1,hash) &&
                    matching_hash(hash,verifier->cache);
    wipe(hash,sizeof hash);
    return ok;
}

bool password_verify(PasswordVerifier *verifier, const char *password)
{
    if (!verifier->enabled || password_cached(verifier,password)) return true;
    if (monotonic_ms() < verifier->retry_at) return false;
    unsigned char hash[PASSWORD_HASH];
    const bool ok = derive(password,verifier->salt,PASSWORD_ROUNDS,hash) &&
                    matching_hash(hash,verifier->hash) &&
                    derive(password,verifier->cache_salt,1,verifier->cache);
    wipe(hash,sizeof hash);
    if (ok) {
        verifier->cached = true;
        verifier->failures = 0;
    } else if (++verifier->failures >= 3) {
        verifier->failures = 0;
        verifier->retry_at = monotonic_ms()+2000;
    }
    return ok;
}

bool credential_token(Text *token)
{
    unsigned char bytes[PASSWORD_HASH];
    char encoded[2*PASSWORD_HASH+1];
    if (!random_bytes(bytes,sizeof bytes)) { wipe(bytes,sizeof bytes); return false; }
    hex_encode(bytes,sizeof bytes,encoded);
    append(token,encoded);
    wipe(bytes,sizeof bytes);
    wipe(encoded,sizeof encoded);
    return true;
}
