#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

// -------------------- Global Variables (replacing original macros) --------------------
// Instead of using fixed macros for capacity and max customers, we use global variables
// which will be assigned values from user input in main().
static int g_escalator_capacity  = 13; // Must not exceed 13, will be parsed from user input
static int g_mall_capacity       = 30; // Must not exceed 30, will be parsed from user input

#define UP    1
#define DOWN -1
#define IDLE  0

// -------------------- Global Mutex + Semaphores --------------------
static pthread_mutex_t mall_mutex;
static sem_t escalator_capacity_sem; 

// -------------------- Data Structures --------------------
typedef struct Customer {
    int id;
    int arrival_time;  // Arrival time (seconds)
    int direction;     // UP or DOWN
    int position;      // 0 or 14 (kept as in original code)
    struct Customer* next;
    struct Customer* prev;
} Customer;

typedef struct {
    Customer* head;
    Customer* tail;
    int length;
    int direction; // 1=UP, -1=DOWN
} Queue;

/*
 * In the Escalator structure, the 'steps' array was originally sized by MAX_ESCALATOR_CAPACITY.
 * We keep its physical size at 13, but we only use the first g_escalator_capacity elements logically.
 * (Because user input cannot exceed 13, this is safe.)
 */
typedef struct {
    Customer* steps[13];  
    int direction; // UP / DOWN / IDLE
    int num_people; 
} Escalator;

typedef struct {
    Queue* upQueue;
    Queue* downQueue;
    Escalator* escalator;
    int total_customers;
    int current_time;
} Mall;

// Customer thread argument structure
typedef struct {
    int direction;     // Direction
    int arrival_time;  // Arrival time
} CustomerThreadArgs;

// -------------------- Global Variables --------------------
static int total_turnaround_time = 0;
static int completed_customers   = 0;

// Tracks how many people have boarded in the current direction (used for forced direction switching)
static int current_dir_boarded_count = 0;

// Global auto-increment ID for customers
static int global_customer_id = 0;

Mall* mall = NULL;
static int simulation_running = 1;

// -------------------- Function Declarations --------------------
Queue* init_queue(int dir);
Escalator* init_escalator();
Mall* init_mall();
Customer* create_customer_struct(int direction, int arrival_time);

void enqueue(Queue* q, Customer* c);
Customer* dequeue(Queue* q);

int can_customer_board(Customer* c);
void board_customer(Customer* c);
void operate_escalator();
void print_escalator_status();

// The mall_control_loop no longer randomly generates customers.
// It only handles transporting already created customers.
void mall_control_loop();

void cleanup_resources();

// New: Customer thread function
void* customer_thread(void* arg);

// --------------------------------------------------
// Initialization
// --------------------------------------------------
Queue* init_queue(int dir) {
    pthread_mutex_lock(&mall_mutex);
    Queue* q = (Queue*)malloc(sizeof(Queue));
    if(!q){
        perror("malloc queue");
        exit(EXIT_FAILURE);
    }
    q->head = NULL;
    q->tail = NULL;
    q->length = 0;
    q->direction = dir; 
    pthread_mutex_unlock(&mall_mutex);
    return q;
}

Escalator* init_escalator(){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = (Escalator*)malloc(sizeof(Escalator));
    if(!e){
        perror("malloc escalator");
        exit(EXIT_FAILURE);
    }
    for(int i=0; i<13; i++){
        e->steps[i] = NULL;
    }
    e->direction = IDLE;
    e->num_people= 0;
    pthread_mutex_unlock(&mall_mutex);
    return e;
}

Mall* init_mall(){
    pthread_mutex_lock(&mall_mutex);
    Mall* m = (Mall*)malloc(sizeof(Mall));
    if(!m){
        perror("malloc mall");
        exit(EXIT_FAILURE);
    }
    m->upQueue   = init_queue(UP);
    m->downQueue = init_queue(DOWN);
    m->escalator = init_escalator();
    m->total_customers=0;
    m->current_time=0;
    pthread_mutex_unlock(&mall_mutex);
    return m;
}

