#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30  // 商场最多容纳人数
#define MAX_ESCALATOR_CAPACITY 13  // 扶梯最多容纳人数
#define MAX_WAIT_TIME 41  // 顾客最大等待时间
#define SWITCH_THRESHOLD (MAX_WAIT_TIME - MAX_ESCALATOR_CAPACITY) // 28秒，决定换方向
#define UP 1  // 上行方向
#define DOWN -1  // 下行方向
#define IDLE 0  // 空闲状态

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
    int current_time;       // 系统当前时间
} Mall;

// ========================== 新增的全局变量 ========================== //
// 用于计算所有顾客总的周转时间以及完成乘梯的顾客数量
int total_turnaround_time = 0;
int completed_customers = 0;
// ================================================================== //

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

// 初始化队列
Queue* init_queue(int dir) {
    Queue* q = (Queue*)malloc(sizeof(Queue));
    if (!q) {
        perror("Failed to allocate memory for queue");
        exit(EXIT_FAILURE);
    }
    q->head = NULL;
    q->tail = NULL;
    q->length = 0;
    q->direction = dir;
    return q;
}

// 初始化扶梯
Escalator* init_escalator() {
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
    return e;
}

// 初始化商场
Mall* init_mall() {
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
    return m;
}

// 创建新顾客
Customer* create_customer(int id, int direction) {
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
    return c;
}

// 将顾客添加到队列
void enqueue(Queue* q, Customer* c) {
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
}

// 从队列移除顾客
Customer* dequeue(Queue* q) {
    if (q->head == NULL) {
        return NULL;
    }
    
    Customer* c = q->head;
    q->head = c->next;
    
    if (q->head == NULL) {
        q->tail = NULL;
    } else {
        q->head->prev = NULL;
    }
    
    c->next = NULL;
    c->prev = NULL;
    q->length--;
    
    return c;
}

// 检查扶梯是否为空
int is_escalator_empty() {
    return mall->escalator->num_people == 0;
}

// 获取顾客等待时间
int get_waiting_time(Customer* customer) {
    if (customer == NULL) return 0;
    return mall->current_time - customer->arrival_time;
}

// 检查是否应该等待逆向队列顾客（根据等待时间）
int should_wait_for_opposite(Queue* opposite_queue) {
    if (opposite_queue->head == NULL) {
        return 0;  // 没有逆向顾客等待
    }
    
    int wait_time = get_waiting_time(opposite_queue->head);
    return wait_time >= SWITCH_THRESHOLD;
}

// 判断顾客是否可以上电梯
int can_customer_board(Customer* customer) {
    if (customer == NULL) return 0;
    
    Escalator* escalator = mall->escalator;
    
    // 电梯已满
    if (escalator->num_people >= MAX_ESCALATOR_CAPACITY) {
        return 0;
    }
    
    // 电梯空闲或方向相同，可以上电梯
    if (escalator->direction == IDLE || escalator->direction == customer->direction) {
        // 检查是否应该等待逆向顾客
        Queue* opposite_queue = (customer->direction == UP) ? mall->downQueue : mall->upQueue;
        if (should_wait_for_opposite(opposite_queue)) {
            printf("顾客 %d 等待，因为逆向队首等待时间过长\n", customer->id);
            return 0;
        }
        return 1;
    }
    
    return 0;
}

// 将顾客放到扶梯上
void board_customer(Customer* customer) {
    Escalator* escalator = mall->escalator;
    
    // 如果电梯空闲，设置方向
    if (escalator->direction == IDLE) {
        escalator->direction = customer->direction;
        printf("电梯方向设为: %s\n", (customer->direction == UP) ? "上行" : "下行");
    }
    
    // 根据方向确定入口位置
    int entry_position;
    if (customer->direction == UP) {
        entry_position = 0;  // 上行电梯入口在底部
    } else {
        entry_position = MAX_ESCALATOR_CAPACITY - 1;  // 下行电梯入口在顶部
    }
    
    // 检查入口是否有空位
    if (escalator->steps[entry_position] == NULL) {
        escalator->steps[entry_position] = customer;
        escalator->num_people++;
        
        printf("顾客 %d 上电梯，方向: %s，等待时间: %d秒\n", 
               customer->id, 
               (customer->direction == UP) ? "上行" : "下行",
               get_waiting_time(customer));
    } else {
        printf("顾客 %d 无法上电梯，入口已被占用\n", customer->id);
        // 将顾客重新放回队列
        if (customer->direction == UP) {
            enqueue(mall->upQueue, customer);
        } else {
            enqueue(mall->downQueue, customer);
        }
    }
}

