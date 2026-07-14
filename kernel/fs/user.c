/* The user database, stored under /userconfig.
 *
 *     /userconfig/users              the list:  name:uid:roles[:shell]
 *     /userconfig/<name>/password    that user's password: salt:hash
 *
 * The split matters.  A user with the usereditor role may write /userconfig --
 * that is how accounts are added and roles changed -- but the password files
 * inside it are root-only, for reading as well as writing.  The VFS enforces
 * that; see may_write() and may_read() in vfs.c.
 *
 * The kernel reaches these files through wfs_* directly, below the permission
 * layer, so it can always read and rewrite them whoever is asking.  That is
 * what lets passwd and su work without setuid: no program ever opens a
 * password file, and there is nothing privileged in those programs to abuse.
 */

#include "user.h"
#include "wfs_kernel.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

#define USERCONFIG_DIR "/userconfig"
#define USER_LIST_FILE "/userconfig/users"

typedef struct {
    bool     used;
    char     name[W_NAME_LEN + 1];
    uint32_t uid;
    uint32_t roles;
    uint64_t salt;
    uint64_t hash;
    char     shell[W_SHELL_MAX + 1];   /* login shell, "" means the default */
} user_entry_t;

#define DEFAULT_SHELL "/app/whell/launch"

static user_entry_t users[W_MAX_USERS];
static uint32_t     next_uid = 1;

/* ------------------------------------------------------------------ *
 *  Password hashing
 * ------------------------------------------------------------------ */

/* Salted, iterated FNV-1a.
 *
 * This is NOT a real password hash.  bcrypt, scrypt and argon2 exist because
 * a fast hash can be brute-forced at billions of guesses a second, and four
 * thousand rounds of FNV does not change that by much.  It is here so the file
 * holds something other than plaintext, and because the salt at least means
 * two users with the same password do not look identical.
 *
 * What actually protects passwords is that the files are unreadable outside
 * the kernel; see the comment at the top of this file. */
static uint64_t hash_password(uint64_t salt, const char *password)
{
    uint64_t h = 0xcbf29ce484222325UL ^ salt;

    for (int round = 0; round < 4096; round++) {
        for (const char *p = password; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 0x100000001b3UL;
        }
        h ^= salt;
        h *= 0x100000001b3UL;
    }

    /* Never produce 0: that value means "no password set". */
    return h ? h : 1;
}

/* Compare in constant time, so a caller cannot learn the hash one byte at a
 * time from how long a rejection takes. */
static bool hash_equal(uint64_t a, uint64_t b)
{
    uint64_t diff = a ^ b;
    uint8_t  acc = 0;

    for (int i = 0; i < 8; i++)
        acc |= (uint8_t)(diff >> (i * 8));

    return acc == 0;
}

/* There is no entropy source in WOS, so the salt comes from the timer and an
 * address.  Weak, but it varies between users, which is all a salt has to do
 * here. */
static uint64_t make_salt(void)
{
    uint64_t marker = (uint64_t)(uintptr_t)&next_uid;
    return (pit_ticks() * 0x9E3779B97F4A7C15UL) ^ (marker * 0xBF58476D1CE4E5B9UL)
           ^ ((uint64_t)next_uid << 32);
}

/* ------------------------------------------------------------------ *
 *  Small text helpers
 * ------------------------------------------------------------------ */

static uint64_t parse_u64(const char **p)
{
    uint64_t v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (uint64_t)(**p - '0');
        (*p)++;
    }
    return v;
}

static void append_u64(char *buf, size_t *at, size_t cap, uint64_t v)
{
    char tmp[21];
    int  n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0 && *at + 1 < cap)
        buf[(*at)++] = tmp[--n];
}

static void append_str(char *buf, size_t *at, size_t cap, const char *s)
{
    while (*s && *at + 1 < cap)
        buf[(*at)++] = *s++;
}

/* Build "/userconfig/<name>" or "/userconfig/<name>/password". */
static void user_dir_path(char *out, size_t cap, const char *name)
{
    size_t at = 0;
    append_str(out, &at, cap, USERCONFIG_DIR "/");
    append_str(out, &at, cap, name);
    out[at] = '\0';
}

