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

#define PORT "3001"
#define CONNECTION_POOL 10 // how many pending connections queue will hold
#define REQUEST_BUFFER_SIZE 500
#define BASE_PATH "/Users/tte/labs/c/beej"

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

void request_handler(int new_fd)
{
  char request[REQUEST_BUFFER_SIZE];

  if(read_request(new_fd, request) == 0) {
    perror("Can't read request\n");
  }

  char *ptr;
  ptr = strstr(request, "HTTP/");

  if(ptr == NULL) {
    error(new_fd, "Invalid http request\n");
  }

  *ptr = 0;
  ptr = NULL;

  if(strncmp(request, "GET ", 4) == 0) {
    ptr = request + 4;
  }

  if(ptr == NULL) {
    error(new_fd, "Request method is unsupported. Please, use `GET` only.\n");
  }

  printf("request:\n%s\n", request);

  char *body, hd_content_ln[50]; 
  body = "<html><body><H1>Here we are</H1></body></html>";
  sprintf(hd_content_ln, "Content-length: %d\n", (int)strlen(body));

  response(new_fd, "HTTP/1.1 200 OK\n");
  response(new_fd, hd_content_ln);
  response(new_fd, "Content-Type: text/html\n\n");
  response(new_fd, body);

  close(new_fd);
  exit(0);
}

int main(void)
{
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

      request_handler(new_fd);
    }

    close(new_fd);
  }

  return 0;
}