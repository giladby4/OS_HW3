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
    struct timeval arrival;
    struct sockaddr_in clientaddr; 
} job_t;

typedef enum{BLOCK,DT,DH,BF,RANDOM} Sched;

typedef struct {
    int *ids;
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
    int vip_active;              // Flag indicating if VIP worker is active
    int active_jobs;               //count the regular workers working right now
    job_t *real_queue;            // Array of REAL jobs (requests) waiting to be processed
    pthread_mutex_t queue_lock;  // Mutex for protecting the job queue
    pthread_cond_t get_queue_not_empty;  // Condition variable to signal when the queue has jobs
    pthread_cond_t queue_not_full;   // Condition variable to signal when the queue is not full
    pthread_cond_t real_queue_not_empty;  // Condition variable to signal when the queue has jobs
    pthread_cond_t vip_done;     // Signal for VIP completion
    pthread_cond_t job_done;     // Signal for VIP completion


} thread_pool;


void printQueueState(thread_pool *pool) {
    int i = pool->get_queue_front;
    printf("Front: %d, Rear: %d, Size: %d\n", pool->get_queue_front, pool->get_queue_rear, pool->get_queue_size);
    printf("Queue contents: ");
    for (int j = 0; j < pool->get_queue_size; j++) {
        printf("%d ", pool->get_queue[i].connfd);  // Print job connfd or another identifier
        i = (i + 1) % pool->max_queue_size;
    }
    printf("\n");
    fflush(stdout);  // Ensure it gets printed immediately

}
 
void handle(job_t job, int *is_skip,int *is_static,int id,int count_all,int count_static){

        // Prepare the arrival and dispatch time structures
        struct timeval curr,dispatch;
        gettimeofday(&curr,NULL);
        dispatch.tv_sec = curr.tv_sec-job.arrival.tv_sec;
         dispatch.tv_usec = curr.tv_usec-job.arrival.tv_usec;;  // Example values

        // Create a threads_stats object
        threads_stats t_stats = malloc(sizeof(threads_stats));  // Allocate memory for the stats
        t_stats->id = id;  // Example: Initialize with default values
        t_stats->stat_req = count_static;
        t_stats->dynm_req = count_all-count_static;
        t_stats->total_req = count_all;
        requestHandle(job.connfd, job.arrival, dispatch, t_stats,is_skip,is_static);


        free(t_stats);
        close(job.connfd);

}


void *worker_thread(void *arg) {
    int id=-1,count_all=0,count_static=0;
    thread_pool *pool = (thread_pool *)arg;  // Get the thread pool from the argument
    job_t job;  // Variable to hold the job
    
    while (1) {
        // Lock the queue before accessing it
        pthread_mutex_lock(&pool->queue_lock);
        // Wait until there is at least one job in the queue (queue is not empty)
        while (pool->get_queue_size == 0 || pool->vip_active) {
            if (pool->vip_active) {
                pthread_cond_wait(&pool->vip_done, &pool->queue_lock);
            } else {
                pthread_cond_wait(&pool->get_queue_not_empty, &pool->queue_lock);
            }
        }
       
        // Remove the job from the queue (circular queue mechanism)
        job = pool->get_queue[pool->get_queue_front];
        pool->get_queue_front = (pool->get_queue_front + 1) % pool->max_queue_size;  // Move the front pointer
        pool->get_queue_size--;  // Decrease the queue size
        pool->active_jobs++;  // Increment active jobs count

        // Signal that the queue is not full (so more jobs can be added)
        pthread_cond_signal(&pool->queue_not_full);
        if(id==-1){
            for(int i=0; i<pool->num_threads;i++){
                if (pool->ids[i]==0){
                    id=i;
                    pool->ids[i]=1;
                    break;
                }
            }
        }
        // Unlock the queue after accessing it
        pthread_mutex_unlock(&pool->queue_lock);

         
        // Process the job (e.g., handle the connection)
        int is_skip,is_static;
        handle(job,&is_skip,&is_static,id,count_all,count_static);
        count_all++;
        count_static+=is_static;

        if (is_skip){
            pthread_mutex_lock(&pool->queue_lock);

            if (pool->get_queue_size > 0) {
                // Get the rear job
                job_t rear_job = pool->get_queue[(pool->get_queue_rear-1+ pool->max_queue_size) % pool->max_queue_size];
                pool->get_queue_rear = (pool->get_queue_rear - 1 + pool->max_queue_size) % pool->max_queue_size;
                pool->get_queue_size--;
                pool->active_jobs++;  // Decrement active jobs count
                pthread_cond_signal(&pool->queue_not_full);

                pthread_mutex_unlock(&pool->queue_lock);

                handle(rear_job,&is_skip,&is_static,id,count_all,count_static);
                count_all++;
                count_static+=is_static;
                pthread_mutex_lock(&pool->queue_lock);
                pool->active_jobs--;  // Decrement active jobs count

            }

            pthread_mutex_unlock(&pool->queue_lock);
        }
        pthread_mutex_lock(&pool->queue_lock);
        pool->active_jobs--;  // Decrement active jobs count
        pthread_cond_signal(&pool->job_done);

        pthread_mutex_unlock(&pool->queue_lock);


    }
    

    return NULL;
}

