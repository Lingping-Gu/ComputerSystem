#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30           // 商场最多容纳人数
#define MAX_ESCALATOR_CAPACITY 13  // 扶梯最多容纳人数
#define MAX_WAIT_TIME 41           // 顾客最大等待时间
#define SWITCH_THRESHOLD (MAX_WAIT_TIME - MAX_ESCALATOR_CAPACITY) // 28秒，决定换方向
#define UP   1   // 上行方向
#define DOWN -1  // 下行方向
#define IDLE  0  // 空闲状态

// -------------------- 全局互斥锁 --------------------
static pthread_mutex_t mall_mutex; // 在 main() 中初始化(使用递归锁)

// -------------------- 结构体定义 --------------------
typedef struct Customer {
    int id;            
    int arrival_time;  
    int direction;     
    int position;      
    struct Customer* next; 
    struct Customer* prev;
} Customer;

typedef struct {
    Customer* head;     
    Customer* tail;     
    int length;         
    int direction;      
} Queue;

typedef struct {
    Customer* steps[MAX_ESCALATOR_CAPACITY];  
    int direction;      
    int num_people;     
} Escalator;

typedef struct {
    Queue* upQueue;         
    Queue* downQueue;       
    Escalator* escalator;   
    int total_customers;    
    int current_time;       
} Mall;

// ============ 用于平均周转时间统计 ============ //
int total_turnaround_time = 0;   
int completed_customers   = 0;   
// ============================================ //

Mall* mall = NULL;

// ---------------- 函数声明 ----------------
Queue*      init_queue(int dir);
Escalator*  init_escalator();
Mall*       init_mall();
Customer*   create_customer(int id, int direction);

void enqueue(Queue* q, Customer* c);
Customer* dequeue(Queue* q);

int  get_waiting_time(Customer* c);
int  should_wait_for_opposite(Queue* opposite_queue);

// ============ 关键逻辑修改处： ============
// 在这里扩展 can_customer_board(...)，包含对入口阶梯是否空闲的检查
int  can_customer_board(Customer* c);
// board_customer 不再做“重回队列”的操作
void board_customer(Customer* c);

void operate_escalator();
void print_escalator_status();
void mall_control_loop(int simulation_time);
void cleanup_resources();

// --------------------------------------------------
// 初始化相关函数
// --------------------------------------------------
Queue* init_queue(int dir)
{
    pthread_mutex_lock(&mall_mutex);
    Queue* q = (Queue*)malloc(sizeof(Queue));
    if(!q) {
        perror("Failed to allocate memory for queue");
        exit(EXIT_FAILURE);
    }
    q->head      = NULL;
    q->tail      = NULL;
    q->length    = 0;
    q->direction = dir;
    pthread_mutex_unlock(&mall_mutex);
    return q;
}

