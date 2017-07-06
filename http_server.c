#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <signal.h>
#include <regex.h>
#include <fcntl.h>
#include <sys/stat.h>


#define PORT "3001"
#define CONNECTION_POOL 10
#define REQUEST_BUFFER_SIZE 500


void sigchld_handler(int s)
{
  // waitpid() might override errno, so we save and restore it
  int saved_errno = errno;

  while(waitpid(-1, NULL, WNOHANG) > 0);

  errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6
void *get_in_addr(struct sockaddr *sa)
{
  if(sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int read_request(int fd, char *buffer)
{
  #define EOL "\r\n"
  #define EOL_SIZE 2

  char *p = buffer;
  int eol_matched = 0;

  while(recv(fd, p, 1, 0) != 0) {
    if(*p == EOL[eol_matched]) {
      ++eol_matched;

      if(eol_matched == EOL_SIZE) {
        *(p + 1 - EOL_SIZE) = '\0'; // end the string
        return(strlen(buffer));
      }
    } else {
      eol_matched = 0;
    }
    p++;
  }

  return 0;
}

void error(int fd, char *msg)
{
  perror(msg);
  close(fd);
  exit(0);
}

void response(int fd, char *msg)
{
  int ln = strlen(msg);
  if(send(fd, msg, ln, 0) == -1) {
    error(fd, "Error during response\n");
  }
}

void error_response(int fd, int code, char *msg)
{
  perror(msg);

  char *string;
  sprintf(string, "HTTP/1.1 %d %s\r\n", code, msg);
  response(fd, string);
}

int get_path_from_request(char *ptr, char *path)
{
  // Routing
  char *index_route = "/";
  if(strcmp(&ptr[0], index_route) == 0) {
    strncpy(path, "index.html", 10);
  } else {
    regex_t regex;
    int reti;
    char *pattern = "/\\(.*\\.[a-zA-Z0-9]\\{1,4\\}\\)";
    size_t nmatch = 2;
    regmatch_t pmatch[2];

    reti = regcomp(&regex, pattern, 0);
    if(reti != 0) {
      regfree(&regex);
      return 1;
    }

    reti = regexec(&regex, ptr, nmatch, pmatch, 0);
    if(reti) {
      printf("Can't parse URI. Failed to match '%s' with '%s',returning %d.\n", ptr, pattern, reti);
      regfree(&regex);
      return 2; 
    }

    ptr = &ptr[pmatch[1].rm_so];
    strncpy(path, &ptr[pmatch[1].rm_so - 1], (int)(pmatch[1].rm_eo - pmatch[1].rm_so));
    regfree(&regex);

    return 0;
  }
}

void request_handler(int new_fd, char *base_path)
{
  char request[REQUEST_BUFFER_SIZE];

  if(read_request(new_fd, request) == 0) {
    perror("Can't read request\n");
  }

  printf("Request head:\n%s\n", request);

  char *ptr;
  ptr = strstr(request, " HTTP/");

  if(ptr == NULL) {
    error(new_fd, "Invalid http request\n");
  }

  *ptr = 0;
  ptr = NULL;

  if(strncmp(request, "GET ", 4) == 0) {
    ptr = request + 4;
  }

  if(ptr == NULL) {
    error(new_fd, "Requested resource is unsupported.\n");
  }

  printf("URI:\n%s\n", ptr);

  // Routing
  char *path[50];
  memset(path, '\0', sizeof(path));
  int pr;
  if((pr = get_path_from_request(ptr, path)) != 0) {
    switch(pr) {
      case 2:
        error_response(new_fd, 400, "Bad Request");
      case 1:
      default:
        error_response(new_fd, 500, "Internal Server Error");
    }
  }
  printf("path is %s with length %lu\n", path, strlen(path));

  char *resource[500];
  strcpy(resource, base_path);
  strcat(resource, path);
  int fd_res;
  fd_res = open(resource, O_RDONLY, 0);
  printf("Opening \"%s\"\n",resource);
  if(fd_res == -1) {
    printf("404 File not found Error\n");
    error_response(new_fd, 404, "Not Found");
  } else {
    printf("File found - processing...\n");

    int length;
    struct stat st;
    stat(resource, &st);
    length = st.st_size;
    printf("File length is %d\n", length);

    if(length == -1 ) {
      printf("Error getting size of file \n");
    }
    if((ptr = (char *)malloc(length)) == NULL) {
      printf("Error allocating memory during file reading\n");
    }
    read(fd_res, ptr, length);

    response(new_fd, "HTTP/1.1 200 OK\n");
    char *hd_cl[50];
    sprintf(hd_cl, "Content-length: %d\n", length);
    response(new_fd, hd_cl);
    response(new_fd, "Content-Type: text/html\n\n");
    if(send(new_fd, ptr, length, 0) == -1) {
      printf("There are an error during sending GOD DAMN FILE\n");
    }
    free(ptr);
    close(fd_res);
  }

  close(new_fd);
  exit(0);
}

int main(void)
{
  char *base_path;
  if((base_path = getenv("BASE_PATH")) == NULL) {
    printf("Missing `BASE_PATH` env.\n");
    exit(0);
  }

  int sockfd, new_fd; // listen on sock_fd, new connection on new_fd
  struct addrinfo hints, *serverinfo, *p;
  struct sockaddr_storage their_addr; // connector's address information
  socklen_t sin_size;
  struct sigaction sa;
  int yes=1;
  char s[INET6_ADDRSTRLEN];
  int rv;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if((rv = getaddrinfo(NULL, PORT, &hints, &serverinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return 1;
  }

  for(p = serverinfo; p != NULL; p = p->ai_next) {
    if((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      perror("server: socket");
      continue;
    }

    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      exit(1);
    }

    if(bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(serverinfo); // all done with this structure

  if(p == NULL) {
    fprintf(stderr, "server: failed to bind\n");
    exit(1);
  }

  if(listen(sockfd, CONNECTION_POOL) == -1) {
    perror("listen");
    exit(1);
  }

  sa.sa_handler = sigchld_handler; // reap all dead processes
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if(sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }

  printf("server: waiting for connections...\n");

  while(1) { // main accept loop
    sin_size = sizeof their_addr;
    new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
    if(new_fd == -1) {
      perror("accept");
      continue;
    }

    inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), s, sizeof s);
    printf("server: got connection from %s\n", s);

    if(!fork()) {
      close(sockfd); // child doesn't need the listener

      request_handler(new_fd, base_path);
    }

    close(new_fd);
  }

  return 0;
}