void print_escalator_status() {
    Escalator* escalator = mall->escalator;
    
    printf("电梯状态: [");
    for (int i = 0; i < MAX_ESCALATOR_CAPACITY; i++) {
        if (escalator->steps[i] != NULL) {
            printf("%d", escalator->steps[i]->id);
        } else {
            printf("0");
        }
        if (i < MAX_ESCALATOR_CAPACITY-1) printf(", ");
    }
    printf("] 方向: %s\n", 
           (escalator->direction == UP) ? "上行" : 
           (escalator->direction == DOWN) ? "下行" : "空闲");
}

// 电梯运行函数
void operate_escalator() {
    Escalator* escalator = mall->escalator;
    
    // 如果电梯上有人
    if (escalator->num_people > 0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n", 
               (escalator->direction == UP) ? "上行" : "下行", 
               escalator->num_people);
        
        // 移动电梯上的顾客（向终点移动一步）
        if (escalator->direction == UP) {
            // 上行电梯，最后一个台阶的顾客到达终点
            if (escalator->steps[MAX_ESCALATOR_CAPACITY-1] != NULL) {
                Customer* c = escalator->steps[MAX_ESCALATOR_CAPACITY-1];
                int turnaround_time = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(上行)，周转时间: %d 秒\n", c->id, turnaround_time);

                // ========================== 新增：统计周转时间 ==========================
                total_turnaround_time += turnaround_time;
                completed_customers++;
                // ====================================================================

                free(c);
                escalator->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                escalator->num_people--;
                mall->total_customers--;
            }
            
            // 其它顾客向上移动一步
            for (int i = MAX_ESCALATOR_CAPACITY-2; i >= 0; i--) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i+1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        } else if (escalator->direction == DOWN) {
            // 下行电梯，第一个台阶的顾客到达终点
            if (escalator->steps[0] != NULL) {
                Customer* c = escalator->steps[0];
                int turnaround_time = mall->current_time - c->arrival_time;
                printf("顾客 %d 完成乘梯(下行)，周转时间: %d 秒\n", c->id, turnaround_time);

                // ========================== 新增：统计周转时间 ==========================
                total_turnaround_time += turnaround_time;
                completed_customers++;
                // ====================================================================

                free(c);
                escalator->steps[0] = NULL;
                escalator->num_people--;
                mall->total_customers--;
            }
            
            // 其它顾客向下移动一步
            for (int i = 1; i < MAX_ESCALATOR_CAPACITY; i++) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i-1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        }
        
        // 如果电梯空了，检查是否需要改变方向
        if (escalator->num_people == 0) {
            Queue* current_queue = (escalator->direction == UP) ? mall->upQueue : mall->downQueue;
            Queue* opposite_queue = (escalator->direction == UP) ? mall->downQueue : mall->upQueue;
            
            // 如果没有同方向等待且有反方向等待，或者反方向等待时间过长
            if ((current_queue->head == NULL && opposite_queue->head != NULL) || 
                should_wait_for_opposite(opposite_queue)) {
                printf("电梯方向切换: %s -> %s\n", 
                       (escalator->direction == UP) ? "上行" : "下行",
                       (escalator->direction == UP) ? "下行" : "上行");
                
                escalator->direction = -escalator->direction;  // 切换方向
            } else if (current_queue->head == NULL && opposite_queue->head == NULL) {
                printf("电梯空闲\n");
                escalator->direction = IDLE;  // 无人等待，设为空闲
            }
        }
    }
}

