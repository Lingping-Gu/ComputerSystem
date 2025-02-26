#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30  // 商场最多容纳人数
#define MAX_ESCALATOR_CAPACITY 13  // 扶梯最多容纳人数
#define MAX_WAIT_TIME 41  // 顾客最大等待时间
#define SWITCH_THRESHOLD (MAX_WAIT_TIME - MAX_ESCALATOR_CAPACITY) // 28秒，决定换方向
#define UP 1    // 上行方向
#define DOWN -1 // 下行方向
#define IDLE 0  // 空闲状态

// -------------------- 全局互斥锁 --------------------
static pthread_mutex_t mall_mutex; // 在 main() 中初始化(使用递归锁)

// 顾客结构体
typedef struct Customer {
    int id;            // 顾客ID
    int arrival_time;  // 进入商场的时间戳（单位：秒）
    int direction;     // UP: 上行, DOWN: 下行
    int position;      // 当前所在位置，如果是0则是在楼下排队，如果1-13是在楼梯上，如果14则是在楼上排队
    struct Customer* next; 
    struct Customer* prev;
} Customer;

// 队列结构体
typedef struct {
    Customer* head;     // 队首顾客
    Customer* tail;     // 队尾顾客
    int length;         // 队列长度
    int direction;      // UP: 上行, DOWN: 下行
} Queue;

// 扶梯结构体
typedef struct {
    Customer* steps[MAX_ESCALATOR_CAPACITY];  // 扶梯上的顾客，每个阶梯对应一个顾客
    int direction;      // UP: 上行, DOWN: 下行, IDLE: 空闲
    int num_people;     // 当前扶梯上有多少人
} Escalator;

// 商场结构体
typedef struct {
    Queue* upQueue;         // 上行队列
    Queue* downQueue;       // 下行队列
    Escalator* escalator;   // 扶梯
    int total_customers;    // 商场内的总人数
    int current_time;       // 系统当前时间(模拟秒数)
} Mall;

// ============ 用于平均周转时间统计的全局变量 ============ //
int total_turnaround_time = 0;   // 所有已完成顾客的总周转时间
int completed_customers = 0;     // 已完成乘梯的顾客数量
// ====================================================== //

Mall* mall = NULL;

// 函数声明
Queue* init_queue(int dir);
Escalator* init_escalator();
Mall* init_mall();
Customer* create_customer(int id, int direction);
void enqueue(Queue* q, Customer* c);
Customer* dequeue(Queue* q);
int is_escalator_empty();
int get_waiting_time(Customer* customer);
int should_wait_for_opposite(Queue* opposite_queue);
int can_customer_board(Customer* customer);
void board_customer(Customer* customer);
void print_escalator_status();
void operate_escalator();
void mall_control_loop(int simulation_time);
void cleanup_resources();

// ---------------- 初始化函数和队列/电梯操作 ----------------

Queue* init_queue(int dir) {
    pthread_mutex_lock(&mall_mutex);
    Queue* q = (Queue*)malloc(sizeof(Queue));
    if (!q) {
        perror("Failed to allocate memory for queue");
        exit(EXIT_FAILURE);
    }
    q->head = NULL;
    q->tail = NULL;
    q->length = 0;
    q->direction = dir;
    pthread_mutex_unlock(&mall_mutex);
    return q;
}

Escalator* init_escalator() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* e = (Escalator*)malloc(sizeof(Escalator));
    if (!e) {
        perror("Failed to allocate memory for escalator");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < MAX_ESCALATOR_CAPACITY; i++) {
        e->steps[i] = NULL;
    }
    e->direction = IDLE;  // 初始为空闲
    e->num_people = 0;
    pthread_mutex_unlock(&mall_mutex);
    return e;
}

Mall* init_mall() {
    pthread_mutex_lock(&mall_mutex);
    Mall* m = (Mall*)malloc(sizeof(Mall));
    if (!m) {
        perror("Failed to allocate memory for mall");
        exit(EXIT_FAILURE);
    }
    m->upQueue = init_queue(UP);
    m->downQueue = init_queue(DOWN);
    m->escalator = init_escalator();
    m->total_customers = 0;
    m->current_time = 0;
    pthread_mutex_unlock(&mall_mutex);
    return m;
}

