#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

// -------------------- Constants --------------------
#define MAX_CUSTOMERS           30   // Maximum number of customers in the mall
#define MAX_ESCALATOR_CAPACITY  13   // Number of steps on the escalator

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
    int position;      // 0 or 14
    struct Customer* next;
    struct Customer* prev;
} Customer;

typedef struct {
    Customer* head;
    Customer* tail;
    int length;
    int direction; // 1=UP, -1=DOWN
} Queue;

typedef struct {
    Customer* steps[MAX_ESCALATOR_CAPACITY]; 
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
// Used for tracking turnaround time
static int total_turnaround_time = 0;
static int completed_customers   = 0;

// **Tracks how many people have been transported in the current direction** 
// If the escalator becomes empty and this current_dir_boarded_count >=5 while there are people waiting in the opposite direction, force a direction switch.
static int current_dir_boarded_count = 0;

// Global ID auto-increment
static int global_customer_id = 0;

Mall* mall = NULL;

// Check if threads should keep running
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
void mall_control_loop(int simulation_time);
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
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
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

// Create Customer Structure (Non-Thread)
Customer* create_customer_struct(int direction, int arrival_time) {
    pthread_mutex_lock(&mall_mutex);
    global_customer_id++;
    Customer* c = (Customer*)malloc(sizeof(Customer));
    c->id = global_customer_id;
    c->arrival_time = arrival_time;
    c->direction    = direction;
    c->position     = (direction==UP)? 0 : 14;
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
    
    // Create Customer Structure
    Customer* c = create_customer_struct(direction, arrival_time);
    
    // Increase Total Customer Number in the Mall
    mall->total_customers++;
    
    // Add to the Appropriate Queue
    if (direction == UP) {
        enqueue(mall->upQueue, c);
    } else {
        enqueue(mall->downQueue, c);
    }
    
    pthread_mutex_unlock(&mall_mutex);
    
    // Free the Argument Memory
    free(args);
    
    // Thread Exit
    return NULL;
}

// Create Customer (Start New Thread)
void create_customer(int direction) {
    // Create Thread Arguments
    CustomerThreadArgs* args = (CustomerThreadArgs*)malloc(sizeof(CustomerThreadArgs));
    if (!args) {
        perror("malloc customer thread args");
        exit(EXIT_FAILURE);
    }
    
    args->direction = direction;
    
    pthread_mutex_lock(&mall_mutex);
    args->arrival_time = mall->current_time;
    pthread_mutex_unlock(&mall_mutex);
    
    // Create Thread
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, customer_thread, args) != 0) {
        perror("pthread_create");
        free(args);
        exit(EXIT_FAILURE);
    }
    
    // Detach Thread to Allow It to Exit on Its Own
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
// Check If a Customer Can Board the Escalator
// --------------------------------------------------
int can_customer_board(Customer* c){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    // If the escalator is full, customer cannot board
    if(e->num_people >= MAX_ESCALATOR_CAPACITY){
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
    
    // If the escalator direction matches the customer's direction, they can board
    if(e->direction == c->direction){
        // Additionally check: If current_dir_boarded_count >= 5 and opposite direction has waiting customers => Deny boarding
        Queue* oppQ = (c->direction==UP)? mall->downQueue: mall->upQueue;
        if(oppQ->length>0 && current_dir_boarded_count>=5){
            pthread_mutex_unlock(&mall_mutex);
            return 0; 
        }
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // If the escalator direction is opposite to the customer's, they cannot board
    pthread_mutex_unlock(&mall_mutex);
    return 0;
}

// --------------------------------------------------
// Customer Boards the Escalator
// --------------------------------------------------
void board_customer(Customer* c){
    sem_wait(&escalator_capacity_sem); // Acquire the lock

    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    
    // Direction has already been set in can_customer_board, no need to set it here
    
    // Place at Entry Point
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
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
            // Departure at the top
            if(e->steps[MAX_ESCALATOR_CAPACITY-1]){
                Customer* c = e->steps[MAX_ESCALATOR_CAPACITY-1];
                int tat = mall->current_time - c->arrival_time;
                printf("Customer %d completed upward travel, Turnaround time = %d sec\n", c->id, tat);
                total_turnaround_time += tat;
                completed_customers++;
                free(c);
                e->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                e->num_people--;
                mall->total_customers--;
                sem_post(&escalator_capacity_sem);
            }
            // Move the rest upward
            for(int i=MAX_ESCALATOR_CAPACITY-2; i>=0; i--){
                if(e->steps[i]){
                    e->steps[i+1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }
        // Moving down
        else if(e->direction==DOWN){
            // Departure at the bottom
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
            // Move the rest downward
            for(int i=1; i<MAX_ESCALATOR_CAPACITY; i++){
                if(e->steps[i]){
                    e->steps[i-1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }
        // If the escalator is empty => check whether to switch direction
        if(e->num_people==0){
            printf("Escalator is now empty. Passengers transported in this direction = %d\n", current_dir_boarded_count);

            // If at least 5 passengers were transported in this direction and there are people waiting in the opposite queue => Force a switch
            Queue* oppQ = (e->direction==UP)? mall->downQueue: mall->upQueue;
            int oppLen  = oppQ->length;

            if(current_dir_boarded_count>=5 && oppLen>0){
                printf(">=5 people have crossed, and there're customers waiting in the opposite direction. Force direction switch to %s\n",
                       (e->direction==UP)?"Down":"Up");
                e->direction = - e->direction; // 反向
            } else {
                // Otherwise, remain idle
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
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
        if(e->steps[i]) {
            printf("%d", e->steps[i]->id);
        } else {
            printf("0");
        }
        if(i<MAX_ESCALATOR_CAPACITY-1) printf(",");
    }
    printf("], Direction: %s\n",
           (e->direction==UP)?"Up":
           (e->direction==DOWN)?"Down":"Idle");
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------
void mall_control_loop(int simulation_time){
    while(simulation_running){
        pthread_mutex_lock(&mall_mutex);
        printf("\n----- Time: %d sec -----\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. Run the escalator
        operate_escalator();

        // 2. Print escalator status
        print_escalator_status();

        // 3. Allow the frontmost customer in the up queue to board (if possible)
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

        // 4. Allow the frontmost customer in the down queue to board (if possible)
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

        // Print escalator status
        print_escalator_status();

        // 5. Generate new customers (if <100 seconds)
        pthread_mutex_lock(&mall_mutex);
        if(mall->current_time < simulation_time){
            int new_cust = rand() % 3; // 0~2
            if(new_cust > 0){
                printf("%d new customers arrived this second\n", new_cust);
                for(int i=0; i<new_cust; i++){
                    if(mall->total_customers >= MAX_CUSTOMERS){
                        printf("Mall is full, no new customers allowed\n");
                        break;
                    }
                    int dir = (rand() % 2 == 0) ? UP : DOWN;
                    // Create new customers using multithreading
                    pthread_mutex_unlock(&mall_mutex);
                    create_customer(dir);
                    pthread_mutex_lock(&mall_mutex);
                }
            } else {
                printf("No new customers this second\n");
            }
        } else {
            printf(">= %d seconds, no more new customers will be generated\n", simulation_time);
        }

        // 6. Print mall status
        printf("Mall status: Total customers = %d, upQ = %d, downQ = %d, On escalator = %d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);
               
        // 7. Termination condition
        if(mall->current_time >= simulation_time && mall->total_customers == 0){
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
// cleanup
// --------------------------------------------------
void cleanup_resources(){
    pthread_mutex_lock(&mall_mutex);
    Customer* c;
    while( (c=dequeue(mall->upQueue))!=NULL ) free(c);
    while( (c=dequeue(mall->downQueue))!=NULL ) free(c);

    // Remaining customers on the escalator
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
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

    // Initialize recursive mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // Semaphore
    sem_init(&escalator_capacity_sem, 0, MAX_ESCALATOR_CAPACITY);

    // Parse command line arguments
    int init_customers=10;
    if(argc>1){
        init_customers=atoi(argv[1]);
        if(init_customers<0||init_customers>MAX_CUSTOMERS){
            printf("Initial number of customers must be between [0..%d]\n", MAX_CUSTOMERS);
            return 1;
        }
    }

    mall=init_mall();

    // Create initial customer threads
    for(int i=0; i<init_customers; i++){
        int dir=(rand()%2==0)?UP:DOWN;
        create_customer(dir);
        
        // Give threads some time to initialize
        usleep(10000);
    }

    // Enter main loop
    mall_control_loop(100);

    // Ensure all threads have finished
    sleep(1);

    cleanup_resources();
    sem_destroy(&escalator_capacity_sem);
    pthread_mutex_destroy(&mall_mutex);

    return 0;
}