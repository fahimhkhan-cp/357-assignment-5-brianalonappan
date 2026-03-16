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
#include <sys/stat.h>
#include <fcntl.h>

#define READ_BUF_SIZE 4096
#define CGI_MAX_ARGS  32

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
 *  *       Each header line ends with \r\n and the header ends with a blank line. */

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

/* Parse: METHOD PATH VERSION
 *  *    Split by spaces by inserting '\0' terminators. */
static int parse_request_line(char *line, char **method, char **path, char **version)
{
   char *space1;
   char *space2;
   char *p;

   *method = NULL;
   *path = NULL;
   *version = NULL;

   if (line == NULL || line[0] == '\0')
   {
      return -1;
   }

   *method = line;

   /* Find first space */
   space1 = NULL;
   for (p = line; *p != '\0'; p++)
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
   space2 = NULL;
   for (p = *path; *p != '\0'; p++)
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

/* Parse cgi-like request path.
 *  *    Example: /cgi-like/ls?-l&index.html
 *   *       Returns 0 on success, -1 on failure. */
static int parse_cgi_path(char *path,
                          char **prog,
                          char *argv[],
                          int *argc_out)
{
   char *p;
   char *qmark;
   int argc;

   *prog = NULL;
   *argc_out = 0;

   /* Must start with /cgi-like/ */
   if (strncmp(path, "/cgi-like/", 10) != 0)
   {
      return -1;
   }

   /* program name begins right after /cgi-like/ */
   p = path + 10;
   if (*p == '\0')
   {
      return -1;
   }

   /* Split program and query string */
   qmark = strchr(p, '?');
   if (qmark != NULL)
   {
      *qmark = '\0';
      qmark++;
   }

   *prog = p;

   /* argv[0] should be the program name */
   argc = 0;
   argv[argc++] = *prog;

   /* No args if there is no '?' or it's empty */
   if (qmark == NULL || *qmark == '\0')
   {
      argv[argc] = NULL;
      *argc_out = argc;
      return 0;
   }

   /* Split args on '&' */
   while (*qmark != '\0' && argc < CGI_MAX_ARGS - 1)
   {
      argv[argc++] = qmark;

      /* Find next '&' */
      while (*qmark != '\0' && *qmark != '&')
      {
         qmark++;
      }

      if (*qmark == '&')
      {
         *qmark = '\0';
         qmark++;
      }
   }

   argv[argc] = NULL;
   *argc_out = argc;
   return 0;
}

static void build_temp_filename(char *out, size_t outsz, pid_t pid)
{
   /* Build: cgi-out-<pid>.tmp without snprintf. */
   const char *prefix = "cgi-out-";
   const char *suffix = ".tmp";
   char rev[32];
   char pidbuf[32];
   int r = 0;
   int i = 0;
   size_t pos = 0;
   size_t n;
   size_t v;

   /* copy prefix */
   n = strlen(prefix);
   if (pos + n + 1 < outsz)
   {
      memcpy(out + pos, prefix, n);
      pos += n;
   }

   /* convert pid to decimal */
   v = (size_t)pid;
   if (v == 0)
   {
      pidbuf[i++] = '0';
   }
   else
   {
      while (v > 0 && r < (int)sizeof(rev))
      {
         rev[r++] = (char)('0' + (v % 10));
         v /= 10;
      }
      while (r > 0)
      {
         pidbuf[i++] = rev[--r];
      }
   }

   if (pos + (size_t)i + 1 < outsz)
   {
      memcpy(out + pos, pidbuf, (size_t)i);
      pos += (size_t)i;
   }

   /* copy suffix */
   n = strlen(suffix);
   if (pos + n + 1 < outsz)
   {
      memcpy(out + pos, suffix, n);
      pos += n;
   }

   out[pos] = '\0';
}

static void serve_static_file(int nfd, const char *reqpath, const char *method)
{
   /* reqpath starts with a leading '/' per HTTP request; remove it for local path */
   const char *p = reqpath;
   if (*p == '/')
   {
      p++;
   }

   /* Require a filename; reject empty path */
   if (*p == '\0')
   {
      const char *body = "<html><body>400 Bad Request</body></html>\n";
      send_response(nfd, "400 Bad Request", body, 1);
      return;
   }

   /* Prevent directory traversal attempts */
   if (strstr(p, "..") != NULL)
   {
      const char *body = "<html><body>403 Permission Denied</body></html>\n";
      send_response(nfd, "403 Permission Denied", body, 1);
      return;
   }

   /* Stat the file to get size and check existence/permissions */
   struct stat sb;
   if (stat(p, &sb) < 0)
   {
      /* File not found or other stat error */
      const char *body = "<html><body>404 Not Found</body></html>\n";
      send_response(nfd, "404 Not Found", body, 1);
      return;
   }

   /* Ensure it's a regular file */
   if (!S_ISREG(sb.st_mode))
   {
      const char *body = "<html><body>403 Permission Denied</body></html>\n";
      send_response(nfd, "403 Permission Denied", body, 1);
      return;
   }

   /* Try to open the file for reading */
   int fd = open(p, O_RDONLY);
   if (fd < 0)
   {
      /* Could be permission denied or other open error */
      const char *body = "<html><body>403 Permission Denied</body></html>\n";
      send_response(nfd, "403 Permission Denied", body, 1);
      return;
   }

   /* Build and send headers */
   (void)write_all(nfd, "HTTP/1.0 200 OK\r\n", 17);
   (void)write_all(nfd, "Content-Type: text/html\r\n", 25);
   (void)write_all(nfd, "Content-Length: ", 16);
   write_size_t_as_decimal(nfd, (size_t)sb.st_size);
   (void)write_all(nfd, "\r\n", 2);
   (void)write_all(nfd, "\r\n", 2);

   /* If method is HEAD, do not send body */
   if (strcmp(method, "HEAD") == 0)
   {
      close(fd);
      return;
   }

   /* Send file contents in chunks */
   char buf[READ_BUF_SIZE];
   ssize_t r;
   while ((r = read(fd, buf, sizeof(buf))) > 0)
   {
      if (write_all(nfd, buf, (size_t)r) < 0)
      {
         /* Client closed or write error; stop sending */
         break;
      }
   }

   if (r < 0)
   {
      /* Read error */
      /* Attempt to send a 500 if possible (note: we may have already sent headers) */
   }

   close(fd);
}

static void serve_cgi_like(int nfd, char *path, const char *method)
{
   char *prog;
   char *argv[CGI_MAX_ARGS];
   int argc;
   pid_t pid;
   int status;
   char exec_path[256];
   char tmpfile[256];

   /* Prevent directory traversal attempts */
   if (strstr(path, "..") != NULL)
   {
      const char *body = "<html><body>403 Permission Denied</body></html>\n";
      send_response(nfd, "403 Permission Denied", body, 1);
      return;
   }

   if (parse_cgi_path(path, &prog, argv, &argc) != 0)
   {
      const char *body = "<html><body>400 Bad Request</body></html>\n";
      send_response(nfd, "400 Bad Request", body, 1);
      return;
   }

   /* Program name should not contain '/' */
   if (strchr(prog, '/') != NULL)
   {
      const char *body = "<html><body>403 Permission Denied</body></html>\n";
      send_response(nfd, "403 Permission Denied", body, 1);
      return;
   }

   /* Build exec path: ./cgi-like/<prog> */
   {
      const char *prefix = "./cgi-like/";
      size_t pre = strlen(prefix);
      size_t prg = strlen(prog);

      if (pre + prg + 1 > sizeof(exec_path))
      {
         const char *body = "<html><body>500 Internal Error</body></html>\n";
         send_response(nfd, "500 Internal Error", body, 1);
         return;
      }

      memcpy(exec_path, prefix, pre);
      memcpy(exec_path + pre, prog, prg);
      exec_path[pre + prg] = '\0';
   }

   /* Fork to execute the cgi-like program */
   pid = fork();
   if (pid < 0)
   {
      const char *body = "<html><body>500 Internal Error</body></html>\n";
      send_response(nfd, "500 Internal Error", body, 1);
      return;
   }

   if (pid == 0)
   {
      /* Child process: redirect stdout to a temp file, then exec the program. */
      int outfd;

      close(nfd);

      build_temp_filename(tmpfile, sizeof(tmpfile), getpid());

      outfd = open(tmpfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (outfd < 0)
      {
         _exit(1);
      }

      if (dup2(outfd, STDOUT_FILENO) < 0)
      {
         close(outfd);
         _exit(1);
      }

      close(outfd);

      execv(exec_path, argv);

      /* If exec returns, it failed */
      _exit(1);
   }

   /* Parent process: wait for program to complete */
   if (waitpid(pid, &status, 0) < 0)
   {
      const char *body = "<html><body>500 Internal Error</body></html>\n";
      send_response(nfd, "500 Internal Error", body, 1);
      return;
   }

   build_temp_filename(tmpfile, sizeof(tmpfile), pid);

   /* If exec failed, treat as internal error */
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
   {
      (void)unlink(tmpfile);
      {
         const char *body = "<html><body>500 Internal Error</body></html>\n";
         send_response(nfd, "500 Internal Error", body, 1);
      }
      return;
   }

   /* Stat the output file to get Content-Length */
   struct stat sb;
   if (stat(tmpfile, &sb) < 0)
   {
      (void)unlink(tmpfile);
      {
         const char *body = "<html><body>500 Internal Error</body></html>\n";
         send_response(nfd, "500 Internal Error", body, 1);
      }
      return;
   }

   /* Send headers */
   (void)write_all(nfd, "HTTP/1.0 200 OK\r\n", 17);
   (void)write_all(nfd, "Content-Type: text/html\r\n", 25);
   (void)write_all(nfd, "Content-Length: ", 16);
   write_size_t_as_decimal(nfd, (size_t)sb.st_size);
   (void)write_all(nfd, "\r\n", 2);
   (void)write_all(nfd, "\r\n", 2);

   /* HEAD returns header only */
   if (strcmp(method, "HEAD") == 0)
   {
      (void)unlink(tmpfile);
      return;
   }

   /* Send the temp file contents */
   int fd = open(tmpfile, O_RDONLY);
   if (fd < 0)
   {
      (void)unlink(tmpfile);
      {
         const char *body = "<html><body>500 Internal Error</body></html>\n";
         send_response(nfd, "500 Internal Error", body, 1);
      }
      return;
   }

   char buf[READ_BUF_SIZE];
   ssize_t r;
   while ((r = read(fd, buf, sizeof(buf))) > 0)
   {
      if (write_all(nfd, buf, (size_t)r) < 0)
      {
         /* Client closed or write error; stop sending */
         break;
      }
   }

   close(fd);
   (void)unlink(tmpfile);
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

   {
      char *method = NULL;
      char *path = NULL;
      char *version = NULL;

      if (parse_request_line(line, &method, &path, &version) != 0)
      {
         const char *body = "<html><body>400 Bad Request</body></html>\n";
         send_response(nfd, "400 Bad Request", body, 1);
      }
      else if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
      {
         const char *body = "<html><body>501 Not Implemented</body></html>\n";
         send_response(nfd, "501 Not Implemented", body, 1);
      }
      else
      {
         /* cgi-like requests are handled separately from static files */
         if (strncmp(path, "/cgi-like/", 10) == 0)
         {
            serve_cgi_like(nfd, path, method);
         }
         else
         {
            /* Valid request line and supported method: serve static file (or HEAD). */
            serve_static_file(nfd, path, method);
         }
      }
   }

   /* Do not close the socket until the client does.
 *  *       For now, read and ignore remaining lines until EOF. */
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
 *  *       process to handle the request while the parent continues accepting. */
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


static int parse_port(const char *s, int *port_out)
{
   /* Parse a decimal port number without scanf/sscanf. */
   long port = 0;
   const char *p = s;

   if (s == NULL || *s == '\0')
   {
      return -1;
   }

   while (*p != '\0')
   {
      if (*p < '0' || *p > '9')
      {
         return -1;
      }
      port = port * 10 + (*p - '0');
      if (port > 65535)
      {
         return -1;
      }
      p++;
   }

   if (port < 1024 || port > 65535)
   {
      return -1;
   }

   *port_out = (int)port;
   return 0;
}

int main(int argc, char **argv)
{
   /* Register signal handler to reap terminated children. */
   if (signal(SIGCHLD, reap_children) == SIG_ERR)
   {
      perror("signal");
      exit(1);
   }

   /* Ignore SIGPIPE so client disconnects do not kill the process. */
   if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
   {
      perror("signal");
      exit(1);
   }

   /* Parse port argument. */
   if (argc != 2)
   {
      const char *msg = "usage: ./httpd <port>\n";
      (void)write(STDERR_FILENO, msg, strlen(msg));
      exit(1);
   }

   int port;
   if (parse_port(argv[1], &port) != 0)
   {
      const char *msg = "error: port must be an integer between 1024 and 65535\n";
      (void)write(STDERR_FILENO, msg, strlen(msg));
      exit(1);
   }

   /* Create server socket and bind to port. */
   int fd = create_service(port);
   if (fd == -1)
   {
      perror("create_service");
      exit(1);
   }

   printf("listening on port: %d\n", port);

   /* Start accepting connections. */
   run_service(fd);

   close(fd);
   return 0;
}