// 创建新顾客
Customer* create_customer(int id, int direction) {
    pthread_mutex_lock(&mall_mutex);
    Customer* c = (Customer*)malloc(sizeof(Customer));
    if (!c) {
        perror("Failed to allocate memory for customer");
        exit(EXIT_FAILURE);
    }
    c->id = id;
    c->arrival_time = mall->current_time;
    c->direction = direction;
    c->position = (direction == UP) ? 0 : 14;  // 上行从0楼开始，下行从14楼开始
    c->next = NULL;
    c->prev = NULL;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// 队列入队
void enqueue(Queue* q, Customer* c) {
    pthread_mutex_lock(&mall_mutex);
    if (q->head == NULL) {
        q->head = c;
        q->tail = c;
    } else {
        q->tail->next = c;
        c->prev = q->tail;
        q->tail = c;
    }
    q->length++;

    printf("顾客 %d 加入队列，方向: %s，到达时间: %d\n", 
           c->id, (c->direction == UP) ? "上行" : "下行", c->arrival_time);

    pthread_mutex_unlock(&mall_mutex);
}

// 队列出队
Customer* dequeue(Queue* q) {
    pthread_mutex_lock(&mall_mutex);
    if (q->head == NULL) {
        pthread_mutex_unlock(&mall_mutex);
        return NULL;
    }
    Customer* c = q->head;
    q->head = c->next;
    if (q->head == NULL) {
        q->tail = NULL;
    } else {
        q->head->prev = NULL;
    }
    q->length--;
    pthread_mutex_unlock(&mall_mutex);
    return c;
}

// 判断电梯是否为空
int is_escalator_empty() {
    pthread_mutex_lock(&mall_mutex);
    int empty = (mall->escalator->num_people == 0);
    pthread_mutex_unlock(&mall_mutex);
    return empty;
}

// 获取顾客等待时间
int get_waiting_time(Customer* customer) {
    if (customer == NULL) return 0;
    pthread_mutex_lock(&mall_mutex);
    int wt = mall->current_time - customer->arrival_time;
    pthread_mutex_unlock(&mall_mutex);
    return wt;
}

// 判断是否要让对向顾客先行
int should_wait_for_opposite(Queue* opposite_queue) {
    pthread_mutex_lock(&mall_mutex);
    if (opposite_queue->head == NULL) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    int wait_time = mall->current_time - opposite_queue->head->arrival_time;
    int result = (wait_time >= SWITCH_THRESHOLD);
    pthread_mutex_unlock(&mall_mutex);
    return result;
}

// 顾客是否能进电梯
int can_customer_board(Customer* customer) {
    pthread_mutex_lock(&mall_mutex);
    if (!customer) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    Escalator* escalator = mall->escalator;
    // 电梯已满
    if (escalator->num_people >= MAX_ESCALATOR_CAPACITY) {
        pthread_mutex_unlock(&mall_mutex);
        return 0;
    }
    // 电梯空闲或同向
    if (escalator->direction == IDLE || escalator->direction == customer->direction) {
        Queue* opposite_queue = (customer->direction == UP) ? mall->downQueue : mall->upQueue;
        if (should_wait_for_opposite(opposite_queue)) {
            printf("顾客 %d 等待，因为逆向队首等待时间过长\n", customer->id);
            pthread_mutex_unlock(&mall_mutex);
            return 0;
        }
        pthread_mutex_unlock(&mall_mutex);
        return 1;
    }
    pthread_mutex_unlock(&mall_mutex);
    return 0;
}

// 顾客进入电梯
void board_customer(Customer* customer) {
    pthread_mutex_lock(&mall_mutex);
    Escalator* escalator = mall->escalator;
    // 如果电梯空闲，则由该顾客设定方向
    if (escalator->direction == IDLE) {
        escalator->direction = customer->direction;
        printf("电梯方向设为: %s\n", (customer->direction == UP) ? "上行" : "下行");
    }
    // 入口位置
    int entry_position = (customer->direction == UP) ? 0 : (MAX_ESCALATOR_CAPACITY - 1);
    if (escalator->steps[entry_position] == NULL) {
        escalator->steps[entry_position] = customer;
        escalator->num_people++;
        printf("顾客 %d 上电梯，方向: %s，等待时间: %d秒\n",
               customer->id,
               (customer->direction == UP) ? "上行" : "下行",
               mall->current_time - customer->arrival_time);
    } else {
        printf("顾客 %d 无法上电梯，入口已被占用\n", customer->id);
        // 重新入队
        if (customer->direction == UP) {
            enqueue(mall->upQueue, customer);
        } else {
            enqueue(mall->downQueue, customer);
        }
    }
    pthread_mutex_unlock(&mall_mutex);
}

// 打印电梯状态
void print_escalator_status() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* escalator = mall->escalator;
    printf("电梯状态: [");
    for (int i = 0; i < MAX_ESCALATOR_CAPACITY; i++) {
        if (escalator->steps[i] != NULL) {
            printf("%d", escalator->steps[i]->id);
        } else {
            printf("0");
        }
        if (i < MAX_ESCALATOR_CAPACITY - 1) printf(", ");
    }
    printf("] 方向: %s\n",
           (escalator->direction == UP) ? "上行" :
           (escalator->direction == DOWN) ? "下行" : "空闲");
    pthread_mutex_unlock(&mall_mutex);
}

