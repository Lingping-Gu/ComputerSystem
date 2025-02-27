#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS           30
#define MAX_ESCALATOR_CAPACITY  13

// 新增两个关键参数：批量上限 + 队列人数差额阈值
#define BATCH_LIMIT    2  // 同一方向连续运送2批后，考虑切换
#define DIFF_THRESHOLD 8  // 对向队列比当前多 >= 8 时，也切换

// 方向
#define UP    1
#define DOWN -1
#define IDLE  0

// -------------------- 全局互斥锁 --------------------
static pthread_mutex_t mall_mutex; // 在 main() 中初始化(使用递归锁)

// -------------------- 结构体定义 --------------------
typedef struct Customer {
    int id;             // 全局递增ID
    int arrival_time;   // 到达商场时的模拟时间戳
    int direction;      // UP / DOWN
    int position;       // 0或14 (可以保留不用)
    struct Customer* next;
    struct Customer* prev;
} Customer;

typedef struct {
    Customer* head;
    Customer* tail;
    int length;
    int direction;  // UP / DOWN
} Queue;

typedef struct {
    Customer* steps[MAX_ESCALATOR_CAPACITY];
    int direction;  // UP / DOWN / IDLE
    int num_people;
} Escalator;

typedef struct {
    Queue* upQueue;
    Queue* downQueue;
    Escalator* escalator;
    int total_customers; // 当前商场内(排队 + 电梯上)顾客数
    int current_time;    // 模拟当前时间
} Mall;

// ============ 用于统计 ============
int total_turnaround_time = 0;
int completed_customers   = 0;
// ============ 批量计数 ============
int batch_count = 0;   // 当前方向已连续完成多少批

// ============ 全局ID自增 ============
static int global_customer_id = 0; 

Mall* mall = NULL;

// 函数声明
Queue*      init_queue(int dir);
Escalator*  init_escalator();
Mall*       init_mall();
Customer*   create_customer(int direction);

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

