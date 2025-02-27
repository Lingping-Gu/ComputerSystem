#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

// -------------------- 常量 --------------------
#define MAX_CUSTOMERS           30   // 商场容纳人数
#define MAX_ESCALATOR_CAPACITY  13   // 楼梯台阶数

#define UP    1
#define DOWN -1
#define IDLE  0

// -------------------- 全局互斥锁 + 信号量 --------------------
static pthread_mutex_t mall_mutex;
static sem_t escalator_capacity_sem; 

// -------------------- 数据结构 --------------------
typedef struct Customer {
    int id;
    int arrival_time;  // 到达时间(秒)
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

// 顾客线程参数结构
typedef struct {
    int direction;     // 方向
    int arrival_time;  // 到达时间
} CustomerThreadArgs;

// -------------------- 全局变量 --------------------
// 用于统计周转时间
static int total_turnaround_time = 0;
static int completed_customers   = 0;

// **记录当前方向已送了多少人** 
// 楼梯空后如果 >=5 且对向有人，就强制切方向
static int current_dir_boarded_count = 0;

// 全局ID自增
static int global_customer_id = 0;

Mall* mall = NULL;

// 线程是否应该继续运行
static int simulation_running = 1;

// -------------------- 函数声明 --------------------
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

// 新增：顾客线程函数
void* customer_thread(void* arg);

// --------------------------------------------------
// 初始化
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

// 创建顾客结构体（非线程）
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

// 顾客线程函数
void* customer_thread(void* arg) {
    CustomerThreadArgs* args = (CustomerThreadArgs*)arg;
    
    pthread_mutex_lock(&mall_mutex);
    int direction = args->direction;
    int arrival_time = args->arrival_time;
    
    // 创建顾客结构体
    Customer* c = create_customer_struct(direction, arrival_time);
    
    // 增加商场总人数
    mall->total_customers++;
    
    // 加入相应队列
    if (direction == UP) {
        enqueue(mall->upQueue, c);
    } else {
        enqueue(mall->downQueue, c);
    }
    
    pthread_mutex_unlock(&mall_mutex);
    
    // 释放参数内存
    free(args);
    
    // 线程退出
    return NULL;
}

// 创建顾客（启动新线程）
void create_customer(int direction) {
    // 创建线程参数
    CustomerThreadArgs* args = (CustomerThreadArgs*)malloc(sizeof(CustomerThreadArgs));
    if (!args) {
        perror("malloc customer thread args");
        exit(EXIT_FAILURE);
    }
    
    args->direction = direction;
    
    pthread_mutex_lock(&mall_mutex);
    args->arrival_time = mall->current_time;
    pthread_mutex_unlock(&mall_mutex);
    
    // 创建线程
    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, customer_thread, args) != 0) {
        perror("pthread_create");
        free(args);
        exit(EXIT_FAILURE);
    }
    
    // 分离线程，让它自行结束
    pthread_detach(thread_id);
    
    printf("顾客线程已创建，方向: %s\n", (direction==UP)?"上行":"下行");
}

