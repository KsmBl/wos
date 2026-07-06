/* socktest -- prove that two processes can talk over a local socket.
 *
 * Not a command anyone needs, the way `hello` is not: it exists so that the
 * socket calls are exercised end to end, through the real syscalls and between
 * two real processes, rather than only inside the kernel where the boot-time
 * self-test reaches them.
 *
 * It runs both halves.  With no arguments it is the server: it listens on an
 * address, starts a copy of itself as the client, waits with wpoll(), accepts,
 * and reads the message.  `socktest client` is the other half, which connects,
 * opens a file, makes a page of shared memory, and sends the message together
 * with both descriptors.
 *
 * The interesting lines are the last two from the server.  It reads the file
 * descriptor the client passed, in its own process, out of a file it never
 * opened.  Then it maps the shared memory and finds what the client wrote
 * there -- not a copy of it, the same bytes in the same physical page.  Those
 * two mechanisms together are how a display protocol hands over a buffer of
 * pixels: the descriptor travels and the pixels do not.
 */

#include <wkernel.h>

/* On /ramdisk because an address is not a file and leaving one on the disk
 * would be litter -- the name is gone when the listener closes either way. */
#define ADDRESS "/ramdisk/socktest.sock"
#define TOKEN   "/ramdisk/socktest.txt"

static int client(void)
{
    int fd = wconnect(ADDRESS);
    if (fd < 0) {
        wfprintf(W_STDERR, "client: connect: %s\n", wstrerror(-fd));
        return 1;
    }

    /* Something worth passing: a file this process opened and the server did
     * not, positioned at the start so the read on the far side sees it all. */
    int file = wopen(TOKEN, W_O_RDWR | W_O_CREAT | W_O_TRUNC);
    if (file < 0) {
        wfprintf(W_STDERR, "client: open: %s\n", wstrerror(-file));
        return 1;
    }
    wwrite(file, "pixels would go here", 20);
    wlseek(file, 0, W_SEEK_SET);

    /* And a page of shared memory with something recognisable in it.  This is
     * a wl_shm pool in miniature: the client draws, and only the descriptor
     * goes down the socket. */
    int shm = wshmopen(4096);
    if (shm < 0) {
        wfprintf(W_STDERR, "client: shmopen: %s\n", wstrerror(-shm));
        return 1;
    }

    char *pool = wshmmap(shm);
    if (!pool) {
        wfprintf(W_STDERR, "client: could not map the shared memory\n");
        return 1;
    }
    strlcpy(pool, "these bytes were never copied", 4096);
    wprintf("client: shared %d bytes, wrote into them at %p\n",
            wshmsize(shm), (void *)pool);

    int    fds[2] = { file, shm };
    wmsg_t msg    = { (void *)"take these", 10, 2, fds };

    int n = wsend(fd, &msg);
    wprintf("client: sent %d bytes and %d descriptors\n", n, msg.fd_count);

    /* The server has its own references now, so closing these is harmless --
     * which is the whole point of passing a descriptor rather than a name.
     * The shared page survives even this process exiting, for as long as the
     * server still has it. */
    wclose(file);
    wshmunmap(pool);
    wclose(shm);

    char   reply[64];
    wmsg_t back = { reply, sizeof(reply) - 1, 0, 0 };

    n = wrecv(fd, &back);
    if (n > 0) {
        reply[n] = '\0';
        wprintf("client: the server said \"%s\"\n", reply);
    }

    wclose(fd);
    return 0;
}

static int server(void)
{
    int listener = wlisten(ADDRESS);
    if (listener < 0) {
        wfprintf(W_STDERR, "server: listen: %s\n", wstrerror(-listener));
        return 1;
    }
    wprintf("server: listening on %s\n", ADDRESS);

    char *const args[] = { "socktest", "client", 0 };
    int pid = wspawn("/app/socktest/launch", args);
    if (pid < 0) {
        wfprintf(W_STDERR, "server: spawn: %s\n", wstrerror(-pid));
        return 1;
    }

    /* Wait for the client rather than blocking in waccept(), so that the wait
     * is the same one a server with several clients would be doing. */
    wpollfd_t watch = { listener, W_POLLIN, 0 };
    int ready = wpoll(&watch, 1, 3000);
    wprintf("server: wpoll returned %d, revents %d\n", ready, watch.revents);

    int c = waccept(listener);
    if (c < 0) {
        wfprintf(W_STDERR, "server: accept: %s\n", wstrerror(-c));
        return 1;
    }

    char   buf[64];
    int    fds[4];
    wmsg_t msg = { buf, sizeof(buf) - 1, 4, fds };

    int n = wrecv(c, &msg);
    buf[n > 0 ? n : 0] = '\0';
    wprintf("server: got %d bytes \"%s\" and %d descriptor(s)\n",
            n, buf, msg.fd_count);

    if (msg.fd_count > 0) {
        char through[64];
        int  m = wread(fds[0], through, sizeof(through) - 1);

        through[m > 0 ? m : 0] = '\0';
        wprintf("server: the passed descriptor reads \"%s\"\n", through);
        wclose(fds[0]);
    }

    if (msg.fd_count > 1) {
        /* wshmsize() before trusting anything in it: the size is the one fact
         * about a buffer that the sender cannot lie about, and it is what
         * bounds every read of the pixels inside. */
        int   size = wshmsize(fds[1]);
        char *pool = wshmmap(fds[1]);

        if (pool)
            wprintf("server: the shared %d bytes at %p say \"%s\"\n",
                    size, (void *)pool, pool);
        else
            wprintf("server: could not map the shared memory\n");

        if (pool)
            wshmunmap(pool);
        wclose(fds[1]);
    }

    wmsg_t reply = { (void *)"thank you", 9, 0, 0 };
    wsend(c, &reply);

    int status = 0;
    wwait(pid, &status);

    wclose(c);
    wclose(listener);
    wunlink(TOKEN);

    wprintf("server: the client exited with %d\n", status);
    return status == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "client") == 0)
        return client();

    return server();
}
