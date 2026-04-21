/* The user database.
 *
 * Stored as text at /etc/users, one record per line:
 *
 *     name:uid:roles:salt:hash
 *
 * Text rather than a packed struct so the file can be read with `cat` while
 * debugging -- though only by root, since /etc is not writable by anyone else
 * and the hashes are useless without the salt anyway.
 */

#include "user.h"
#include "wfs_kernel.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

#define USER_FILE "/etc/users"

typedef struct {
    bool     used;
    char     name[W_NAME_LEN + 1];
    uint32_t uid;
    uint32_t roles;
    uint64_t salt;
    uint64_t hash;
} user_entry_t;

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
 * What actually protects passwords here is that no program can read the file:
 * see the comment at the top of user.h. */
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

/* There is no entropy source in WOS, so the salt comes from the timer and the
 * address of a stack object.  Weak, but it varies between users, which is all
 * a salt has to do here. */
static uint64_t make_salt(void)
{
    uint64_t marker = (uint64_t)(uintptr_t)&next_uid;
    return (pit_ticks() * 0x9E3779B97F4A7C15UL) ^ (marker * 0xBF58476D1CE4E5B9UL)
           ^ ((uint64_t)next_uid << 32);
}

/* ------------------------------------------------------------------ *
 *  Parsing and serialising
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

/* Write the whole table back. Small enough that rewriting beats patching. */
static int save_users(void)
{
    char   text[W_MAX_USERS * 96];
    size_t at = 0;

    for (int i = 0; i < W_MAX_USERS; i++) {
        if (!users[i].used)
            continue;

        for (const char *n = users[i].name; *n && at + 1 < sizeof(text); n++)
            text[at++] = *n;
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].uid);
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].roles);
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].salt);
        text[at++] = ':';
        append_u64(text, &at, sizeof(text), users[i].hash);
        text[at++] = '\n';
    }

    uint32_t ino;
    int r = wfs_lookup(USER_FILE, &ino);

    if (r == -W_ENOENT) {
        /* /etc may not exist yet on a freshly built image. */
        uint32_t dir;
        if (wfs_lookup("/etc", &dir) == -W_ENOENT)
            wfs_create("/etc", WFS_TYPE_DIR, NULL);

        r = wfs_create(USER_FILE, WFS_TYPE_FILE, &ino);
    }
    if (r < 0)
        return r;

    r = wfs_truncate(ino);
    if (r < 0)
        return r;

    r = wfs_write(ino, 0, text, (uint32_t)at);
    return (r == (int)at) ? 0 : (r < 0 ? r : -W_EIO);
}

static void parse_line(const char *line)
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

    e.uid   = (uint32_t)parse_u64(&line);
    if (*line++ != ':') return;
    e.roles = (uint32_t)parse_u64(&line);
    if (*line++ != ':') return;
    e.salt  = parse_u64(&line);
    if (*line++ != ':') return;
    e.hash  = parse_u64(&line);

    for (int i = 0; i < W_MAX_USERS; i++) {
        if (!users[i].used) {
            e.used = true;
            users[i] = e;
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
    if (wfs_lookup(USER_FILE, &ino) == 0) {
        struct wfs_inode in;

        if (wfs_read_inode(ino, &in) == 0 && in.size > 0) {
            char text[W_MAX_USERS * 96];
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
                            parse_line(line);
                        text[i] = saved;
                        line = text + i + 1;
                    }
                }
            }
        }
    }

    /* A system with no root is unusable, so make one.  It starts without a
     * password: there is no sensible default to ship, and a known one would
     * be worse than none. */
    if (!find_by_uid(W_ROOT_UID)) {
        users[0].used  = true;
        users[0].uid   = W_ROOT_UID;
        users[0].roles = 0;         /* root bypasses roles entirely */
        users[0].salt  = 0;
        users[0].hash  = 0;         /* 0 means no password set */
        strlcpy(users[0].name, "root", sizeof(users[0].name));

        if (save_users() < 0)
            kprintf("user   : could not write %s\n", USER_FILE);
    }

    int count = 0;
    for (int i = 0; i < W_MAX_USERS; i++)
        if (users[i].used)
            count++;

    kprintf("user   : %d user%s, root %s\n",
            count, count == 1 ? "" : "s",
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
                      user_has_role(actor, W_ROLE_USERADMIN);

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

    return save_users();
}

int user_add(uint32_t actor, const char *name, const char *password,
             uint32_t roles)
{
    if (actor != W_ROOT_UID && !user_has_role(actor, W_ROLE_USERADMIN))
        return -W_EPERM;

    if (name[0] == '\0' || strlen(name) > W_NAME_LEN)
        return -W_EINVAL;

    /* Names go straight into a path, so anything that could escape the home
     * directory has to be refused here. */
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

    int r = save_users();
    if (r < 0) {
        e->used = false;
        return r;
    }

    /* A user with nowhere to write is not much of a user. */
    char home[W_PATH_MAX + 1];
    size_t at = 0;

    for (const char *s = "/home/"; *s; s++)
        home[at++] = *s;
    for (const char *s = name; *s; s++)
        home[at++] = *s;
    home[at] = '\0';

    uint32_t ino;
    if (wfs_lookup(home, &ino) == -W_ENOENT)
        wfs_create(home, WFS_TYPE_DIR, NULL);

    return (int)e->uid;
}
