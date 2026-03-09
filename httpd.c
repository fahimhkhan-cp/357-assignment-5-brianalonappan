#define _GNU_SOURCE
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#define PORT 40000

static void reap_children(int signum)
{
   (void)signum;

   int saved_errno = errno;

   while (waitpid(-1, NULL, WNOHANG) > 0)
   {
      /* reap all terminated children */
   }

   errno = saved_errno;
}

static ssize_t write_all(int fd, const void *buf, size_t count)
{
   const char *p = buf;
   size_t total = 0;

   while (total < count)
   {
      ssize_t n = write(fd, p + total, count - total);
      if (n < 0)
      {
         return -1;
      }
      total += (size_t)n;
   }

   return (ssize_t)total;
}

static void write_size_t_as_decimal(int fd, size_t value)
{
   /* Convert a non-negative integer to decimal ASCII without snprintf. */
   char rev[32];
   char out[32];
   int r = 0;
   int o = 0;

   if (value == 0)
   {
      out[o++] = '0';
      (void)write_all(fd, out, (size_t)o);
      return;
   }

   while (value > 0)
   {
      rev[r++] = (char)('0' + (value % 10));
      value /= 10;
   }

   while (r > 0)
   {
      out[o++] = rev[--r];
   }

   (void)write_all(fd, out, (size_t)o);
}

static void send_response(int nfd, const char *status, const char *body, int send_body)
{
   /* Send a minimal HTTP/1.0 response with Content-Type and Content-Length.
 *       Each header line ends with \r\n and the header ends with a blank line. */

   size_t body_len = 0;
   if (body != NULL)
   {
      body_len = strlen(body);
   }

   (void)write_all(nfd, "HTTP/1.0 ", 9);
   (void)write_all(nfd, status, strlen(status));
   (void)write_all(nfd, "\r\n", 2);

   (void)write_all(nfd, "Content-Type: text/html\r\n", 25);

   (void)write_all(nfd, "Content-Length: ", 16);
   write_size_t_as_decimal(nfd, body_len);
   (void)write_all(nfd, "\r\n", 2);

   (void)write_all(nfd, "\r\n", 2);

   if (send_body && body != NULL && body_len > 0)
   {
      (void)write_all(nfd, body, body_len);
   }
}

static int parse_request_line(char *line, char **method, char **path, char **version)
{
   /* Parse Split by spaces by inserting '\0' terminators. */

   *method = NULL;
   *path = NULL;
   *version = NULL;

   if (line == NULL || line[0] == '\0')
   {
      return -1;
   }

   *method = line;

   /* Find first space */
   char *space1 = NULL;
   for (char *p = line; *p != '\0'; p++)
   {
      if (*p == ' ')
      {
         space1 = p;
         break;
      }
   }
   if (space1 == NULL)
   {
      return -1;
   }

   *space1 = '\0';
   *path = space1 + 1;

   if (**path == '\0')
   {
      return -1;
   }

   /* Find second space */
   char *space2 = NULL;
   for (char *p = *path; *p != '\0'; p++)
   {
      if (*p == ' ')
      {
         space2 = p;
         break;
      }
   }
   if (space2 == NULL)
   {
      return -1;
   }

   *space2 = '\0';
   *version = space2 + 1;

   if (**version == '\0')
   {
      return -1;
   }

   return 0;
}

static void handle_request(int nfd)
{
   /* nfd is the connected socket returned by accept_connection */

   /* Convert socket descriptor to FILE* so we can read lines . */
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

   /* Read the request line: TYPE filename HTTP/version */
   num = getline(&line, &size, network);
   if (num <= 0)
   {
      free(line);
      fclose(network);
      return;
   }

   printf("Request: %s", line);

   /* Remove trailing newline and optional carriage return so parsing is clean. */
   if (num > 0 && line[num - 1] == '\n')
   {
      line[num - 1] = '\0';
      num--;
   }
   if (num > 0 && line[num - 1] == '\r')
   {
      line[num - 1] = '\0';
   }

   char *method = NULL;
   char *path = NULL;
   char *version = NULL;

   if (parse_request_line(line, &method, &path, &version) != 0)
   {
      const char *body =
         "<html><body>400 Bad Request</body></html>\n";
      send_response(nfd, "400 Bad Request", body, 1);
   }
   else if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
   {
      const char *body =
         "<html><body>501 Not Implemented</body></html>\n";
      send_response(nfd, "501 Not Implemented", body, 1);
   }
   else
   {
      /* Valid request line and supported method.
 *          File-serving will be implemented in the next commits. */
      const char *body =
         "<html><body>200 OK</body></html>\n";

      if (strcmp(method, "HEAD") == 0)
      {
         send_response(nfd, "200 OK", body, 0); /* header only */
      }
      else
      {
         send_response(nfd, "200 OK", body, 1); /* header + body */
      }
   }

   /* Do not close the socket until the client does.
 *       For now, read and ignore remaining lines until EOF. */
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
