/* ps -- list processes and their memory use. */

#include <wkernel.h>

#define MAX_PROCS 32

int main(int argc, char **argv)
{
    wprocmem_t procs[MAX_PROCS];

    int n = wproclist(procs, MAX_PROCS);
    if (n < 0) {
        wfprintf(W_STDERR, "ps: %s\n", wstrerror(-n));
        return 1;
    }

    wprintf("%5s %-12s %9s %8s %8s %8s %8s %3s\n",
            "PID", "NAME", "RESIDENT", "CODE", "DATA", "HEAP", "STACK", "THR");

    for (int i = 0; i < n; i++)
        wprintf("%5d %-12s %9s %8s %8s %8s %8s %3d\n",
                procs[i].pid,
                procs[i].name[0] ? procs[i].name : "?",
                whuman(procs[i].resident_bytes),
                whuman(procs[i].code_bytes),
                whuman(procs[i].data_bytes),
                whuman(procs[i].heap_bytes),
                whuman(procs[i].stack_bytes),
                procs[i].thread_count);

    return 0;
}
