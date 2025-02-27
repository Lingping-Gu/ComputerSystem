#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30
#define MAX_ESCALATOR_CAPACITY 13

// 新增两个关键参数：批量上限 + 队列人数差额阈值
#define BATCH_LIMIT    2   // 连续运送2批后，检查是否切向另一方向
#define DIFF_THRESHOLD 8   // 另一边人数比当前多 >= 8 时也可强制切换

// 方向
#define UP    1
#define DOWN -1
#define IDLE  0

// -------------------- 全局互斥锁 --------------------
static pthread_mutex_t mall_mutex; // 在 main() 中初始化(使用递归锁)

// -------------------- 结构体定义 --------------------
typedef struct Customer {
    int id;
    int arrival_time;   // 到达时的时间戳
    int direction;      // UP / DOWN
    int position;       // 0 or 14 (你原来存着楼层位置信息)
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
    int direction;   // UP / DOWN / IDLE
    int num_people;
} Escalator;

typedef struct {
    Queue* upQueue;
    Queue* downQueue;
    Escalator* escalator;
    int total_customers; // 当前商场内总顾客数(包括在排队+在电梯上)
    int current_time;    // 模拟的当前时间(秒)
} Mall;

// ============ 用于统计 ============ //
int total_turnaround_time = 0;
int completed_customers   = 0;
// ============ 批量计数 ============ //
int batch_count = 0;  // 当前方向已连续完成了多少个批次

Mall* mall = NULL;

// -------- 函数声明 --------
Queue* init_queue(int dir);
Escalator* init_escalator();
Mall* init_mall();
Customer* create_customer(int id, int direction);

void enqueue(Queue* q, Customer* c);
Customer* dequeue(Queue* q);

int  can_customer_board(Customer* c);
void board_customer(Customer* c);

void operate_escalator();
void print_escalator_status();
void mall_control_loop(int simulation_time);
void cleanup_resources();