// 电梯运行
void operate_escalator() {
    pthread_mutex_lock(&mall_mutex);
    Escalator* escalator = mall->escalator;
    if (escalator->num_people > 0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n",
               (escalator->direction == UP) ? "上行" : "下行",
               escalator->num_people);
        // 移动乘客
        if (escalator->direction == UP) {
            // 最后一格乘客离开
            if (escalator->steps[MAX_ESCALATOR_CAPACITY - 1] != NULL) {
                Customer* c = escalator->steps[MAX_ESCALATOR_CAPACITY - 1];
                int turnaround_time = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(上行)，周转时间: %d 秒\n", c->id, turnaround_time);
                total_turnaround_time += turnaround_time;
                completed_customers++;
                free(c);
                escalator->steps[MAX_ESCALATOR_CAPACITY - 1] = NULL;
                escalator->num_people--;
                mall->total_customers--;
            }
            // 其余往上挪
            for (int i = MAX_ESCALATOR_CAPACITY - 2; i >= 0; i--) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i + 1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        } else if (escalator->direction == DOWN) {
            // 第一格乘客离开
            if (escalator->steps[0] != NULL) {
                Customer* c = escalator->steps[0];
                int turnaround_time = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(下行)，周转时间: %d 秒\n", c->id, turnaround_time);
                total_turnaround_time += turnaround_time;
                completed_customers++;
                free(c);
                escalator->steps[0] = NULL;
                escalator->num_people--;
                mall->total_customers--;
            }
            // 其余往下挪
            for (int i = 1; i < MAX_ESCALATOR_CAPACITY; i++) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i - 1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        }
        // 如果电梯空了
        if (escalator->num_people == 0) {
            Queue* current_queue = (escalator->direction == UP) ? mall->upQueue : mall->downQueue;
            Queue* opposite_queue = (escalator->direction == UP) ? mall->downQueue : mall->upQueue;
            // 若当前方向无人等，并且另一方向有人，或对向等待过久
            if ((current_queue->head == NULL && opposite_queue->head != NULL) ||
                should_wait_for_opposite(opposite_queue)) {
                printf("电梯方向切换: %s -> %s\n",
                       (escalator->direction == UP) ? "上行" : "下行",
                       (escalator->direction == UP) ? "下行" : "上行");
                escalator->direction = -escalator->direction;
            }
            else if (current_queue->head == NULL && opposite_queue->head == NULL) {
                printf("电梯空闲\n");
                escalator->direction = IDLE;
            }
        }
    }
    pthread_mutex_unlock(&mall_mutex);
}

