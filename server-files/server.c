#include "segel.h"
#include "request.h"

// 
// server.c: A very, very simple web server
//
// To run:
//  ./server <portnum (above 2000)>
//
// Repeatedly handles HTTP requests sent to this port number.
// Most of the work is done within routines written in request.c
//

// HW3: Parse the new arguments too
void getargs(int *port, int argc, char *argv[])
{
    if (argc < 2) {
	fprintf(stderr, "Usage: %s <port>\n", argv[0]);
	exit(1);
    }
    *port = atoi(argv[1]);
}

int main(int argc, char *argv[])
{

    int listenfd, connfd, port, clientlen;
    struct sockaddr_in clientaddr;

    getargs(&port, argc, argv);

    // 
    // HW3: Create some threads...
    //

    listenfd = Open_listenfd(port);
    while (1) {
	clientlen = sizeof(clientaddr);
	connfd = Accept(listenfd, (SA *)&clientaddr, (socklen_t *) &clientlen);
	// 
	// HW3: In general, don't handle the request in the main thread.
	// Save the relevant info in a buffer and have one of the worker threads 
	// do the work. 
	// 
    // only for demostration purpuse:
    threads_stats t = malloc(sizeof(threads_stats));
    t->id = 0;
	t->stat_req = 0;
	t->dynm_req = 0;
	t->total_req =0;
    struct timeval arrival, dispatch;
    dispatch.tv_sec = 0; dispatch.tv_usec = 0;
    arrival.tv_sec = 0; arrival.tv_usec =0;
    // only for demostration purpuse
    requestHandle(connfd,arrival, dispatch, t);
    free(t);
	Close(connfd);
    }
}



    


 