// --------------------------------------------------
// 初始化相关函数
// --------------------------------------------------
Queue* init_queue(int dir) {
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

Escalator* init_escalator() {
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

Mall* init_mall() {
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

// 创建顾客
Customer* create_customer(int id, int direction) {
    pthread_mutex_lock(&mall_mutex);
    Customer* c = (Customer*)malloc(sizeof(Customer));
    if(!c) {
        perror("Failed to allocate memory for customer");
        exit(EXIT_FAILURE);
    }
    c->id           = id;
    c->arrival_time = mall->current_time;
    c->direction    = direction;
    c->position     = (direction==UP)? 0 : 14;  // 你原来的设定
    c->next = NULL;
    c->prev = NULL;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// --------------------------------------------------
// 队列基本操作
// --------------------------------------------------
void enqueue(Queue* q, Customer* c) {
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
           c->id, (c->direction==UP)?"上行":"下行", c->arrival_time);
    pthread_mutex_unlock(&mall_mutex);
}

Customer* dequeue(Queue* q) {
    pthread_mutex_lock(&mall_mutex);
    if(!q->head) {
        pthread_mutex_unlock(&mall_mutex);
        return NULL;
    }
    Customer* c = q->head;
    q->head = c->next;
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
// 判断能否上电梯(方向一致或IDLE；没满；入口空闲)
// --------------------------------------------------
int can_customer_board(Customer* c) {
    if(!c) return 0;
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    // 电梯是否满
    if(e->num_people >= MAX_ESCALATOR_CAPACITY) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 方向是否匹配
    if(e->direction != IDLE && e->direction != c->direction) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 入口是否空闲
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    if(e->steps[entry] != NULL) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 可以上
    pthread_mutex_unlock(&mall_mutex);
    return 1;
}

// 上电梯
void board_customer(Customer* c) {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    if(e->direction == IDLE) {
        e->direction = c->direction;
        printf("电梯方向设为: %s\n", (c->direction==UP)?"上行":"下行");
    }
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    e->steps[entry] = c;
    e->num_people++;
    int wait_time = mall->current_time - c->arrival_time;
    printf("顾客 %d 上电梯，方向: %s，等待时间: %d 秒\n",
           c->id, (c->direction==UP)?"上行":"下行", wait_time);
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 电梯每秒移动顾客，若空了则考虑是否切换方向
// --------------------------------------------------
void operate_escalator() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;

    if(e->num_people > 0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n",
               (e->direction==UP)?"上行":(e->direction==DOWN)?"下行":"空闲",
               e->num_people);

        // 上行
        if(e->direction == UP) {
            // 最后一格离开
            if(e->steps[MAX_ESCALATOR_CAPACITY-1]) {
                Customer* c = e->steps[MAX_ESCALATOR_CAPACITY-1];
                int turnaround = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(上行)，周转时间: %d 秒\n", c->id, turnaround);
                total_turnaround_time += turnaround;
                completed_customers++;
                free(c);
                e->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                e->num_people--;
                mall->total_customers--;
            }
            // 其它往上挪
            for(int i=MAX_ESCALATOR_CAPACITY-2; i>=0; i--){
                if(e->steps[i]) {
                    e->steps[i+1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }
        // 下行
        else if(e->direction == DOWN) {
            // 第一格离开
            if(e->steps[0]) {
                Customer* c = e->steps[0];
                int turnaround = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(下行)，周转时间: %d 秒\n", c->id, turnaround);
                total_turnaround_time += turnaround;
                completed_customers++;
                free(c);
                e->steps[0] = NULL;
                e->num_people--;
                mall->total_customers--;
            }
            // 其它往下挪
            for(int i=1; i<MAX_ESCALATOR_CAPACITY; i++){
                if(e->steps[i]) {
                    e->steps[i-1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }

        // 如果电梯空了 => 完成了一个批次
        if(e->num_people == 0) {
            batch_count++;  // 同方向完成一批
            printf("电梯已空，完成一批(%d)\n", batch_count);

            Queue* upQ   = mall->upQueue;
            Queue* downQ = mall->downQueue;
            int upLen   = upQ->length;
            int downLen = downQ->length;

            // 如果两边都没人 => IDLE
            if(upLen==0 && downLen==0) {
                e->direction = IDLE;
                printf("电梯空闲\n");
                batch_count=0;  // 重置
            }
            else {
                // 看看是否要切向对方
                if(e->direction == UP) {
                    // 若已达到批量限制,或(对向人很多)
                    if(batch_count >= BATCH_LIMIT && downLen>0) {
                        printf("已达BATCH_LIMIT=%d, 切换到下行\n", BATCH_LIMIT);
                        e->direction = DOWN;
                        batch_count=0;
                    }
                    else if(downLen - upLen >= DIFF_THRESHOLD) {
                        printf("下行队列比上行多 >= %d, 切换到下行\n", DIFF_THRESHOLD);
                        e->direction = DOWN;
                        batch_count=0;
                    }
                }
                else if(e->direction == DOWN) {
                    // 若已达批量上限,或(对向人很多)
                    if(batch_count >= BATCH_LIMIT && upLen>0) {
                        printf("已达BATCH_LIMIT=%d, 切换到上行\n", BATCH_LIMIT);
                        e->direction = UP;
                        batch_count=0;
                    }
                    else if(upLen - downLen >= DIFF_THRESHOLD) {
                        printf("上行队列比下行多 >= %d, 切换到上行\n", DIFF_THRESHOLD);
                        e->direction = UP;
                        batch_count=0;
                    }
                }
            }
        }
    }

    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 打印电梯状态
// --------------------------------------------------
void print_escalator_status() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    printf("电梯状态: [");
    for(int i=0; i<MAX_ESCALATOR_CAPACITY; i++){
        if(e->steps[i]) {
            printf("%d", e->steps[i]->id);
        } else {
            printf("0");
        }
        if(i<MAX_ESCALATOR_CAPACITY-1) printf(", ");
    }
    printf("] 方向: %s\n",
           (e->direction==UP)?"上行":
           (e->direction==DOWN)?"下行":"空闲");
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 主控制循环：每秒操作电梯、检查队列上电梯、生成新顾客
// --------------------------------------------------
void mall_control_loop(int simulation_time) {
    // 在本示例里, simulation_time=100 表示 100 秒后不再进顾客，等商场清空结束
    while(1) {
        pthread_mutex_lock(&mall_mutex);
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. 运行电梯(移动一步)
        operate_escalator();

        // 2. 电梯状态
        print_escalator_status();

        // 3. 让上行队首顾客(若有)上电梯
        pthread_mutex_lock(&mall_mutex);
        if(mall->upQueue->head) {
            Customer* c = mall->upQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)) {
                Customer* top = dequeue(mall->upQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("上行队首顾客 %d 不能上电梯\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // 4. 让下行队首顾客(若有)上电梯
        pthread_mutex_lock(&mall_mutex);
        if(mall->downQueue->head) {
            Customer* c = mall->downQueue->head;
            pthread_mutex_unlock(&mall_mutex);

            if(can_customer_board(c)) {
                Customer* top = dequeue(mall->downQueue);
                board_customer(top);
            } else {
                pthread_mutex_lock(&mall_mutex);
                printf("下行队首顾客 %d 不能上电梯\n", c->id);
                pthread_mutex_unlock(&mall_mutex);
            }
        } else {
            pthread_mutex_unlock(&mall_mutex);
        }

        // 5. 生成新顾客(若当前时间<100秒 & 未达容量)
        pthread_mutex_lock(&mall_mutex);
        if(mall->current_time < simulation_time) {
            int max_new = MAX_CUSTOMERS - mall->total_customers;
            if(max_new>0) {
                int new_cust = rand() % 3; // 0~2
                if(new_cust>0) {
                    printf("本秒生成 %d 个新顾客\n", new_cust);
                    for(int i=0; i<new_cust; i++){
                        int id = mall->total_customers + 1;
                        int dir= (rand()%2==0)? UP:DOWN;
                        Customer* nc = create_customer(id, dir);
                        mall->total_customers++;
                        if(dir==UP) enqueue(mall->upQueue, nc);
                        else         enqueue(mall->downQueue, nc);
                    }
                } else {
                    printf("本秒没有新顾客到达\n");
                }
            } else {
                printf("商场已满,无法接收新顾客\n");
            }
        } else {
            printf("时间已达(或超过) %d 秒，不再接收新顾客\n", simulation_time);
        }

        // 6. 打印当前商场状态
        printf("当前商场状态: 总人数=%d, 上行队列=%d, 下行队列=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // 7. 如果时间>=100秒 且商场没人 => 结束
        if(mall->current_time >= simulation_time && mall->total_customers==0) {
            pthread_mutex_unlock(&mall_mutex);
            break;
        }

        // 时间+1
        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);
        sleep(1);
    }

    // 结束后打印平均周转时间
    printf("\n========== 模拟结束 ==========\n");
    pthread_mutex_lock(&mall_mutex);
    printf("剩余顾客数: %d\n", mall->total_customers);
    if(completed_customers>0) {
        double avg_turn = (double)total_turnaround_time / completed_customers;
        printf("所有顾客的平均周转时间: %.2f 秒\n", avg_turn);
    } else {
        printf("没有完成乘梯的顾客，无法计算平均周转时间。\n");
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// cleanup资源
// --------------------------------------------------
void cleanup_resources() {
    pthread_mutex_lock(&mall_mutex);
    Customer* c;
    while((c=dequeue(mall->upQueue))!=NULL)   free(c);
    while((c=dequeue(mall->downQueue))!=NULL) free(c);

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

// --------------------------------------------------
// main函数
// --------------------------------------------------
int main(int argc, char* argv[]) {
    srand(time(NULL));

    // 初始化递归互斥锁
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    int num_customers=10; 
    int num_steps=13;     
    if(argc>1){
        num_customers = atoi(argv[1]);
        if(num_customers<0 || num_customers>MAX_CUSTOMERS){
            printf("初始顾客数量必须在0~%d之间\n", MAX_CUSTOMERS);
            return EXIT_FAILURE;
        }
    }
    if(argc>2){
        num_steps = atoi(argv[2]);
        if(num_steps<=0 || num_steps>MAX_ESCALATOR_CAPACITY){
            printf("楼梯数量必须在1~%d之间\n", MAX_ESCALATOR_CAPACITY);
            return EXIT_FAILURE;
        }
    }

    printf("开始模拟，初始顾客=%d, 楼梯台阶=%d\n", num_customers, num_steps);

    mall = init_mall();

    // 创建初始顾客
    for(int i=0; i<num_customers; i++){
        int dir=(rand()%2==0)?UP:DOWN;
        Customer* c = create_customer(i+1, dir);
        mall->total_customers++;
        if(dir==UP)   enqueue(mall->upQueue,c);
        else          enqueue(mall->downQueue,c);
    }

    // 至少模拟100秒，之后等顾客清空
    mall_control_loop(100);

    cleanup_resources();
    pthread_mutex_destroy(&mall_mutex);
    return EXIT_SUCCESS;
}