void *vip_worker_thread(void *arg) {

    thread_pool *pool = (thread_pool *)arg;
    job_t job;
    int id,count_all=0,count_static=0;
    while (1) {
        // Lock the VIP queue before accessing it
        pthread_mutex_lock(&pool->queue_lock);
        id=pool->num_threads;
        // Wait until there is a job in the VIP queue
        while (pool->real_queue_size == 0) {
            pthread_cond_wait(&pool->real_queue_not_empty, &pool->queue_lock);
        }
        pool->vip_active = 1;

        while (pool->real_queue_size > 0) {

            // Get the job from the VIP queue (circular queue)
            job = pool->real_queue[pool->real_queue_front];
            pool->real_queue_front = (pool->real_queue_front + 1) % pool->max_queue_size;
            pool->real_queue_size--;

            // Signal that the VIP queue is not full
            pthread_cond_signal(&pool->queue_not_full);

            pthread_mutex_unlock(&pool->queue_lock);
            int result, is_static;
            handle(job,&result,&is_static,id,count_all,count_static);
            count_all++;
            count_static+=is_static;
            pthread_mutex_lock(&pool->queue_lock);

        }
        pool->vip_active = 0;
        pthread_cond_signal(&pool->vip_done);

        pthread_mutex_unlock(&pool->queue_lock);


    }

    return NULL;
}



void add_job_to_queue(thread_pool *pool, job_t job, int is_vip, Sched sched) {

    pthread_mutex_lock(&pool->queue_lock);
    if(is_vip){
        // Wait if the queue is full
        while (pool->real_queue_size+pool->get_queue_size == pool->max_queue_size) {
            pthread_cond_wait(&pool->queue_not_full, &pool->queue_lock);
        }
    
        // Add the job to the queue
        pool->real_queue[pool->real_queue_rear] = job;
        pool->real_queue_rear = (pool->real_queue_rear + 1) % pool->max_queue_size;
        pool->real_queue_size++;

        pthread_cond_signal(&pool->real_queue_not_empty);  // Signal worker threads to start processing
        pthread_mutex_unlock(&pool->queue_lock);
    }
    else{
        switch(sched){
            case BLOCK:
                while (pool->get_queue_size+pool->real_queue_size == pool->max_queue_size) {
                    pthread_cond_wait(&pool->queue_not_full, &pool->queue_lock);
                }
                break;
            case DT:
                if (pool->get_queue_size + pool->real_queue_size == pool->max_queue_size) {
                    close(job.connfd);
                    pthread_mutex_unlock(&pool->queue_lock);
                    return;  // Drop the job
                }
                break;                
            case DH:
                if (pool->get_queue_size + pool->real_queue_size == pool->max_queue_size) {
                    close(pool->get_queue[pool->get_queue_front].connfd);
                    pool->get_queue_front = (pool->get_queue_front + 1) % pool->max_queue_size;  // Move front pointer
                    pool->get_queue_size--;  // Decrease size
                }
                break;                

            case BF:
                if (pool->get_queue_size + pool->real_queue_size == pool->max_queue_size) {

                    while (pool->get_queue_size > 0 || pool->real_queue_size > 0) {
                        pthread_cond_wait(&pool->queue_not_full, &pool->queue_lock);
                        while(pool->vip_active)
                            pthread_cond_wait(&pool->vip_done, &pool->queue_lock);
                        while(pool->active_jobs)
                            pthread_cond_wait(&pool->job_done, &pool->queue_lock);
                    }
                    close(job.connfd);  // Drop the new request
                    pthread_mutex_unlock(&pool->queue_lock);
                    return;  // No new request is added
                }
                break;

            case RANDOM:
                if (pool->get_queue_size + pool->real_queue_size == pool->max_queue_size) {
                    int drop_count = (pool->get_queue_size+1 ) / 2;  // Calculate how many jobs to drop
                    for (int i = 0; i < drop_count; i++) {
                        int drop_index = (pool->get_queue_front + rand() % pool->get_queue_size) % pool->max_queue_size;
                        // Shift elements to overwrite the dropped job
                        close( pool->get_queue[drop_index].connfd);

                        for (int j = drop_index; j != pool->get_queue_rear; j = (j + 1) % pool->max_queue_size) {
                            pool->get_queue[j] = pool->get_queue[(j + 1) % pool->max_queue_size];
                        }
                        pool->get_queue_rear = (pool->get_queue_rear - 1 + pool->max_queue_size) % pool->max_queue_size;
                        pool->get_queue_size--;  // Decrease queue size
                    }
                }                
                break;
            default:
                pthread_mutex_unlock(&pool->queue_lock);
                return ;
        }




        pool->get_queue[pool->get_queue_rear] = job;
 
        pool->get_queue_rear = (pool->get_queue_rear + 1) % pool->max_queue_size;
        pool->get_queue_size++;


        pthread_cond_signal(&pool->get_queue_not_empty);  // Signal worker threads to start processing
        pthread_mutex_unlock(&pool->queue_lock);
       

    }


}

