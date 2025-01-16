#ifndef __REQUEST_H__
#define __REQUEST_H__


typedef struct Threads_stats{
	int id;
	int stat_req;
	int dynm_req;
	int total_req;
} * threads_stats;

// handle a request
int requestHandle(int fd, struct timeval arrival, struct timeval dispatch, threads_stats t_stats);

//  Returns True/False if realtime event
int getRequestMetaData(int fd);

#endif
