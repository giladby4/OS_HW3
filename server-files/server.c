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
typedef struct {
    int connfd;
    struct sockaddr_in clientaddr; 
} job_t;



typedef struct {
    pthread_t *threads;          // Array of worker threads
    pthread_t vip_thread;
    int num_threads;             // Number of threads in the pool
    int max_queue_size;          // Maximum size of the job queue
    int get_queue_size;              // Current size of the GET job queue
    int get_queue_front;             // Front index of the job queue
    int get_queue_rear;              // Rear index of the job queue
    job_t *get_queue;            // Array of GET jobs (requests) waiting to be processed
    int real_queue_size;              // Current size of the REAL job queue
    int real_queue_front;             // Front index of the job queue
    int real_queue_rear;              // Rear index of the job queue
    job_t *real_queue;            // Array of REAL jobs (requests) waiting to be processed
    pthread_mutex_t queue_lock;  // Mutex for protecting the job queue
    pthread_cond_t get_queue_not_empty;  // Condition variable to signal when the queue has jobs
    pthread_cond_t get_queue_not_full;   // Condition variable to signal when the queue is not full
    pthread_cond_t real_queue_not_empty;  // Condition variable to signal when the queue has jobs
    pthread_cond_t real_queue_not_full;   // Condition variable to signal when the queue is not full

} thread_pool;


void *worker_thread(void *arg) {
    thread_pool *pool = (thread_pool *)arg;  // Get the thread pool from the argument
    job_t job;  // Variable to hold the job

    while (1) {
        // Lock the queue before accessing it
        pthread_mutex_lock(&pool->queue_lock);

        // Wait until there is at least one job in the queue (queue is not empty)
        while (pool->get_queue_size == 0) {
            pthread_cond_wait(&pool->get_queue_not_empty, &pool->queue_lock);
        }

        // Remove the job from the queue (circular queue mechanism)
        job = pool->get_queue[pool->get_queue_front];
        pool->get_queue_front = (pool->get_queue_front + 1) % pool->max_queue_size;  // Move the front pointer
        pool->get_queue_size--;  // Decrease the queue size

        // Signal that the queue is not full (so more jobs can be added)
        pthread_cond_signal(&pool->get_queue_not_full);

        // Unlock the queue after accessing it
        pthread_mutex_unlock(&pool->queue_lock);

        // Prepare the arrival and dispatch time structures
        struct timeval arrival, dispatch;
        arrival.tv_sec = 0; arrival.tv_usec = 0;  // Example values
        dispatch.tv_sec = 0; dispatch.tv_usec = 0;  // Example values

        // Create a threads_stats object
        threads_stats t_stats = malloc(sizeof(threads_stats));  // Allocate memory for the stats
        t_stats->id = 0;  // Example: Initialize with default values
        t_stats->stat_req = 0;
        t_stats->dynm_req = 0;
        t_stats->total_req = 0;

        // Process the job (e.g., handle the connection)
        requestHandle(job.connfd, arrival, dispatch, t_stats);

        // Free the memory allocated for the stats object
        free(t_stats);

        // Optionally, you can close the connection if no further communication is required
        close(job.connfd);
    }

    return NULL;
}

void *vip_worker_thread(void *arg) {
    thread_pool *pool = (thread_pool *)arg;
    job_t job;

    while (1) {
        // Lock the VIP queue before accessing it
        pthread_mutex_lock(&pool->queue_lock);

        // Wait until there is a job in the VIP queue
        while (pool->real_queue_size == 0) {
            pthread_cond_wait(&pool->real_queue_not_empty, &pool->queue_lock);
        }

        // Get the job from the VIP queue (circular queue)
        job = pool->real_queue[pool->real_queue_front];
        pool->real_queue_front = (pool->real_queue_front + 1) % pool->max_queue_size;
        pool->real_queue_size--;

        // Signal that the VIP queue is not full
        pthread_cond_signal(&pool->real_queue_not_full);

        pthread_mutex_unlock(&pool->queue_lock);

        // Process the VIP request
        struct timeval arrival, dispatch;
        arrival.tv_sec = 0; arrival.tv_usec = 0;
        dispatch.tv_sec = 0; dispatch.tv_usec = 0;

        // Example: VIP request handling (change according to actual request handling)
        threads_stats t_stats = malloc(sizeof(threads_stats));
        t_stats->id = 0;
        t_stats->stat_req = 0;
        t_stats->dynm_req = 0;
        t_stats->total_req = 0;
        requestHandle(job.connfd, arrival, dispatch, t_stats);
        free(t_stats);

        close(job.connfd);
    }

    return NULL;
}