Sched string_to_sched(const char* sched_str) {

    // Compare the uppercased string to the valid enum strings
    if (strcasecmp(sched_str, "BLOCK") == 0) {
        return BLOCK;
    } else if (strcasecmp(sched_str, "DT") == 0) {
        return DT;
    } else if (strcasecmp(sched_str, "DH") == 0) {
        return DH;
    } else if (strcasecmp(sched_str, "BF") == 0) {
        return BF;
    } else if (strcasecmp(sched_str, "RANDOM") == 0) {
        return RANDOM;
    } else {
        return -1;  // Return an invalid enum value
    }
}


// HW3: Parse the new arguments too
void getargs(int *port, int *threads, int *queue_size, Sched *sched, int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <port> <threads> <queue_size> <schedalg>\n", argv[0]);
        exit(1);
    }
    *port = atoi(argv[1]);
    
    *threads = atoi(argv[2]);
    if(*threads<1){
        app_error("threads number must be positive");
        exit(1);
    }
    *queue_size = atoi(argv[3]); 
    if(*queue_size<1){
        app_error("queue size must be positive");
        exit(1);
    }
    *sched=string_to_sched(argv[4]);
    if(*sched==-1){
        app_error("couldn't find schedalg name");
        exit(1);
    }
    


}

int main(int argc, char *argv[])
{

    int listenfd, connfd, port, threads, clientlen, queue_size;
    struct sockaddr_in clientaddr;
    Sched sched;

    getargs(&port, &threads, &queue_size, &sched, argc, argv);
    if (sched==-1){
        
    }
    thread_pool *pool= malloc(sizeof(thread_pool));
    pool->ids=malloc(sizeof(int) *threads);
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
    pool->vip_active = 0;
    pool->active_jobs=0;


    pthread_mutex_init(&pool->queue_lock, NULL);
    pthread_cond_init(&pool->get_queue_not_empty, NULL);
    pthread_cond_init(&pool->queue_not_full, NULL);
    pthread_cond_init(&pool->real_queue_not_empty, NULL);
    pthread_cond_init(&pool->job_done, NULL);
    pthread_cond_init(&pool->vip_done, NULL);

    for (int i = 0; i < pool->num_threads; i++) {
        pool->ids[i]=0;
    }
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
        struct timeval arrival;
        gettimeofday(&arrival,NULL);
        job_t job = {connfd,arrival, clientaddr};  
        int is_vip=getRequestMetaData(connfd);
        add_job_to_queue(pool, job,is_vip,sched);
        }
    free(pool->threads);
    pthread_mutex_destroy(&pool->queue_lock);
    pthread_cond_destroy(&pool->get_queue_not_empty);
    pthread_cond_destroy(&pool->queue_not_full);
    pthread_cond_destroy(&pool->real_queue_not_empty);
    pthread_cond_destroy(&pool->job_done);
    pthread_cond_destroy(&pool->vip_done);



}



    


 