// Create Customer Structure (non-thread, just the data)
Customer* create_customer_struct(int direction, int arrival_time) {
    pthread_mutex_lock(&mall_mutex);
    global_customer_id++;
    Customer* c = (Customer*)malloc(sizeof(Customer));
    c->id = global_customer_id;
    c->arrival_time = arrival_time;
    c->direction    = direction;
    // Original code uses 0 or 14 for position, not changed.
    c->position     = (direction==UP) ? 0 : 14;
    c->next = NULL;
    c->prev = NULL;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// Customer Thread Function
void* customer_thread(void* arg) {
    CustomerThreadArgs* args = (CustomerThreadArgs*)arg;
    
    pthread_mutex_lock(&mall_mutex);
    int direction = args->direction;
    int arrival_time = args->arrival_time;
    
    // Create the data structure
    Customer* c = create_customer_struct(direction, arrival_time);
    
    // Increase total number of customers in the mall
    mall->total_customers++;
    
    // Insert customer into the appropriate queue
    if (direction == UP) {
        enqueue(mall->upQueue, c);
    } else {
        enqueue(mall->downQueue, c);
    }
    
    pthread_mutex_unlock(&mall_mutex);
    
    // Free the argument memory
    free(args);
    
    // Thread exit
    return NULL;
}

// Create Customer (start a new thread)
void create_customer(int direction) {
    // Prepare thread arguments
    CustomerThreadArgs* args = (CustomerThreadArgs*)malloc(sizeof(CustomerThreadArgs));
    if (!args) {
        perror("malloc customer thread args");
        exit(EXIT_FAILURE);
    }
    
    args->direction = direction;
    
    pthread_mutex_lock(&mall_mutex);
    args->arrival_time = mall->current_time;
    pthread_mutex_unlock(&mall_mutex);
    
    // Create thread
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, customer_thread, args) != 0) {
        perror("pthread_create");
        free(args);
        exit(EXIT_FAILURE);
    }
    
    // Detach thread to let it exit independently
    pthread_detach(thread_id);
    
    printf("Customer thread created, direction: %s\n", (direction==UP)?"Up":"Down");
}

// --------------------------------------------------
// Queue Operations
// --------------------------------------------------
void enqueue(Queue* q, Customer* c){
    pthread_mutex_lock(&mall_mutex);
    if(!q->head){
        q->head = c;
        q->tail = c;
    } else {
        q->tail->next = c;
        c->prev       = q->tail;
        q->tail       = c;
    }
    q->length++;
    printf("Customer %d joined the queue, direction: %s, arrival time: %d\n",
           c->id, 
           (q->direction==UP)?"Up":"Down", 
           c->arrival_time);
    pthread_mutex_unlock(&mall_mutex);
}

