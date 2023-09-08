#include "csapp.h"
#define SA struct sockaddr

/* recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* predetermined client response headers */
static const char *client_res_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\nConnection: close\r\nProxy-Connection: close\r\n\r\n";

/* client response for bad requests */
static const char *bad_request = "HTTP/1.0 400 Bad Request\r\nContent-Type: plain/text\r\nContent-Length: 0\r\n\r\n";

/*
 * cache item structure (linked list)
 *
 * length: (head) length of list / (other) length of data
 * host: (head) NULL / (other) ptr to host of data
 * port: (head) NULL / (other) ptr to port of data
 * uri: (head) NULL / (other) ptr to uri of data
 * data: (head) NULL / (other) ptr to data
 *
 * cachehead: head of cache list
 * cachesize: total size of all cache data
 */
typedef struct cacheitem {
    int length;
    char *host;
    char *port;
    char *uri;
    char *data;
    struct cacheitem *next;
} cacheitem;

static cacheitem *cachehead;
static int cachesize = 0;

/*
 * helper functions
 *
 * proxy: thread routine, work with each client in each thread
 * check_request_line: parse request line and check validity
 * parse_url: parse URL to get host, port, and URI
 * get_cached_item: return cached data if same request exists in cache list
 *                  for LRU eviction policy, move recently used item at the first of cache list
 * delete_last_cache: delete last item in cache list
 */
void *proxy(void *vargp);
int check_request_line(char *reqline, char **method, char **uri, char **version);
void parse_url(char *url, char **host, char **port, char **uri);
cacheitem *get_cached_item(char *host, char *port, char *uri);
void delete_last_cache();

/*
 * main - concurrent proxy server
 */
int main(int argc, char *argv[]) {
    int listenfd, *connfdp;
    struct sockaddr_in clientaddr;
    socklen_t clientlen;
    pthread_t tid;

    // get listening descriptor
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    listenfd = open_listenfd(argv[1]);

    // init cache list
    cachehead = malloc(sizeof(cacheitem));
    cachehead->length = 0;
    cachehead->host = NULL;
    cachehead->port = NULL;
    cachehead->uri = NULL;
    cachehead->data = NULL;
    cachehead->next = NULL;

    // accept connection from client
    clientlen = sizeof(struct sockaddr_in);
    while (1) {
        connfdp = malloc(sizeof(int));
        if ((*connfdp = accept(listenfd, (SA *)&clientaddr, &clientlen)) < 0) {
            fprintf(stderr, "client connection failed\n");
            free(connfdp);
            continue;
        }
        // create new thread for each new connection
        pthread_create(&tid, NULL, proxy, connfdp);
    }

    // close listening descriptor & free cache
    close(listenfd);
    cacheitem *curr = cachehead->next;
    cacheitem *next;
    while (curr != NULL) {
        next = curr->next;
        free(curr->host);
        free(curr->port);
        free(curr->uri);
        free(curr->data);
        free(curr);
        curr = next;
    }
    free(cachehead);

    return 0;
}

/*
 * proxy - thread routine, work with each client in each thread
 */
