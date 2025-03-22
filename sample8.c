#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

// -------------------- Constants (改为变量) --------------------
// 原来的宏定义被去掉，这里改成全局变量，由用户输入决定大小；并且会在 main() 里赋值。
// 这两个全局变量在代码里替代了原先的 MAX_ESCALATOR_CAPACITY 和 MAX_CUSTOMERS。
static int g_escalator_capacity  = 13; // 不再固定写死13，用户输入时不可 >13
static int g_mall_capacity       = 30; // 不再固定写死30，用户输入时不可 >30

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
    int position;      // 0 or 14   (原代码写死14，不做更改)
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
 * 注意这里 Escalator 中的 steps 数组，原代码是写死 [MAX_ESCALATOR_CAPACITY]。
 * 我们继续用一个固定大小 [13]，但实际只使用前 g_escalator_capacity 个位置。
 * （因为用户输入不超过13，所以这样做是安全的。）
 */
typedef struct {
    Customer* steps[13];  // 物理上分配 13 个，但只在逻辑上使用 g_escalator_capacity 个
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

// 原逻辑所需，用来判断是否需要“强制切换方向”的条件计数
static int current_dir_boarded_count = 0;

// Global ID auto-increment
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

// 这里去掉了“随机生成顾客”的过程，所以 mall_control_loop 不再根据时间生成顾客，
// 只负责让已经在队列和电梯上的顾客完成乘梯。
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
    for(int i=0; i<13; i++){  // 原来是 i<MAX_ESCALATOR_CAPACITY
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
    // 原代码这里写死 0 或 14，不去改动
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
    
    // Detach Thread
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

    // 如果电梯已经满，则不能上
    if(e->num_people >= g_escalator_capacity) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    
    // 如果电梯是空闲的，则可以上并设置方向
    if(e->direction == IDLE){
        e->direction = c->direction;
        current_dir_boarded_count = 0;
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // 如果电梯方向和顾客方向一致，可以上
    if(e->direction == c->direction){
        // 如果已经运了 >=5 人 且 对面方向上还有人排队，则不让再上
        Queue* oppQ = (c->direction==UP)? mall->downQueue: mall->upQueue;
        if(oppQ->length>0 && current_dir_boarded_count>=5){
            pthread_mutex_unlock(&mall_mutex);
            return 0; 
        }
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // 方向不符，不能上
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
    
    // entry index
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
            // Departure at the top
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
            // Move the rest upward
            for(int i=g_escalator_capacity-2; i>=0; i--){
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
            for(int i=1; i<g_escalator_capacity; i++){
                if(e->steps[i]){
                    e->steps[i-1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }

        // 若电梯此刻空了，就判断是否要强制切换方向
        if(e->num_people==0){
            printf("Escalator is now empty. Passengers transported in this direction = %d\n", current_dir_boarded_count);

            // 如果已运送 >=5 人 且 对面有人排队 => 强制切换
            Queue* oppQ = (e->direction==UP)? mall->downQueue: mall->upQueue;
            int oppLen  = oppQ->length;

            if(current_dir_boarded_count>=5 && oppLen>0){
                printf(">=5 people have crossed, and there're customers waiting in the opposite direction. Force direction switch to %s\n",
                       (e->direction==UP)?"Down":"Up");
                e->direction = - e->direction; 
            } else {
                e->direction = IDLE;
            }
            // 重置计数
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
    // 只打印 g_escalator_capacity 个台阶对应的存储
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
// Main loop (去掉了“随机生成顾客”的部分)
// --------------------------------------------------
void mall_control_loop(){
    while(simulation_running){
        pthread_mutex_lock(&mall_mutex);
        printf("\n----- Time: %d sec -----\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. 运行电梯
        operate_escalator();

        // 2. 打印电梯状态
        print_escalator_status();

        // 3. 尝试让上队列的队首顾客上电梯
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

        // 4. 尝试让下队列的队首顾客上电梯
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

        // 再打印一次状态
        print_escalator_status();

        // 【原代码第5步：随机生成顾客】已经去除，改为空操作
        // printf("No new customers are generated in this version.\n");

        // 6. 打印商场状态
        pthread_mutex_lock(&mall_mutex);
        printf("Mall status: Total customers = %d, upQ = %d, downQ = %d, On escalator = %d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // 7. 判断结束条件：当商场内所有顾客都处理完，退出循环
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
// cleanup
// --------------------------------------------------
void cleanup_resources(){
    pthread_mutex_lock(&mall_mutex);
    Customer* c;
    while( (c=dequeue(mall->upQueue))!=NULL ) free(c);
    while( (c=dequeue(mall->downQueue))!=NULL ) free(c);

    // 清理电梯上剩余的顾客
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

    // -------------------- 1. 参数解析 --------------------
    // 需要两个参数：argv[1] = 电梯台阶数（<=13）, argv[2] = 总顾客数（<=30）
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
    // g_mall_capacity 不一定要等于 total_cust_to_generate，但假设这里我们就直接用它
    // 或者你也可以把 g_mall_capacity 理解为“可容纳人数上限”。
    g_mall_capacity = total_cust_to_generate;
    
    // -------------------- 2. 初始化 Mutex & Semaphore --------------------
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // 注意，这里的信号量初值也改为 g_escalator_capacity
    sem_init(&escalator_capacity_sem, 0, g_escalator_capacity);

    // -------------------- 3. 初始化商场 --------------------
    mall = init_mall();

    // -------------------- 4. 创建固定数量的顾客线程 --------------------
    // 不再在循环里随机生成，这里一次性生成 total_cust_to_generate 个
    for(int i=0; i<total_cust_to_generate; i++){
        int dir = (rand() % 2 == 0) ? UP : DOWN;
        create_customer(dir);
        
        // 给线程一点时间初始化，非必须，但原代码里也有
        usleep(10000);
    }

    // -------------------- 5. 进入主循环 --------------------
    // 原代码是 mall_control_loop(100)，这里改为不带参数
    mall_control_loop();

    // -------------------- 6. 清理资源 & 退出 --------------------
    sleep(1);
    cleanup_resources();
    sem_destroy(&escalator_capacity_sem);
    pthread_mutex_destroy(&mall_mutex);

    return 0;
}