Customer* dequeue(Queue* q){
    pthread_mutex_lock(&mall_mutex);
    if(!q->head){
        pthread_mutex_unlock(&mall_mutex);
        return NULL;
    }
    Customer* c = q->head;
    q->head     = c->next;
    if(!q->head){
        q->tail = NULL;
    } else {
        q->head->prev = NULL;
    }
    q->length--;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// --------------------------------------------------
// Check if a Customer Can Board the Escalator
// --------------------------------------------------
int can_customer_board(Customer* c){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    // If the escalator is full, they cannot board
    if(e->num_people >= g_escalator_capacity) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    
    // If the escalator is idle, customer can board and set direction
    if(e->direction == IDLE){
        e->direction = c->direction;
        current_dir_boarded_count = 0;
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // If escalator direction matches the customer's direction, allow boarding
    if(e->direction == c->direction){
        // If we already boarded >=5 people in this direction AND there are people waiting in the opposite queue => deny
        Queue* oppQ = (c->direction==UP)? mall->downQueue: mall->upQueue;
        if(oppQ->length>0 && current_dir_boarded_count>=5){
            pthread_mutex_unlock(&mall_mutex);
            return 0; 
        }
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // Opposite direction => cannot board
    pthread_mutex_unlock(&mall_mutex);
    return 0;
}

// --------------------------------------------------
// Customer Boards the Escalator
// --------------------------------------------------
void board_customer(Customer* c){
    sem_wait(&escalator_capacity_sem); // Acquire lock for escalator capacity

    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    
    // Determine entry index
    int entry = (c->direction==UP)? 0 : (g_escalator_capacity - 1);
    e->steps[entry] = c;
    e->num_people++;
    current_dir_boarded_count++;
    int wait_time = mall->current_time - c->arrival_time;
    printf("Customer %d boarded the escalator, direction: %s, wait time=%d sec, transported=%d people\n",
           c->id, 
           (c->direction==UP)?"Up":"Down",
           wait_time, current_dir_boarded_count);
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// Move Customers on the Escalator Every Second
// --------------------------------------------------
void operate_escalator(){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    if(e->num_people>0){
        printf("Escalator direction = %s, Passengers = %d\n",
               (e->direction==UP)?"Up":
               (e->direction==DOWN)?"Down":"Idle",
               e->num_people);

        // Moving up
        if(e->direction==UP){
            // Disembark at the top
            if(e->steps[g_escalator_capacity-1]){
                Customer* c = e->steps[g_escalator_capacity-1];
                int tat = mall->current_time - c->arrival_time;
                printf("Customer %d completed upward travel, Turnaround time = %d sec\n", c->id, tat);
                total_turnaround_time += tat;
                completed_customers++;
                free(c);
                e->steps[g_escalator_capacity-1] = NULL;
                e->num_people--;
                mall->total_customers--;
                sem_post(&escalator_capacity_sem);
            }
            // Shift everyone else up by 1
            for(int i=g_escalator_capacity-2; i>=0; i--){
                if(e->steps[i]){
                    e->steps[i+1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }
        // Moving down
        else if(e->direction==DOWN){
            // Disembark at the bottom
            if(e->steps[0]){
                Customer* c = e->steps[0];
                int tat = mall->current_time - c->arrival_time;
                printf("Customer %d completed downward travel, Turnaround time = %d sec\n", c->id, tat);
                total_turnaround_time += tat;
                completed_customers++;
                free(c);
                e->steps[0] = NULL;
                e->num_people--;
                mall->total_customers--;
                sem_post(&escalator_capacity_sem);
            }
            // Shift everyone else down by 1
            for(int i=1; i<g_escalator_capacity; i++){
                if(e->steps[i]){
                    e->steps[i-1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }

        // If escalator is now empty, decide whether to force a direction switch
        if(e->num_people==0){
            printf("Escalator is now empty. Passengers transported in this direction = %d\n", current_dir_boarded_count);

            // If we have transported >=5 people and there are people waiting in the opposite direction => switch direction
            Queue* oppQ = (e->direction==UP)? mall->downQueue: mall->upQueue;
            int oppLen  = oppQ->length;

            if(current_dir_boarded_count>=5 && oppLen>0){
                printf(">=5 people have crossed, and there are customers waiting in the opposite direction. Forcing direction switch to %s\n",
                       (e->direction==UP)?"Down":"Up");
                e->direction = - e->direction; 
            } else {
                e->direction = IDLE;
            }
            // Reset count
            current_dir_boarded_count=0;
        }
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// Print escalator status
// --------------------------------------------------
void print_escalator_status(){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    printf("Escalator status: [");
    // Only print g_escalator_capacity steps
    for(int i=0; i<g_escalator_capacity; i++){
        if(e->steps[i]) {
            printf("%d", e->steps[i]->id);
        } else {
            printf("0");
        }
        if(i<g_escalator_capacity-1) printf(",");
    }
    printf("], Direction: %s\n",
           (e->direction==UP)?"Up":
           (e->direction==DOWN)?"Down":"Idle");
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// Main loop (no random generation of new customers anymore)
// --------------------------------------------------
void mall_control_loop(){
    while(simulation_running){
        pthread_mutex_lock(&mall_mutex);
        printf("\n----- Time: %d sec -----\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. Operate escalator
        operate_escalator();

        // 2. Print escalator status
        print_escalator_status();

        // 3. Attempt to board the first customer in the up queue
        pthread_mutex_lock(&mall_mutex);
        if(mall->upQueue->head){
            Customer* c = mall->upQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)){
                Customer* top = dequeue(mall->upQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("Upward customer %d cannot board the escalator yet\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // 4. Attempt to board the first customer in the down queue
        pthread_mutex_lock(&mall_mutex);
        if(mall->downQueue->head){
            Customer* c = mall->downQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)){
                Customer* top = dequeue(mall->downQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("Downward customer %d cannot board the escalator yet\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // Print status again
        print_escalator_status();

        // [Removed Step 5 of original code: no random customers generated anymore]

        // 6. Print mall status
        pthread_mutex_lock(&mall_mutex);
        printf("Mall status: Total customers = %d, upQ = %d, downQ = %d, On escalator = %d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // 7. Termination condition: if no more customers remain, end
        if(mall->total_customers == 0){
            simulation_running = 0;
            pthread_mutex_unlock(&mall_mutex);
            break;
        }

        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);

        sleep(1);
    }

    printf("\n===== Simulation Ended =====\n");
    pthread_mutex_lock(&mall_mutex);
    printf("Remaining customers: %d\n", mall->total_customers);
    if(completed_customers > 0){
        double avg = (double)total_turnaround_time / completed_customers;
        printf("Average turnaround time = %.2f sec\n", avg);
    } else {
        printf("No customers completed their ride?\n");
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// Cleanup
// --------------------------------------------------
void cleanup_resources(){
    pthread_mutex_lock(&mall_mutex);
    Customer* c;
    while( (c=dequeue(mall->upQueue))!=NULL ) free(c);
    while( (c=dequeue(mall->downQueue))!=NULL ) free(c);

    // Clean up any remaining customers on the escalator
    for(int i=0; i<13; i++){
        if(mall->escalator->steps[i]){
            free(mall->escalator->steps[i]);
        }
    }
    free(mall->upQueue);
    free(mall->downQueue);
    free(mall->escalator);
    free(mall);
    pthread_mutex_unlock(&mall_mutex);
}

int main(int argc, char* argv[]){
    srand(time(NULL));

    // 1. Parse command line arguments: <EscalatorSteps <= 13>, <TotalCustomers <= 30>
    if(argc < 3){
        fprintf(stderr, "Usage: %s <EscalatorSteps <= 13> <TotalCustomers <= 30>\n", argv[0]);
        return 1;
    }

    g_escalator_capacity = atoi(argv[1]);
    if(g_escalator_capacity < 1 || g_escalator_capacity > 13){
        fprintf(stderr, "Error: escalator capacity must be between 1 and 13.\n");
        return 1;
    }

    int total_cust_to_generate = atoi(argv[2]);
    if(total_cust_to_generate < 0 || total_cust_to_generate > 30){
        fprintf(stderr, "Error: total customers must be between 0 and 30.\n");
        return 1;
    }
    // Here we set g_mall_capacity to total_cust_to_generate as the mall capacity.
    g_mall_capacity = total_cust_to_generate;
    
    // 2. Initialize recursive mutex and semaphore
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    sem_init(&escalator_capacity_sem, 0, g_escalator_capacity);

    // 3. Initialize mall
    mall = init_mall();

    // 4. Create fixed number of customer threads
    for(int i=0; i<total_cust_to_generate; i++){
        int dir = (rand() % 2 == 0) ? UP : DOWN;
        create_customer(dir);
        
        // Give threads some time (not strictly necessary, but used in original)
        usleep(10000);
    }

    // 5. Main loop
    mall_control_loop();

    // 6. Cleanup
    sleep(1);
    cleanup_resources();
    sem_destroy(&escalator_capacity_sem);
    pthread_mutex_destroy(&mall_mutex);

    return 0;
}