// 主控制循环
void mall_control_loop(int simulation_time) {
    // 只修改以下逻辑：100秒后不允许新创建顾客，但要等所有顾客送走再结束
    int generation_end_time = mall->current_time + simulation_time; // 比如 100 秒
    int max_generation_time = mall->current_time + 300; // 原始生成时间限制保留，但实际只用来打印提示
    
    // 改成无限循环，靠条件判断退出
    while (1) {
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);
        
        // 打印初始电梯状态
        print_escalator_status();
        
        // 1. 运行电梯
        operate_escalator();
        
        // 打印电梯状态（运行后）
        print_escalator_status();
        
        // 2. 检查队列中的顾客是否可以上电梯
        // 检查上行队列
        if (mall->upQueue->head != NULL) {
            Customer* c = mall->upQueue->head;
            printf("上行队首顾客 %d，等待时间: %d 秒\n", c->id, get_waiting_time(c));
            
            if (can_customer_board(c)) {
                c = dequeue(mall->upQueue);
                board_customer(c);
            } else {
                printf("上行队首顾客 %d 不能上电梯\n", c->id);
            }
        }
        
        // 检查下行队列
        if (mall->downQueue->head != NULL) {
            Customer* c = mall->downQueue->head;
            printf("下行队首顾客 %d，等待时间: %d 秒\n", c->id, get_waiting_time(c));
            
            if (can_customer_board(c)) {
                c = dequeue(mall->downQueue);
                board_customer(c);
            } else {
                printf("下行队首顾客 %d 不能上电梯\n", c->id);
            }
        }
        
        // 打印电梯状态（顾客上电梯后）
        print_escalator_status();
        
        // 3. 随机生成新顾客：只在“当前时间 < generation_end_time”时生成
        if (mall->current_time < generation_end_time) {
            // 原逻辑：最多在300秒内生成顾客，这里仅额外判断是否过了100秒
            if (mall->current_time < max_generation_time) {
                int max_new_customers = MAX_CUSTOMERS - mall->total_customers;
                if (max_new_customers > 0) {
                    int new_customers = (rand() % (max_new_customers + 21)) - 20;
                    if (new_customers > 0) {
                        printf("生成 %d 个新顾客\n", new_customers);
                        
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
                printf("已达最大顾客生成时间（300秒），停止生成新顾客\n");
            }
        } else {
            // 超过 100 秒
            printf("已超过 %d 秒，不再生成新顾客\n", simulation_time);
        }
        
        // 4. 时间前进
        mall->current_time++;
        
        // 5. 输出当前状态统计
        printf("当前商场状态: 总人数=%d, 上行队列=%d, 下行队列=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);
        
        // 退出条件：如果已经过了生成顾客的时间，并且商场空了，则结束模拟
        if (mall->current_time >= generation_end_time && mall->total_customers == 0) {
            printf("已过%d秒且商场顾客已全部完成乘梯，模拟结束\n", simulation_time);
            break;
        }
        
        // 模拟时间流逝
        sleep(1);
    }
    
    printf("\n========== 模拟结束 ==========\n");
    printf("剩余顾客数: %d\n", mall->total_customers);

    // ============ 新增：在模拟结束后打印平均周转时间 ============
    if (completed_customers > 0) {
        double avg_turnaround = (double)total_turnaround_time / completed_customers;
        printf("所有顾客的平均周转时间: %.2f 秒\n", avg_turnaround);
    } else {
        printf("没有完成乘梯的顾客，无法计算平均周转时间。\n");
    }
    // ============================================================
}

// 资源清理函数
void cleanup_resources() {
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
}

// 主函数
int main(int argc, char* argv[]) {
    srand(time(NULL));  // 初始化随机数生成器
    
    // 解析命令行参数
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
    
    // 运行商场控制循环（模拟100秒，但会等顾客全送完才结束）
    mall_control_loop(100);
    
    // 清理资源
    cleanup_resources();
    
    return EXIT_SUCCESS;
}