static void user_password_path(char *out, size_t cap, const char *name)
{
    size_t at = 0;
    append_str(out, &at, cap, USERCONFIG_DIR "/");
    append_str(out, &at, cap, name);
    append_str(out, &at, cap, "/password");
    out[at] = '\0';
}

static user_entry_t *find_by_name(const char *name)
{
    for (int i = 0; i < W_MAX_USERS; i++)
        if (users[i].used && strcmp(users[i].name, name) == 0)
            return &users[i];
    return NULL;
}

static user_entry_t *find_by_uid(uint32_t uid)
{
    for (int i = 0; i < W_MAX_USERS; i++)
        if (users[i].used && users[i].uid == uid)
            return &users[i];
    return NULL;
}

static void fill(wuser_t *out, const user_entry_t *e)
{
    out->uid   = e->uid;
    out->roles = e->roles;
    strlcpy(out->name, e->name, sizeof(out->name));
}

/* ------------------------------------------------------------------ *
 *  Storage
 * ------------------------------------------------------------------ */

/* Create a directory if it is not already there. */
static int ensure_dir(const char *path)
{
    uint32_t ino;

    if (wfs_lookup(path, &ino) == 0)
        return 0;

    return wfs_create(path, WFS_TYPE_DIR, NULL);
}

/* Replace a file's contents wholesale. */
static int write_file(const char *path, const char *text, uint32_t len)
{
    uint32_t ino;
    int r = wfs_lookup(path, &ino);

    if (r == -W_ENOENT)
        r = wfs_create(path, WFS_TYPE_FILE, &ino);
    if (r < 0)
        return r;

    r = wfs_truncate(ino);
    if (r < 0)
        return r;

    if (len == 0)
        return 0;

    r = wfs_write(ino, 0, text, len);
    return (r == (int)len) ? 0 : (r < 0 ? r : -W_EIO);
}

/* Write the list of users. Deliberately holds no password material: that
 * lives in each user's own directory, under a stricter rule. */
static int save_user_list(void)
{
    char   text[W_MAX_USERS * 128];
    size_t at = 0;

    for (int i = 0; i < W_MAX_USERS; i++) {
        if (!users[i].used)
            continue;

        append_str(text, &at, sizeof(text), users[i].name);
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].uid);
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].roles);

        /* The login shell is a fourth field, written only when set so old
         * three-field lines stay valid and the file stays tidy. */
        if (users[i].shell[0]) {
            text[at++] = ':';
            append_str(text, &at, sizeof(text), users[i].shell);
        }

        text[at++] = '\n';
    }

    int r = ensure_dir(USERCONFIG_DIR);
    if (r < 0)
        return r;

    return write_file(USER_LIST_FILE, text, (uint32_t)at);
}

/* Write one user's password file, creating their directory if needed. */
static int save_password(const user_entry_t *e)
{
    char path[W_PATH_MAX + 1];
    char text[64];
    size_t at = 0;

    append_u64(text, &at, sizeof(text), e->salt);
    text[at++] = ':';
    append_u64(text, &at, sizeof(text), e->hash);
    text[at++] = '\n';

    int r = ensure_dir(USERCONFIG_DIR);
    if (r < 0)
        return r;

    user_dir_path(path, sizeof(path), e->name);
    r = ensure_dir(path);
    if (r < 0)
        return r;

    user_password_path(path, sizeof(path), e->name);
    return write_file(path, text, (uint32_t)at);
}

static void load_password(user_entry_t *e)
{
    char     path[W_PATH_MAX + 1];
    uint32_t ino;

    e->salt = 0;
    e->hash = 0;

    user_password_path(path, sizeof(path), e->name);
    if (wfs_lookup(path, &ino) != 0)
        return;                     /* no file means no password set */

    char text[64];
    int  got = wfs_read(ino, 0, text, sizeof(text) - 1);
    if (got <= 0)
        return;
    text[got] = '\0';

    const char *p = text;
    e->salt = parse_u64(&p);
    if (*p == ':') {
        p++;
        e->hash = parse_u64(&p);
    }
}