void *proxy(void *vargp) {
    int connfd, clientfd, n, len, valid = 1;
    char buf[MAXLINE], *method, *version, *url, *host, *port, *uri, *cachebuf;
    rio_t rio;
    cacheitem *item;

    // detach itself for reaping & save connfd
    pthread_detach(pthread_self());
    connfd = *(int *)vargp;
    free(vargp);

    // get HTTP request line from client
    rio_readinitb(&rio, connfd);
    if (rio_readlineb(&rio, buf, MAXLINE) == 0) {
        fprintf(stderr, "empty request\n");
        rio_writen(connfd, (void *)bad_request, strlen(bad_request));
        close(connfd);
        return NULL;
    }
    if (check_request_line(buf, &method, &url, &version) < 0) {
        fprintf(stderr, "invalid HTTP request line\n");
        rio_writen(connfd, (void *)bad_request, strlen(bad_request));
        close(connfd);
        return NULL;
    }
    // parse URL to get host, port, and URI
    parse_url(url, &host, &port, &uri);

    // if same request info is in cache list, send data directly to client and close connection
    // same request: host, port, and uri are all same
    if ((item = get_cached_item(host, port, uri)) != NULL) {
        rio_writen(connfd, item->data, item->length);
        close(connfd);
        return NULL;
    }

    // connect to server and forward request line from client
    if ((clientfd = open_clientfd(host, port)) < 0) {
        fprintf(stderr, "server connection failed\n");
        rio_writen(connfd, (void *)bad_request, strlen(bad_request));
        free(host);
        free(port);
        free(uri);
        return NULL;
    }
    // put URI instead of URL as 2nd argument
    sprintf(buf, "%s %s %s\r\n", method, uri, version);
    rio_writen(clientfd, buf, strlen(buf));

    // forward request headers from client to server
    while (rio_readlineb(&rio, buf, MAXLINE) != 0) {
        if (!strcmp(buf, "\r\n")) { break; }    // end of HTTP header

        // ignore 3 headers from client (User-Agent, Connection, Proxy-Connection)
        // replace them to predetermined values
        if (strncmp(buf, "User-Agent", 10) && strncmp(buf, "Connection", 10) && strncmp(buf, "Proxy-Connection", 16)) {
            rio_writen(clientfd, buf, strlen(buf));
        }
    }
    rio_writen(clientfd, (void *)client_res_hdr, strlen(client_res_hdr));

    // init cache buffer for this connection
    cachebuf = malloc(MAX_OBJECT_SIZE);
    len = 0;

    // forward response from server to client
    rio_readinitb(&rio, clientfd);
    while ((n = rio_readnb(&rio, buf, MAXLINE)) != 0) {
        if (valid && (len + n < MAX_OBJECT_SIZE)) {  // valid (size not exceeded)
            memcpy(cachebuf + len, buf, n);
            len += n;
        } else if (valid) {     // size too big -> don't save in cache list
            valid = 0;
        }
        rio_writen(connfd, buf, n);
    }

    // if valid, insert data at the first of cache list
    if (valid) {
        // if cache is full, delete last item
        while (cachesize + len > MAX_CACHE_SIZE) {
            delete_last_cache();
        }

        cacheitem *ci = malloc(sizeof(cacheitem));
        ci->data = malloc(len);
        memcpy(ci->data, cachebuf, len);
        ci->length = len;
        ci->host = host;
        ci->port = port;
        ci->uri = uri;

        ci->next = cachehead->next;
        cachehead->next = ci;
        (cachehead->length)++;
        cachesize += len;

    } else {
        free(host);
        free(port);
        free(uri);
    }

    // close listening descriptor & free cache buffer
    close(connfd);
    free(cachebuf);
    return NULL;
}

/*
 * check_request_line - parse request line and check validity
 * return 0 if valid, -1 if invalid
 */
int check_request_line(char *reqline, char **method, char **url, char **version) {
    if (strchr(reqline, ' ') == NULL) {     // only 1 arg
        return -1;
    }
    *method = strtok(reqline, " ");
    *url = strtok(NULL, " ");
    if ((*version = strtok(NULL, "\r\n")) == NULL) {    // only 2 args
        return -1;
    }
    if (strcmp(*version, "HTTP/1.1") && strcmp(*version, "HTTP/1.0")) { // unsupported HTTP version
        return -1;
    }
    return 0;
}

/*
 * parse_url - parse URL to get host, port, and URI
 */
void parse_url(char *url, char **host, char **port, char **uri) {
    // url = http://<host>:<port><uri>
    char *hp, *pu;

    if (!strncmp(url, "http://", 7)) {
        url += 7;
    }
    hp = strchr(url, ':');
    pu = strchr(url, '/');

    if (hp == NULL) {
        strtok(url, "/");
        *host = malloc((strlen(url) + 1));
        strcpy(*host, url);
        *port = malloc(4);
        strcpy(*port, "80");
    } else {
        strtok(url, ":");
        *host = malloc((strlen(url) + 1));
        strcpy(*host, url);
        url = strtok(NULL, "/");
        *port = malloc(sizeof(url));
        strcpy(*port, url);
    }
    if (pu == NULL || (url = strtok(NULL, "")) == NULL) {
        *uri = malloc(2);
        strcpy(*uri, "/");
    } else {
        *uri = malloc((strlen(url) + 2));
        strcpy(*uri, "/");
        strcat(*uri, url);
    }
}

/*
 * get_cached_item - return cached data if same request exists in cache list
 *                   for LRU eviction policy, move recently used item at the first of cache list
 */
cacheitem *get_cached_item(char *host, char *port, char *uri) {
    cacheitem *prev, *curr;

    prev = cachehead;
    curr = cachehead->next;
    while (curr != NULL) {
        if (!strcmp(host, curr->host) && !strcmp(port, curr->port) && !strcmp(uri, curr->uri)) {
            prev->next = curr->next;
            curr->next = cachehead->next;
            cachehead->next = curr;
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }
    return NULL;
}

/*
 * delete_last_cache - delete last item in cache list
 */
void delete_last_cache() {
    cacheitem *prev, *last;

    prev = cachehead;
    last = cachehead->next;
    while (last->next != NULL) {
        prev = last;
        last = last->next;
    }

    prev->next = NULL;
    cachesize -= last->length;

    free(last->host);
    free(last->port);
    free(last->uri);
    free(last->data);
    free(last);
    (cachehead->length)--;
}