// --------------------------------------------------
// 创建顾客(全局ID自增)
// --------------------------------------------------
Customer* create_customer(int direction) {
    pthread_mutex_lock(&mall_mutex);
    global_customer_id++;  // 无论能否进商场, ID都自增
    Customer* c = (Customer*)malloc(sizeof(Customer));
    if(!c) {
        perror("Failed to allocate memory for customer");
        exit(EXIT_FAILURE);
    }
    c->id           = global_customer_id;
    c->arrival_time = mall->current_time;
    c->direction    = direction;
    c->position     = (direction==UP)? 0 : 14;
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
// 判断顾客能否上电梯
// --------------------------------------------------
int can_customer_board(Customer* c) {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    // 是否满
    if(e->num_people >= MAX_ESCALATOR_CAPACITY) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 方向匹配 or IDLE
    if(e->direction!=IDLE && e->direction!=c->direction) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 入口是否空
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    if(e->steps[entry] != NULL) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    pthread_mutex_unlock(&mall_mutex);
    return 1;
}

// --------------------------------------------------
// 上电梯
// --------------------------------------------------
void board_customer(Customer* c) {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    if(e->direction==IDLE) {
        e->direction = c->direction;
        printf("电梯方向设为: %s\n", (c->direction==UP)?"上行":"下行");
    }
    int entry = (c->direction==UP)? 0 : (MAX_ESCALATOR_CAPACITY-1);
    e->steps[entry] = c;
    e->num_people++;
    int wait_time = mall->current_time - c->arrival_time;
    printf("顾客 %d 上电梯，方向: %s，等待时间: %d秒\n",
           c->id, (c->direction==UP)?"上行":"下行", wait_time);
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// 电梯每秒移动顾客，如果空了 => 完成一批 => 看是否切换方向
// --------------------------------------------------
void operate_escalator() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = mall->escalator;
    if(e->num_people>0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n",
               (e->direction==UP)?"上行":(e->direction==DOWN)?"下行":"空闲",
               e->num_people);

        // 上行
        if(e->direction==UP) {
            // 最后一格乘客离开
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
            // 其它顾客往上移动
            for(int i=MAX_ESCALATOR_CAPACITY-2; i>=0; i--){
                if(e->steps[i]) {
                    e->steps[i+1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }
        // 下行
        else if(e->direction==DOWN) {
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
            // 其它顾客往下移动
            for(int i=1; i<MAX_ESCALATOR_CAPACITY; i++){
                if(e->steps[i]) {
                    e->steps[i-1] = e->steps[i];
                    e->steps[i]   = NULL;
                }
            }
        }

        // 如果电梯空了 => 这一批结束
        if(e->num_people==0) {
            batch_count++;
            printf("电梯已空，完成一批(batch_count=%d)\n", batch_count);

            Queue* upQ   = mall->upQueue;
            Queue* downQ = mall->downQueue;
            int upLen   = upQ->length;
            int downLen = downQ->length;

            if(upLen==0 && downLen==0) {
                // 两边都空 => idle
                e->direction = IDLE;
                batch_count  = 0;
                printf("电梯空闲\n");
            }
            else {
                // 当前方向
                int curDir = e->direction;
                // 如果当前方向是UP
                if(curDir==UP) {
                    // 若当前上行队列空、而下行队列还有人 => 切
                    if(upLen==0 && downLen>0) {
                        printf("当前上行队列空, 下行有%d人 => 切到下行\n", downLen);
                        e->direction = DOWN;
                        batch_count=0;
                    }
                    // 否则若已达批量限制 & 下行有人 => 切
                    else if(batch_count>=BATCH_LIMIT && downLen>0) {
                        printf("已达BATCH_LIMIT=%d, 切到下行\n", BATCH_LIMIT);
                        e->direction = DOWN;
                        batch_count=0;
                    }
                    // 否则若 (下行 - 上行 >= DIFF_THRESHOLD) => 切
                    else if(downLen - upLen >= DIFF_THRESHOLD) {
                        printf("下行比上行多 >=%d, 切到下行\n", DIFF_THRESHOLD);
                        e->direction = DOWN;
                        batch_count=0;
                    }
                    // 否则保留UP继续 (不切)
                }
                else if(curDir==DOWN) {
                    // 若当前下行队列空、而上行队列有人 => 切
                    if(downLen==0 && upLen>0) {
                        printf("当前下行队列空, 上行有%d人 => 切到上行\n", upLen);
                        e->direction = UP;
                        batch_count=0;
                    }
                    // 若达批量限制 & upLen>0 => 切
                    else if(batch_count>=BATCH_LIMIT && upLen>0) {
                        printf("已达BATCH_LIMIT=%d, 切到上行\n", BATCH_LIMIT);
                        e->direction = UP;
                        batch_count=0;
                    }
                    // 若(upLen - downLen >= DIFF_THRESHOLD) => 切
                    else if(upLen - downLen >= DIFF_THRESHOLD) {
                        printf("上行比下行多 >=%d, 切到上行\n", DIFF_THRESHOLD);
                        e->direction = UP;
                        batch_count=0;
                    }
                    // 否则保留DOWN
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
// 主控制循环
// --------------------------------------------------
void mall_control_loop(int simulation_time) {
    // simulation_time=100 => 100秒后停止新顾客, 等清空后结束
    while(1) {
        pthread_mutex_lock(&mall_mutex);
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);
        pthread_mutex_unlock(&mall_mutex);

        // 1. 运行电梯
        operate_escalator();

        // 2. 打印电梯状态
        print_escalator_status();

        // 3. 检查上行队首
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

        // 4. 检查下行队首
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

        // 5. 尝试生成新顾客(若未到100秒)
        pthread_mutex_lock(&mall_mutex);
        if(mall->current_time < simulation_time) {
            // 随机生成0~2个新顾客
            int new_cust = rand() % 3; 
            if(new_cust>0) {
                printf("本秒尝试生成 %d 个顾客\n", new_cust);
                for(int i=0; i<new_cust; i++){
                    // 无论能不能进商场, global_customer_id都会自增
                    // 先创建
                    int dir = (rand()%2==0)? UP:DOWN;
                    Customer* nc = create_customer(dir);

                    // 如果商场满,直接拒绝
                    if(mall->total_customers >= MAX_CUSTOMERS) {
                        printf("商场已满，拒绝顾客 %d\n", nc->id);
                        free(nc);
                    }
                    else {
                        // 可以进商场
                        mall->total_customers++;
                        if(dir==UP) enqueue(mall->upQueue, nc);
                        else         enqueue(mall->downQueue, nc);
                    }
                }
            } else {
                printf("本秒没有新顾客到达\n");
            }
        } else {
            printf("时间已达(或超过) %d 秒，不再接收新顾客\n", simulation_time);
        }

        // 6. 打印当前商场状态
        printf("当前商场: 总人数=%d, 上行=%d, 下行=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // 7. 若时间>=100 且 没顾客 => 结束
        if(mall->current_time >= simulation_time && mall->total_customers==0){
            pthread_mutex_unlock(&mall_mutex);
            break;
        }

        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);
        sleep(1);
    }

    // 结束后输出平均周转时间
    printf("\n========== 模拟结束 ==========\n");
    pthread_mutex_lock(&mall_mutex);
    printf("剩余顾客数: %d\n", mall->total_customers);
    if(completed_customers>0) {
        double avg_t = (double)total_turnaround_time / completed_customers;
        printf("所有顾客平均周转时间: %.2f 秒\n", avg_t);
    } else {
        printf("没有完成乘梯的顾客，无法计算平均周转时间\n");
    }
    pthread_mutex_unlock(&mall_mutex);
}

// --------------------------------------------------
// cleanup
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
// main
// --------------------------------------------------
int main(int argc, char* argv[]) {
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
            printf("初始顾客数需在0~%d之间\n", MAX_CUSTOMERS);
            return EXIT_FAILURE;
        }
    }
    if(argc>2){
        num_steps = atoi(argv[2]);
        if(num_steps<=0 || num_steps>MAX_ESCALATOR_CAPACITY){
            printf("楼梯数需在1~%d之间\n", MAX_ESCALATOR_CAPACITY);
            return EXIT_FAILURE;
        }
    }

    printf("开始模拟: 初始顾客=%d, 楼梯台阶=%d\n", num_customers, num_steps);

    // 初始化商场
    mall = init_mall();

    // 创建初始顾客
    for(int i=0; i<num_customers; i++){
        int dir = (rand()%2==0)?UP:DOWN;
        Customer* c = create_customer(dir);
        mall->total_customers++;
        if(dir==UP) enqueue(mall->upQueue, c);
        else        enqueue(mall->downQueue, c);
    }

    // 至少100秒，然后等人清空
    mall_control_loop(100);

    cleanup_resources();
    pthread_mutex_destroy(&mall_mutex);
    return EXIT_SUCCESS;
}