// ------------------- 主控制循环 -------------------
void mall_control_loop(int simulation_time) {
    // 在本示例里，simulation_time=100 表示：前100秒可以进新顾客，100秒后就拒绝新顾客
    // 一旦商场顾客清空(total_customers=0)，就结束模拟

    while (1) {
        pthread_mutex_lock(&mall_mutex);
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);

        // 1. 打印电梯状态
        print_escalator_status();

        // 2. 运行电梯
        operate_escalator();

        // 3. 再看电梯状态
        print_escalator_status();

        // 4. 检查上行队列能否上电梯
        if (mall->upQueue->head != NULL) {
            Customer* c = mall->upQueue->head;
            int wait_time = mall->current_time - c->arrival_time;
            printf("上行队首顾客 %d，等待时间: %d 秒\n", c->id, wait_time);
            if (can_customer_board(c)) {
                c = dequeue(mall->upQueue);
                board_customer(c);
            } else {
                printf("上行队首顾客 %d 不能上电梯\n", c->id);
            }
        }

        // 5. 检查下行队列能否上电梯
        if (mall->downQueue->head != NULL) {
            Customer* c = mall->downQueue->head;
            int wait_time = mall->current_time - c->arrival_time;
            printf("下行队首顾客 %d，等待时间: %d 秒\n", c->id, wait_time);
            if (can_customer_board(c)) {
                c = dequeue(mall->downQueue);
                board_customer(c);
            } else {
                printf("下行队首顾客 %d 不能上电梯\n", c->id);
            }
        }

        // 6. 可能生成新顾客(若时间还未到100秒 && 未达商场最大容量)
        if (mall->current_time < simulation_time) {
            // 未到100秒，可以生成新顾客
            int max_new_customers = MAX_CUSTOMERS - mall->total_customers;
            if (max_new_customers > 0) {
                // 可根据需要改为更简单的：比如每秒生成 0~2 人
                int new_customers = rand() % 3; 
                if (new_customers > 0) {
                    printf("本秒生成 %d 个新顾客\n", new_customers);
                    for (int i = 0; i < new_customers; i++) {
                        int id = mall->total_customers + 1;
                        int direction = (rand() % 2 == 0) ? UP : DOWN;
                        Customer* c = create_customer(id, direction);
                        mall->total_customers++;
                        if (direction == UP) {
                            enqueue(mall->upQueue, c);
                        } else {
                            enqueue(mall->downQueue, c);
                        }
                    }
                } else {
                    printf("本秒没有新顾客到达\n");
                }
            } else {
                printf("商场已满，无法接收新顾客\n");
            }
        } else {
            // 已经 >= 100秒，不再生成新顾客
            printf("时间已达或超过 %d 秒，不再允许新顾客进入\n", simulation_time);
        }

        // 打印当前商场状态
        printf("当前商场状态: 总人数=%d, 上行队列=%d, 下行队列=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        // 判断：若已经过了100秒且商场中没有顾客，则退出
        if (mall->current_time >= simulation_time && mall->total_customers == 0) {
            // 解锁后再break
            pthread_mutex_unlock(&mall_mutex);
            break;
        }

        // 时间前进 + 解锁
        mall->current_time++;
        pthread_mutex_unlock(&mall_mutex);

        // 每秒模拟
        sleep(1);
    }

    // 退出循环后，打印平均周转时间
    printf("\n========== 模拟结束 ==========\n");
    printf("剩余顾客数: %d\n", mall->total_customers);
    if (completed_customers > 0) {
        double avg_turnaround = (double)total_turnaround_time / completed_customers;
        printf("所有顾客的平均周转时间: %.2f 秒\n", avg_turnaround);
    } else {
        printf("没有完成乘梯的顾客，无法计算平均周转时间。\n");
    }
}

// 资源清理函数
void cleanup_resources() {
    pthread_mutex_lock(&mall_mutex);
    // 释放队列中剩余的顾客
    Customer* c;
    while ((c = dequeue(mall->upQueue)) != NULL) {
        free(c);
    }
    while ((c = dequeue(mall->downQueue)) != NULL) {
        free(c);
    }
    // 释放扶梯上的顾客
    for (int i = 0; i < MAX_ESCALATOR_CAPACITY; i++) {
        if (mall->escalator->steps[i] != NULL) {
            free(mall->escalator->steps[i]);
        }
    }
    // 释放队列、扶梯和商场
    free(mall->upQueue);
    free(mall->downQueue);
    free(mall->escalator);
    free(mall);
    pthread_mutex_unlock(&mall_mutex);
}

// ------------------- main函数 -------------------
int main(int argc, char* argv[]) {
    srand(time(NULL));  // 初始化随机数

    // 初始化“递归互斥锁”
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mall_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    // 可通过命令行指定初始顾客数和楼梯数量
    int num_customers = 10;  // 默认初始顾客数
    int num_steps = 13;      // 默认楼梯数量
    
    if (argc > 1) {
        num_customers = atoi(argv[1]);
        if (num_customers < 0 || num_customers > MAX_CUSTOMERS) {
            printf("初始顾客数量必须在0到%d之间\n", MAX_CUSTOMERS);
            return EXIT_FAILURE;
        }
    }
    
    if (argc > 2) {
        num_steps = atoi(argv[2]);
        if (num_steps <= 0 || num_steps > MAX_ESCALATOR_CAPACITY) {
            printf("楼梯数量必须在1到%d之间\n", MAX_ESCALATOR_CAPACITY);
            return EXIT_FAILURE;
        }
    }
    
    printf("开始模拟，初始顾客数量: %d，楼梯数量: %d\n", num_customers, num_steps);
    
    // 初始化商场
    mall = init_mall();
    
    // 创建初始顾客
    for (int i = 0; i < num_customers; i++) {
        int direction = (rand() % 2 == 0) ? UP : DOWN;
        Customer* c = create_customer(i + 1, direction);
        mall->total_customers++;
        
        if (direction == UP) {
            enqueue(mall->upQueue, c);
        } else {
            enqueue(mall->downQueue, c);
        }
    }
    
    // 运行商场控制循环：simulation_time=100表示100秒后不再进人
    mall_control_loop(100);
    
    // 清理资源
    cleanup_resources();
    
    // 销毁互斥锁
    pthread_mutex_destroy(&mall_mutex);
    return EXIT_SUCCESS;
}