// --------------------------------------------------
// 队列操作
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
    printf("顾客 %d 加入队列，方向: %s，到达时间: %d\n",
           c->id, 
           (q->direction==UP)?"上行":"下行", 
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
// 判断能否让这个顾客进电梯
// --------------------------------------------------
int can_customer_board(Customer* c){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    // 如果电梯满,不行
    if(e->num_people >= MAX_ESCALATOR_CAPACITY){
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    
    // 如果电梯是空闲(IDLE)，可以上，并设置方向
    if(e->direction == IDLE){
        e->direction = c->direction;
        current_dir_boarded_count = 0;
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // 如果电梯方向与顾客方向相同，可以上
    if(e->direction == c->direction){
        // 同时检查: 如果 current_dir_boarded_count>=5 && 对向有人 => 拒绝
        Queue* oppQ = (c->direction==UP)? mall->downQueue: mall->upQueue;
        if(oppQ->length>0 && current_dir_boarded_count>=5){
            pthread_mutex_unlock(&mall_mutex);
            return 0; 
        }
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    
    // 如果电梯方向与顾客方向不同，不能上
    pthread_mutex_unlock(&mall_mutex);
    return 0;
}

// --------------------------------------------------
// 顾客正式上电梯
// --------------------------------------------------
void board_customer(Customer* c){
    sem_wait(&escalator_capacity_sem); // 拿令牌

    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    
    // 方向已在can_customer_board中设置，不需要在这里设置
    
    // 放到入口
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    e->steps[entry] = c;
    e->num_people++;
    current_dir_boarded_count++;
    int wait_time = mall->current_time - c->arrival_time;
    printf("顾客 %d 上楼梯，方向: %s，等待=%d秒, 已运送=%d 人\n",
           c->id, 
           (c->direction==UP)?"上行":"下行",
           wait_time, current_dir_boarded_count);
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 每秒移动楼梯上的顾客
// --------------------------------------------------
void operate_escalator(){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    if(e->num_people>0){
        printf("楼梯方向=%s, 载客=%d\n",
               (e->direction==UP)?"上行":
               (e->direction==DOWN)?"下行":"空闲",
               e->num_people);

        // 上行
        if(e->direction==UP){
            // 顶部离开
            if(e->steps[MAX_ESCALATOR_CAPACITY-1]){
                Customer* c = e->steps[MAX_ESCALATOR_CAPACITY-1];
                int tat = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成上行,周转=%d秒\n", c->id, tat);
                total_turnaround_time += tat;
                completed_customers++;
                free(c);
                e->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                e->num_people--;
                mall->total_customers--;
                sem_post(&escalator_capacity_sem);
            }
            // 其余往上挪
            for(int i=MAX_ESCALATOR_CAPACITY-2; i>=0; i--){
                if(e->steps[i]){
                    e->steps[i+1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }
        // 下行
        else if(e->direction==DOWN){
            // 底部离开
            if(e->steps[0]){
                Customer* c = e->steps[0];
                int tat = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成下行,周转=%d秒\n", c->id, tat);
                total_turnaround_time += tat;
                completed_customers++;
                free(c);
                e->steps[0] = NULL;
                e->num_people--;
                mall->total_customers--;
                sem_post(&escalator_capacity_sem);
            }
            // 其余往下挪
            for(int i=1; i<MAX_ESCALATOR_CAPACITY; i++){
                if(e->steps[i]){
                    e->steps[i-1]=e->steps[i];
                    e->steps[i]=NULL;
                }
            }
        }
        // 如果空了 => 检查是否要切到对向
        if(e->num_people==0){
            printf("楼梯已空. 该方向已运送=%d 人\n", current_dir_boarded_count);

            // 若该方向已送>=5，且对向有人 => 强制切
            Queue* oppQ = (e->direction==UP)? mall->downQueue: mall->upQueue;
            int oppLen  = oppQ->length;

            if(current_dir_boarded_count>=5 && oppLen>0){
                printf("已运送>=5人,对向有人. 强制切到%s\n",
                       (e->direction==UP)?"下行":"上行");
                e->direction = - e->direction; // 反向
            } else {
                // 否则保持IDLE
                e->direction = IDLE;
            }
            // 重置计数
            current_dir_boarded_count=0;
        }
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 打印楼梯状态
// --------------------------------------------------
void print_escalator_status(){
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    printf("楼梯状态: [");
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
        if(e->steps[i]) {
            printf("%d", e->steps[i]->id);
        } else {
            printf("0");
        }
        if(i<MAX_ESCALATOR_CAPACITY-1) printf(",");
    }
    printf("], 方向: %s\n",
           (e->direction==UP)?"上行":
           (e->direction==DOWN)?"下行":"空闲");
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 主循环
// --------------------------------------------------
void mall_control_loop(int simulation_time){
    while(simulation_running){
        pthread_mutex_lock(&mall_mutex);
        printf("\n----- 时间: %d 秒 -----\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. 运行楼梯
        operate_escalator();

        // 2. 打印楼梯状态
        print_escalator_status();

        // 3. 让上行队首上楼梯(若能)
        pthread_mutex_lock(&mall_mutex);
        if(mall->upQueue->head){
            Customer* c = mall->upQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)){
                Customer* top = dequeue(mall->upQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("上行顾客 %d 暂时不能上楼梯\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // 4. 让下行队首上楼梯(若能)
        pthread_mutex_lock(&mall_mutex);
        if(mall->downQueue->head){
            Customer* c = mall->downQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)){
                Customer* top = dequeue(mall->downQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("下行顾客 %d 暂时不能上楼梯\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // 打印楼梯状态
        print_escalator_status();

        // 5. 生成新顾客(若<100秒)
        pthread_mutex_lock(&mall_mutex);
        if(mall->current_time < simulation_time){
            int new_cust = rand() % 3; // 0~2
            if(new_cust > 0){
                printf("本秒新来 %d 个顾客\n", new_cust);
                for(int i=0; i<new_cust; i++){
                    if(mall->total_customers >= MAX_CUSTOMERS){
                        printf("商场满,不接待新顾客\n");
                        break;
                    }
                    int dir = (rand() % 2 == 0) ? UP : DOWN;
                    // 在这里使用新的多线程创建顾客
                    pthread_mutex_unlock(&mall_mutex);
                    create_customer(dir);
                    pthread_mutex_lock(&mall_mutex);
                }
            } else {
                printf("本秒没有新顾客\n");
            }
        } else {
            printf(">= %d秒,不再生成新顾客\n", simulation_time);
        }

        // 6. 打印商场状态
        printf("商场状态: 总人数=%d, upQ=%d, downQ=%d, 楼梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);
               
        // 7. 结束条件
        if(mall->current_time >= simulation_time && mall->total_customers == 0){
            simulation_running = 0;
            pthread_mutex_unlock(&mall_mutex);
            break;
        }
        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);

        sleep(1);
    }

    printf("\n===== 模拟结束 =====\n");
    pthread_mutex_lock(&mall_mutex);
    printf("剩余顾客数: %d\n", mall->total_customers);
    if(completed_customers > 0){
        double avg = (double)total_turnaround_time / completed_customers;
        printf("平均周转时间=%.2f秒\n", avg);
    } else {
        printf("无完成乘梯顾客?\n");
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

    // 楼梯上残余
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

    // 初始化递归互斥锁
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // 信号量
    sem_init(&escalator_capacity_sem, 0, MAX_ESCALATOR_CAPACITY);

    // 解析命令行
    int init_customers=10;
    if(argc>1){
        init_customers=atoi(argv[1]);
        if(init_customers<0||init_customers>MAX_CUSTOMERS){
            printf("初始顾客数[0..%d]\n",MAX_CUSTOMERS);
            return 1;
        }
    }

    mall=init_mall();

    // 创建初始顾客线程
    for(int i=0; i<init_customers; i++){
        int dir=(rand()%2==0)?UP:DOWN;
        create_customer(dir);
        
        // 给线程一点时间完成初始化
        usleep(10000);
    }

    // 进主循环
    mall_control_loop(100);

    // 确保所有线程都结束
    sleep(1);

    cleanup_resources();
    sem_destroy(&escalator_capacity_sem);
    pthread_mutex_destroy(&mall_mutex);

    return 0;
}