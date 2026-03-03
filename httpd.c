#define _GNU_SOURCE
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PORT 40000

static void handle_request(int nfd)
{
    /* Convert socket descriptor to FILE* so we can read lines.
 *        Standard I/O uses streams; text streams are sequences of lines. */
    FILE *network = fdopen(nfd, "r");
    if (network == NULL)
    {
        perror("fdopen");
        close(nfd);
        return;
    }

    char *line = NULL;
    size_t size = 0;   /* required for getline */
    ssize_t num;

    /* Read ONLY the first request line for now (prepping for HTTP parsing). */
    num = getline(&line, &size, network);
    if (num > 0)
    {
        /* For now: just log it server-side */
        printf("Request: %s", line);
    }

    /* Do not close the socket until the client does.
 *        For now, just read and ignore the rest. */
    while ((num = getline(&line, &size, network)) >= 0)
    {
        (void)num;
    }

    free(line);
    fclose(network); /* also closes nfd */
}

static void run_service(int fd)
{
    while (1)
    {
        int nfd = accept_connection(fd);
        if (nfd != -1)
        {
            printf("Connection established\n");
            handle_request(nfd);
            printf("Connection closed\n");
        }
    }
}

int main(void)
{
    int fd = create_service(PORT);
    if (fd == -1)
    {
        perror("create_service");
        exit(1);
    }

    printf("listening on port: %d\n", PORT);
    run_service(fd);

    close(fd);
    return 0;
}