Escalator* init_escalator()
{
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = (Escalator*)malloc(sizeof(Escalator));
    if(!e) {
        perror("Failed to allocate memory for escalator");
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

Mall* init_mall()
{
    pthread_mutex_lock(&mall_mutex);
    Mall* m = (Mall*)malloc(sizeof(Mall));
    if(!m) {
        perror("Failed to allocate memory for mall");
        exit(EXIT_FAILURE);
    }
    m->upQueue        = init_queue(UP);
    m->downQueue      = init_queue(DOWN);
    m->escalator      = init_escalator();
    m->total_customers= 0;
    m->current_time   = 0;
    pthread_mutex_unlock(&mall_mutex);
    return m;
}

Customer* create_customer(int id, int direction)
{
    pthread_mutex_lock(&mall_mutex);
    Customer* c = (Customer*)malloc(sizeof(Customer));
    if(!c) {
        perror("Failed to allocate memory for customer");
        exit(EXIT_FAILURE);
    }
    c->id           = id;
    c->arrival_time = mall->current_time;
    c->direction    = direction;
    // position可选，在你原逻辑是 0(楼下) 或 14(楼上), 这里不再细用
    c->position     = (direction == UP) ? 0 : 14;
    c->next = NULL;
    c->prev = NULL;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// --------------------------------------------------
// 队列基本操作
// --------------------------------------------------
void enqueue(Queue* q, Customer* c)
{
    pthread_mutex_lock(&mall_mutex);
    if(!q->head) {
        q->head = c;
        q->tail = c;
    } else {
        q->tail->next = c;
        c->prev       = q->tail;
        q->tail       = c;
    }
    q->length++;
    printf("顾客 %d 加入队列，方向: %s，到达时间: %d\n",
           c->id, (c->direction == UP)?"上行":"下行", c->arrival_time);
    pthread_mutex_unlock(&mall_mutex);
}

Customer* dequeue(Queue* q)
{
    pthread_mutex_lock(&mall_mutex);
    if(!q->head) {
        pthread_mutex_unlock(&mall_mutex);
        return NULL;
    }
    Customer* c = q->head;
    q->head     = c->next;
    if(!q->head) {
        q->tail = NULL;
    } else {
        q->head->prev = NULL;
    }
    q->length--;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// --------------------------------------------------
// 工具函数
// --------------------------------------------------
int get_waiting_time(Customer* c)
{
    if(!c) return 0;
    pthread_mutex_lock(&mall_mutex);
    int wt = mall->current_time - c->arrival_time;
    pthread_mutex_unlock(&mall_mutex);
    return wt;
}

// 判断是否应该等待对向顾客
int should_wait_for_opposite(Queue* opposite_queue)
{
    pthread_mutex_lock(&mall_mutex);
    if(!opposite_queue->head) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    int wait_time = mall->current_time - opposite_queue->head->arrival_time;
    int result    = (wait_time >= SWITCH_THRESHOLD);
    pthread_mutex_unlock(&mall_mutex);
    return result;
}

// --------------------------------------------------
// 顾客能否上电梯: 包含入口阶梯是否空闲
// --------------------------------------------------
int can_customer_board(Customer* c)
{
    if(!c) return 0;
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    // 1) 电梯是否满
    if(e->num_people >= MAX_ESCALATOR_CAPACITY) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 2) 方向是否匹配 (或电梯空闲)
    if(e->direction != IDLE && e->direction != c->direction) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 3) 对向队首是否等待过长
    Queue* oppQueue = (c->direction == UP)? mall->downQueue: mall->upQueue;
    if(should_wait_for_opposite(oppQueue)) {
        printf("顾客 %d 等待，因为逆向队首等待时间过长\n", c->id);
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 4) 入口是否空闲
    int entry_position = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    if(e->steps[entry_position] != NULL) {
        // 入口被占用，就不能上
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }

    // 如果全部满足 => 可以上
    pthread_mutex_unlock(&mall_mutex);
    return 1;
}

// --------------------------------------------------
// 真正把顾客放上电梯(不再做 re-enqueue)
// --------------------------------------------------
void board_customer(Customer* c)
{
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    // 若电梯空闲 => 设定方向
    if(e->direction == IDLE) {
        e->direction = c->direction;
        printf("电梯方向设为: %s\n", (c->direction==UP)?"上行":"下行");
    }
    int entry_position = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    // 放上电梯
    e->steps[entry_position] = c;
    e->num_people++;
    int wait_time = mall->current_time - c->arrival_time;
    printf("顾客 %d 上电梯，方向: %s，等待时间: %d秒\n",
           c->id, (c->direction==UP)?"上行":"下行", wait_time);

    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 电梯每秒的运行(移动顾客、换方向等)
// --------------------------------------------------
void operate_escalator()
{
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    if(e->num_people>0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n",
               (e->direction==UP)?"上行":"下行",
               e->num_people);

        // 上行
        if(e->direction == UP) {
            // 最后一格顾客离开
            if(e->steps[MAX_ESCALATOR_CAPACITY-1] != NULL) {
                Customer* c = e->steps[MAX_ESCALATOR_CAPACITY-1];
                int turnaround = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(上行)，周转时间: %d秒\n", c->id, turnaround);

                total_turnaround_time += turnaround;
                completed_customers++;
                free(c);

                e->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                e->num_people--;
                mall->total_customers--;
            }
            // 其余往上挪
            for(int i=MAX_ESCALATOR_CAPACITY-2; i>=0; i--) {
                if(e->steps[i]) {
                    e->steps[i+1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }
        // 下行
        else if(e->direction == DOWN) {
            // 第一格顾客离开
            if(e->steps[0] != NULL) {
                Customer* c = e->steps[0];
                int turnaround = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(下行)，周转时间: %d秒\n", c->id, turnaround);

                total_turnaround_time += turnaround;
                completed_customers++;
                free(c);

                e->steps[0] = NULL;
                e->num_people--;
                mall->total_customers--;
            }
            // 其余往下挪
            for(int i=1; i<MAX_ESCALATOR_CAPACITY; i++) {
                if(e->steps[i]) {
                    e->steps[i-1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }

        // 若电梯空了 => 可能需要切方向 or 置空闲
        if(e->num_people==0) {
            Queue* curQ = (e->direction==UP)? mall->upQueue : mall->downQueue;
            Queue* oppQ = (e->direction==UP)? mall->downQueue: mall->upQueue;
            if((curQ->head == NULL && oppQ->head != NULL) ||
               should_wait_for_opposite(oppQ)) {
                // 切换方向
                printf("电梯方向切换: %s -> %s\n",
                       (e->direction==UP)?"上行":"下行",
                       (e->direction==UP)?"下行":"上行");
                e->direction = -e->direction; 
            }
            else if(curQ->head==NULL && oppQ->head==NULL) {
                // 都没人 => 空闲
                printf("电梯空闲\n");
                e->direction = IDLE;
            }
        }
    }

    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 打印电梯当前状态
// --------------------------------------------------
void print_escalator_status()
{
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    printf("电梯状态: [");
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
        if(e->steps[i]) printf("%d", e->steps[i]->id);
        else            printf("0");
        if(i<MAX_ESCALATOR_CAPACITY-1) printf(", ");
    }
    printf("] 方向: %s\n",
           (e->direction==UP)?"上行":
           (e->direction==DOWN)?"下行":"空闲");
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 主循环：每秒调用 operate_escalator，检查队列等
// --------------------------------------------------
void mall_control_loop(int simulation_time)
{
    // “simulation_time=100” 表示 100秒后停止接收顾客；等顾客清空后结束
    while(1) {
        pthread_mutex_lock(&mall_mutex);
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // (1) 运行电梯
        operate_escalator();
        // (2) 打印电梯状态
        print_escalator_status();

        // (3) 尝试让上行队列队首上电梯
        pthread_mutex_lock(&mall_mutex);
        if(mall->upQueue->head) {
            Customer* c = mall->upQueue->head;
            int wt = mall->current_time - c->arrival_time;
            printf("上行队首顾客 %d，等待时间: %d 秒\n", c->id, wt);
            pthread_mutex_unlock(&mall_mutex);

            // 判断能否上
            if(can_customer_board(c)) {
                // 只有能上电梯时，才 dequeue
                Customer* top = dequeue(mall->upQueue);
                // 安排上电梯
                board_customer(top);
            } else {
                printf("上行队首顾客 %d 不能上电梯\n", c->id);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // (4) 尝试让下行队列队首上电梯
        pthread_mutex_lock(&mall_mutex);
        if(mall->downQueue->head) {
            Customer* c = mall->downQueue->head;
            int wt = mall->current_time - c->arrival_time;
            printf("下行队首顾客 %d，等待时间: %d 秒\n", c->id, wt);
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)) {
                Customer* top = dequeue(mall->downQueue);
                board_customer(top);
            } else {
                printf("下行队首顾客 %d 不能上电梯\n", c->id);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // (5) 判断是否生成新顾客(若时间<100秒 && 未达容量)
        pthread_mutex_lock(&mall_mutex);
        if(mall->current_time < simulation_time) {
            int max_new = MAX_CUSTOMERS - mall->total_customers;
            if(max_new>0) {
                // 比如随机生成 0~2 名新顾客
                int new_cust = rand() % 3;
                if(new_cust>0) {
                    printf("本秒生成 %d 个新顾客\n", new_cust);
                    for(int i=0; i<new_cust; i++){
                        int id = mall->total_customers + 1;
                        int dir= (rand()%2==0)? UP:DOWN;
                        Customer* nc = create_customer(id, dir);
                        mall->total_customers++;
                        if(dir==UP)   enqueue(mall->upQueue, nc);
                        else          enqueue(mall->downQueue, nc);
                    }
                } else {
                    printf("本秒没有新顾客到达\n");
                }
            } else {
                printf("商场已满，无法接收新顾客\n");
            }
        } else {
            printf("时间已达(或超过) %d 秒，不再接收新顾客\n", simulation_time);
        }

        // (6) 打印一下当前商场状态
        printf("当前商场状态: 总人数=%d, 上行队列=%d, 下行队列=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // (7) 若当前时间>=100秒 且 商场顾客已清空 => 结束
        if(mall->current_time >= simulation_time && mall->total_customers==0){
            pthread_mutex_unlock(&mall_mutex);
            break;
        }

        // 时间加1，解锁，sleep 1 秒
        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);
        sleep(1);
    }

    // 结束后打印周转时间
    printf("\n========== 模拟结束 ==========\n");
    pthread_mutex_lock(&mall_mutex);
    printf("剩余顾客数: %d\n", mall->total_customers);
    if(completed_customers>0){
        double avg_turnaround = (double)total_turnaround_time/completed_customers;
        printf("所有顾客的平均周转时间: %.2f 秒\n", avg_turnaround);
    } else {
        printf("没有完成乘梯的顾客，无法计算平均周转时间。\n");
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 资源清理
// --------------------------------------------------
void cleanup_resources()
{
    pthread_mutex_lock(&mall_mutex);
    // 释放队列
    Customer* c;
    while((c=dequeue(mall->upQueue))!=NULL)   free(c);
    while((c=dequeue(mall->downQueue))!=NULL) free(c);

    // 释放电梯上残余顾客
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
        if(mall->escalator->steps[i]){
            free(mall->escalator->steps[i]);
        }
    }
    // 释放结构
    free(mall->upQueue);
    free(mall->downQueue);
    free(mall->escalator);
    free(mall);
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// main函数
// --------------------------------------------------
int main(int argc, char* argv[])
{
    srand(time(NULL));

    // 初始化递归互斥锁
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    int num_customers = 10; 
    int num_steps     = 13; 
    if(argc>1){
        num_customers = atoi(argv[1]);
        if(num_customers<0 || num_customers>MAX_CUSTOMERS){
            printf("初始顾客数量必须在0到%d之间\n", MAX_CUSTOMERS);
            return EXIT_FAILURE;
        }
    }
    if(argc>2){
        num_steps=atoi(argv[2]);
        if(num_steps<=0 || num_steps>MAX_ESCALATOR_CAPACITY){
            printf("楼梯数量必须在1到%d之间\n", MAX_ESCALATOR_CAPACITY);
            return EXIT_FAILURE;
        }
    }
    printf("开始模拟，初始顾客数量: %d, 楼梯数量: %d\n", num_customers, num_steps);

    // 初始化商场
    mall = init_mall();

    // 创建初始顾客
    for(int i=0; i<num_customers; i++){
        int dir = (rand()%2==0)? UP:DOWN;
        Customer* c = create_customer(i+1, dir);
        mall->total_customers++;
        if(dir==UP)  enqueue(mall->upQueue,c);
        else         enqueue(mall->downQueue,c);
    }

    // 至少模拟100秒，之后等顾客清空
    mall_control_loop(100);

    // 结束前清理
    cleanup_resources();

    // 销毁互斥锁
    pthread_mutex_destroy(&mall_mutex);
    return EXIT_SUCCESS;
}
