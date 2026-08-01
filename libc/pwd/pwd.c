#include <pwd.h>

struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid)       { (void)uid; return NULL; }
