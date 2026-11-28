#include "credentials.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint64_t clock_ms;
uint64_t monotonic_ms(void) { return clock_ms; }

#define CHECK(condition) do { \
    if (!(condition)) { fprintf(stderr,"Check failed at line %d: %s\n",__LINE__,#condition); return 1; } \
} while (0)

int main(int argc, char **argv)
{
    CHECK(argc == 2);
#ifdef _WIN32
    const char *path = argv[1];
#else
    (void)argv;
    char directory[] = "/tmp/neutrino-credentials.XXXXXX", path[sizeof directory+20];
    CHECK(mkdtemp(directory));
    CHECK(snprintf(path,sizeof path,"%s/credentials.json",directory) > 0);
#endif
    const char admin[] = "test-admin-password-1", connection[] = "test-connection-password-2";
    LocalCredentials created, loaded = {0}, other;
    remove(path);
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_MISSING);
    CHECK(credentials_prepare(&created,admin,connection));
    CHECK(credentials_prepare(&other,admin,""));
    CHECK(memcmp(created.admin.salt,other.admin.salt,PASSWORD_SALT));
    CHECK(memcmp(created.admin.salt,created.connection.salt,PASSWORD_SALT));
    CHECK(credentials_save(&created,path));
    CHECK(!credentials_save(&other,path));
#ifndef _WIN32
    struct stat info;
    CHECK(!stat(path,&info) && (info.st_mode & 0777) == 0600);
#endif
    char *data = read_file(path);
    CHECK(data && !strstr(data,admin) && !strstr(data,connection));
    free(data);
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_READY);
    CHECK(loaded.saved && !loaded.admin.cached && !loaded.connection.cached);
    CHECK(!password_cached(&loaded.admin,admin));
    CHECK(!password_verify(&loaded.admin,connection));
    CHECK(password_verify(&loaded.admin,admin) && password_cached(&loaded.admin,admin));
    for (int i = 0; i < 3; ++i) CHECK(!password_verify(&loaded.connection,admin));
    CHECK(!password_verify(&loaded.connection,connection));
    clock_ms = loaded.connection.retry_at;
    CHECK(password_verify(&loaded.connection,connection));
    const char updated_admin[] = "different-test-admin-password";
    const char updated_connection[] = "different-connection-password";
    CHECK(credentials_rotate(&loaded,path,connection,updated_connection));
    CHECK(credentials_rotate(&loaded,"",updated_admin,updated_connection));
    CHECK(password_cached(&loaded.admin,admin) && password_cached(&loaded.connection,connection));
    CHECK(!credentials_rotate(&loaded,path,updated_admin,nullptr));
    CHECK(password_cached(&loaded.connection,connection));
    CHECK(!credentials_rotate(&loaded,path,nullptr,updated_connection));
    CHECK(!password_cached(&loaded.admin,admin) && !password_cached(&loaded.connection,connection));
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_READY);
    CHECK(!password_verify(&loaded.admin,admin) && !password_verify(&loaded.connection,connection));
    CHECK(password_verify(&loaded.admin,updated_admin) && password_verify(&loaded.connection,updated_connection));
    CHECK(!credentials_rotate(&loaded,path,nullptr,""));
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_READY);
    CHECK(!loaded.connection.enabled && password_verify(&loaded.admin,updated_admin));
    CHECK(remove(path) == 0);
    CHECK(credentials_save(&other,path));
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_READY);
    CHECK(!loaded.connection.enabled && password_verify(&loaded.connection,""));
    CHECK(write_file(path,"{\"version\":1}",true));
    CHECK(credentials_load(&loaded,path) == CREDENTIALS_INVALID);
    CHECK(remove(path) == 0);
#ifndef _WIN32
    CHECK(rmdir(directory) == 0);
#endif
    return 0;
}
