/* The adapter registry. See netdev.h. */

#include "netdev.h"
#include "string.h"
#include "kprintf.h"

static netdev_t *devices[NETDEV_MAX];
static int       count;
static netdev_t *current;

void netdev_register(netdev_t *dev)
{
    if (!dev || count >= NETDEV_MAX)
        return;

    for (int i = 0; i < count; i++)
        if (devices[i] == dev)
            return;                    /* already in, from a second start */

    devices[count++] = dev;

    if (!current)
        current = dev;

    kprintf("net    : %s registered, mac %x:%x:%x:%x:%x:%x%s\n",
            dev->name, dev->mac[0], dev->mac[1], dev->mac[2],
            dev->mac[3], dev->mac[4], dev->mac[5],
            dev->wireless ? " (wireless)" : "");
}

netdev_t *netdev_default(void)
{
    return current;
}

void netdev_set_default(netdev_t *dev)
{
    for (int i = 0; i < count; i++)
        if (devices[i] == dev) {
            current = dev;
            return;
        }
}

int netdev_count(void)
{
    return count;
}

netdev_t *netdev_at(int index)
{
    if (index < 0 || index >= count)
        return NULL;
    return devices[index];
}

netdev_t *netdev_find(const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcmp(devices[i]->name, name) == 0)
            return devices[i];
    return NULL;
}

int netdev_send(const void *frame, uint32_t len)
{
    netdev_t *dev = current;

    if (!dev || !dev->send)
        return -1;
    return dev->send(dev, frame, len);
}

uint32_t netdev_poll(void *out, uint32_t cap)
{
    netdev_t *dev = current;

    if (!dev || !dev->poll)
        return 0;
    return dev->poll(dev, out, cap);
}

bool netdev_link_up(void)
{
    netdev_t *dev = current;

    if (!dev)
        return false;
    /* An adapter that does not say is assumed up: a wired card with no way to
     * report carrier is more useful treated as connected than as never
     * connected. */
    if (!dev->link_up)
        return true;
    return dev->link_up(dev);
}
