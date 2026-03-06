#define _GNU_SOURCE
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define PORT 40000

static void reap_children(int signum)
{
   (void)signum;

   /* waitpid() with WNOHANG returns immediately if no child has exited. */
   int saved_errno = errno;

   while (waitpid(-1, NULL, WNOHANG) > 0)
   {
      /* reap all terminated children */
   }

   errno = saved_errno;
}

static void handle_request(int nfd)
{
   /* nfd is the connected socket returned by accept_connection */

   /* Convert socket descriptor to FILE* so we can read lines (HTTP is line-based). */
   FILE *network = fdopen(nfd, "r");
   if (network == NULL)
   {
      perror("fdopen");
      close(nfd);
      return;
   }

   char *line = NULL;
   size_t size = 0;
   ssize_t num;

   /* Read ONLY the first request line for now (preparing for HTTP parsing). */
   num = getline(&line, &size, network);
   if (num > 0)
   {
      printf("Request: %s", line);
   }

   /* Do not close the socket until the client does. */
   while ((num = getline(&line, &size, network)) >= 0)
   {
      (void)num;
   }

   free(line);

   /* Closing the stream also closes the underlying socket descriptor. */
   fclose(network);
}

static void run_service(int fd)
{
   /* Continuously accept client connections. For each connection, fork a child
 *       process to handle the request while the parent continues accepting. */
   while (1)
   {
      int nfd = accept_connection(fd);
      if (nfd == -1)
      {
         continue;
      }

      pid_t pid = fork();
      if (pid < 0)
      {
         perror("fork");
         close(nfd);
         continue;
      }

      if (pid == 0)
      {
         /* Child process: handle the request. */
         close(fd); /* child does not accept new connections */
         handle_request(nfd);
         _exit(0);
      }

      /* Parent process: close connected socket and wait for next request. */
      close(nfd);
   }
}

int main(void)
{
   /* Register signal handler to reap terminated children. */
   if (signal(SIGCHLD, reap_children) == SIG_ERR)
   {
      perror("signal");
      exit(1);
   }

   /* Create server socket and bind to PORT. */
   int fd = create_service(PORT);
   if (fd == -1)
   {
      perror("create_service");
      exit(1);
   }

   printf("listening on port: %d\n", PORT);

   /* Start accepting connections. */
   run_service(fd);

   close(fd);
   return 0;
}