static void parse_list_line(const char *line)
{
    user_entry_t e;
    memset(&e, 0, sizeof(e));

    int n = 0;
    while (*line && *line != ':' && n < W_NAME_LEN)
        e.name[n++] = *line++;
    e.name[n] = '\0';

    if (*line != ':' || n == 0)
        return;                     /* malformed: skip the line */
    line++;

    e.uid = (uint32_t)parse_u64(&line);
    if (*line++ != ':')
        return;
    e.roles = (uint32_t)parse_u64(&line);

    /* An optional fourth field is the login shell.  Its absence (an old
     * three-field line) leaves shell empty, which means the default. */
    if (*line == ':') {
        line++;
        int s = 0;
        while (*line && *line != '\n' && s < W_SHELL_MAX)
            e.shell[s++] = *line++;
        e.shell[s] = '\0';
    }

    for (int i = 0; i < W_MAX_USERS; i++) {
        if (!users[i].used) {
            e.used = true;
            users[i] = e;
            load_password(&users[i]);
            if (e.uid >= next_uid)
                next_uid = e.uid + 1;
            return;
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Initialisation
 * ------------------------------------------------------------------ */

void user_init(void)
{
    memset(users, 0, sizeof(users));
    next_uid = 1;

    uint32_t ino;
    if (wfs_lookup(USER_LIST_FILE, &ino) == 0) {
        struct wfs_inode in;

        if (wfs_read_inode(ino, &in) == 0 && in.size > 0) {
            char text[W_MAX_USERS * 128];
            uint32_t want = in.size;

            if (want > sizeof(text) - 1)
                want = sizeof(text) - 1;

            int got = wfs_read(ino, 0, text, want);
            if (got > 0) {
                text[got] = '\0';

                char *line = text;
                for (int i = 0; i <= got; i++) {
                    if (text[i] == '\n' || text[i] == '\0') {
                        char saved = text[i];
                        text[i] = '\0';
                        if (line[0])
                            parse_list_line(line);
                        text[i] = saved;
                        line = text + i + 1;
                    }
                }
            }
        }
    }

    /* A system with no root is unusable, so make one.
     *
     * It gets the same password the shipped image gives root -- 1234, the one
     * in the documentation.  This path is reached only when /userconfig/users
     * is gone, which is a machine being recovered rather than a machine being
     * installed, and a recovery that ends at a login screen nobody can get
     * past is not a recovery.  Change it once you are in; passwd does that. */
    if (!find_by_uid(W_ROOT_UID)) {
        users[0].used  = true;
        users[0].uid   = W_ROOT_UID;
        users[0].roles = 0;         /* root bypasses roles entirely */
        users[0].salt  = make_salt();
        users[0].hash  = hash_password(users[0].salt, "1234");
        strlcpy(users[0].name, "root", sizeof(users[0].name));

        if (save_user_list() < 0)
            kprintf("user   : could not write %s\n", USER_LIST_FILE);
        if (save_password(&users[0]) < 0)
            kprintf("user   : could not write root's password\n");
    }

    int count = 0;
    for (int i = 0; i < W_MAX_USERS; i++)
        if (users[i].used)
            count++;

    kprintf("user   : %d user%s in %s, root %s\n",
            count, count == 1 ? "" : "s", USERCONFIG_DIR,
            find_by_uid(W_ROOT_UID)->hash ? "has a password"
                                          : "has no password yet");
}

/* ------------------------------------------------------------------ *
 *  Queries
 * ------------------------------------------------------------------ */

int user_by_name(const char *name, wuser_t *out)
{
    user_entry_t *e = find_by_name(name);
    if (!e)
        return -W_ENOENT;

    fill(out, e);
    return 0;
}

int user_by_uid(uint32_t uid, wuser_t *out)
{
    user_entry_t *e = find_by_uid(uid);
    if (!e)
        return -W_ENOENT;

    fill(out, e);
    return 0;
}

int user_list(wuser_t *out, int max)
{
    int n = 0;

    for (int i = 0; i < W_MAX_USERS && n < max; i++)
        if (users[i].used)
            fill(&out[n++], &users[i]);

    return n;
}

bool user_has_role(uint32_t uid, uint32_t role)
{
    if (uid == W_ROOT_UID)
        return true;                /* root does everything */

    user_entry_t *e = find_by_uid(uid);
    return e && (e->roles & role);
}

int user_authenticate(const char *name, const char *password, wuser_t *out)
{
    user_entry_t *e = find_by_name(name);
    if (!e)
        return -W_ENOENT;

    /* No password set: anyone naming the account gets in.  That is how root
     * starts, and why `passwd` is the first thing to run. */
    if (e->hash == 0) {
        fill(out, e);
        return 0;
    }

    if (!hash_equal(hash_password(e->salt, password), e->hash))
        return -W_EACCES;

    fill(out, e);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Changes
 * ------------------------------------------------------------------ */

int user_set_password(uint32_t actor, const char *name,
                      const char *old_password, const char *new_password)
{
    user_entry_t *e = find_by_name(name);
    if (!e)
        return -W_ENOENT;

    bool privileged = (actor == W_ROOT_UID) ||
                      user_has_role(actor, W_ROLE_USEREDITOR);

    if (!privileged) {
        /* Anyone else may only change their own, and must prove they know it. */
        if (e->uid != actor)
            return -W_EPERM;
        if (e->hash != 0 &&
            !hash_equal(hash_password(e->salt, old_password), e->hash))
            return -W_EACCES;
    }

    if (new_password[0] == '\0') {
        /* Clearing a password is allowed, but only deliberately. */
        e->salt = 0;
        e->hash = 0;
    } else {
        e->salt = make_salt();
        e->hash = hash_password(e->salt, new_password);
    }

    return save_password(e);
}

int user_set_roles(uint32_t actor, const char *name, uint32_t roles)
{
    if (actor != W_ROOT_UID && !user_has_role(actor, W_ROLE_USEREDITOR))
        return -W_EPERM;

    user_entry_t *e = find_by_name(name);
    if (!e)
        return -W_ENOENT;

    /* Root's roles are meaningless -- every check short-circuits on uid 0 --
     * and letting them be edited would only suggest they mattered. */
    if (e->uid == W_ROOT_UID)
        return -W_EPERM;

    e->roles = roles;
    return save_user_list();
}

/* Copy a user's login shell into `out`, falling back to the default when none
 * is set.  Always yields a usable path. */
int user_shell(uint32_t uid, char *out, size_t cap)
{
    user_entry_t *e = find_by_uid(uid);
    const char   *shell = (e && e->shell[0]) ? e->shell : DEFAULT_SHELL;

    strlcpy(out, shell, cap);
    return 0;
}

/* Set a user's login shell.  A user may set their own; root and holders of
 * W_ROLE_USEREDITOR may set anyone's.  An empty shell restores the default. */
int user_set_shell(uint32_t actor, const char *name, const char *shell)
{
    user_entry_t *e = find_by_name(name);
    if (!e)
        return -W_ENOENT;

    bool privileged = (actor == W_ROOT_UID) ||
                      user_has_role(actor, W_ROLE_USEREDITOR);

    if (!privileged && e->uid != actor)
        return -W_EPERM;

    if (strlen(shell) > W_SHELL_MAX)
        return -W_ENAMETOOLONG;

    strlcpy(e->shell, shell, sizeof(e->shell));
    return save_user_list();
}

int user_add(uint32_t actor, const char *name, const char *password,
             uint32_t roles)
{
    if (actor != W_ROOT_UID && !user_has_role(actor, W_ROLE_USEREDITOR))
        return -W_EPERM;

    if (name[0] == '\0' || strlen(name) > W_NAME_LEN)
        return -W_EINVAL;

    /* Names go straight into a path, so anything that could escape the home
     * or config directory has to be refused here. */
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == ':' || *p == '\n' || *p == '.')
            return -W_EINVAL;

    if (find_by_name(name))
        return -W_EEXIST;

    int slot = -1;
    for (int i = 0; i < W_MAX_USERS; i++) {
        if (!users[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -W_ENOSPC;

    user_entry_t *e = &users[slot];
    memset(e, 0, sizeof(*e));

    e->used  = true;
    e->uid   = next_uid++;
    e->roles = roles;
    strlcpy(e->name, name, sizeof(e->name));

    if (password[0]) {
        e->salt = make_salt();
        e->hash = hash_password(e->salt, password);
    }

    int r = save_user_list();
    if (r == 0)
        r = save_password(e);

    if (r < 0) {
        e->used = false;
        save_user_list();
        return r;
    }

    /* A user with nowhere to write is not much of a user. */
    char home[W_PATH_MAX + 1];
    size_t at = 0;

    append_str(home, &at, sizeof(home), "/home/");
    append_str(home, &at, sizeof(home), name);
    home[at] = '\0';

    ensure_dir(home);

    return (int)e->uid;
}