void add_job_to_queue(thread_pool *pool, job_t job, int is_vip) {

    pthread_mutex_lock(&pool->queue_lock);
    if(is_vip){
        // Wait if the queue is full
        while (pool->real_queue_size == pool->max_queue_size) {
            pthread_cond_wait(&pool->real_queue_not_full, &pool->queue_lock);
        }
    
        // Add the job to the queue
        pool->real_queue[pool->real_queue_rear] = job;
        pool->real_queue_rear = (pool->real_queue_rear + 1) % pool->max_queue_size;
        pool->real_queue_size++;

        pthread_cond_signal(&pool->real_queue_not_empty);  // Signal worker threads to start processing
        pthread_mutex_unlock(&pool->queue_lock);
    }
    else{

        while (pool->get_queue_size == pool->max_queue_size) {
            pthread_cond_wait(&pool->get_queue_not_full, &pool->queue_lock);
        }


        pool->get_queue[pool->get_queue_rear] = job;
 
        pool->get_queue_rear = (pool->get_queue_rear + 1) % pool->max_queue_size;
        pool->get_queue_size++;


        pthread_cond_signal(&pool->get_queue_not_empty);  // Signal worker threads to start processing
        pthread_mutex_unlock(&pool->queue_lock);
       

    }


}


// HW3: Parse the new arguments too
void getargs(int *port, int *threads, int *queue_size, int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <port> <threads> <queue_size>\n", argv[0]);
        exit(1);
    }
    *port = atoi(argv[1]);
    *threads = atoi(argv[2]);
    *queue_size = atoi(argv[3]); 
}

int main(int argc, char *argv[])
{

    int listenfd, connfd, port, threads, clientlen, queue_size;
    struct sockaddr_in clientaddr;

    getargs(&port, &threads, &queue_size, argc, argv);

    thread_pool *pool= malloc(sizeof(thread_pool));
    pool->threads = malloc(sizeof(pthread_t) * threads);
    pool->get_queue=malloc(sizeof(job_t)*queue_size);
    pool->real_queue=malloc(sizeof(job_t)*queue_size);

    pool->num_threads = threads;
    pool->max_queue_size = queue_size;
    pool->get_queue_size = 0; 
    pool->get_queue_front = 0;
    pool->get_queue_rear = 0;
    pool->real_queue_size = 0; 
    pool->real_queue_front = 0;
    pool->real_queue_rear = 0;

    pthread_mutex_init(&pool->queue_lock, NULL);
    pthread_cond_init(&pool->get_queue_not_empty, NULL);
    pthread_cond_init(&pool->get_queue_not_full, NULL);
    pthread_cond_init(&pool->real_queue_not_empty, NULL);
    pthread_cond_init(&pool->real_queue_not_full, NULL);

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, (void *)pool);
    }
    pthread_create(&pool->vip_thread, NULL, vip_worker_thread, (void *)pool);

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
        job_t job = {connfd, clientaddr};  // Example job structure
        int is_vip=getRequestMetaData(connfd);
        add_job_to_queue(pool, job,is_vip);
        }
    free(pool->threads);
    pthread_mutex_destroy(&pool->queue_lock);
    pthread_cond_destroy(&pool->get_queue_not_empty);
    pthread_cond_destroy(&pool->get_queue_not_full);
    pthread_cond_destroy(&pool->real_queue_not_empty);
    pthread_cond_destroy(&pool->real_queue_not_full);


}



    


 
