/*
 * daoSetLatency: hold a CPU DMA latency request (/dev/cpu_dma_latency) while it runs,
 * so the CPUs stay out of deep sleep states. The request ends with the process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

static void ShowHelp(const char *argv0)
{
    printf("%s of " __DATE__ " at " __TIME__ "\n", argv0);
    printf("   Keeps the CPU wake-up latency at most <latency> us while it runs (Ctrl+C ends it).\n");
    printf("   usage:\n");
    printf("   %s <latency in us>\n", argv0);
    printf("   %s -h            display this message and exit\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-h") == 0)
    {
        ShowHelp(argv[0]);
        return 0;
    }
    if (argc != 2)
    {
        ShowHelp(argv[0]);
        return 2;
    }
    char *end;
    errno = 0;
    long value = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || errno != 0 || value < 0 || value > INT_MAX)
    {
        fprintf(stderr, "<latency>: '%s' is not a latency in us (0 or more)\n", argv[1]);
        return 2;
    }
    int l = (int)value;
    printf("setting latency to %d us\n", l);
    int fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (fd < 0)
    {
        perror("open /dev/cpu_dma_latency");
        return 1;
    }
    if (write(fd, &l, sizeof(l)) != sizeof(l))
    {
        perror("write to /dev/cpu_dma_latency");
        return 1;
    }
    while (1)
    {
        pause();      /* the request holds while the file is open */
    }
}
