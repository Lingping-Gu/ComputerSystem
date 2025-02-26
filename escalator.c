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

// 统计变量
int total_turnaround_time = 0; // 记录所有顾客的累计周转时间
int completed_customers = 0;   // 记录完成乘梯的顾客数量

// 扶梯结构体
typedef struct {
    Customer* steps[MAX_ESCALATOR_CAPACITY];  // 扶梯上的顾客，每个阶梯对应一个顾客
    int direction;      // UP: 上行, DOWN: 下行, IDLE: 空闲
    int num_people;     // 当前扶梯上有多少人
} Escalator;

// 商场结构体
typedef struct {
    Queue* upQueue;     // 上行队列
    Queue* downQueue;   // 下行队列
    Escalator* escalator; // 扶梯
    int total_customers;  // 商场内的总人数
    int current_time;     // 系统当前时间
} Mall;

// 电梯运行函数（记录周转时间）
void operate_escalator() {
    Escalator* escalator = mall->escalator;
    
    if (escalator->num_people > 0) {
        printf("电梯运行中，方向: %s，当前载客数: %d\n", 
               (escalator->direction == UP) ? "上行" : "下行", 
               escalator->num_people);

        int completed = 0;
        
        if (escalator->direction == UP) {
            if (escalator->steps[MAX_ESCALATOR_CAPACITY-1] != NULL) {
                Customer* c = escalator->steps[MAX_ESCALATOR_CAPACITY-1];
                int turnaround_time = mall->current_time - c->arrival_time;
                total_turnaround_time += turnaround_time;
                completed_customers++;
                
                printf("顾客 %d 完成乘梯(上行)，周转时间: %d 秒\n", c->id, turnaround_time);
                
                free(c);
                escalator->steps[MAX_ESCALATOR_CAPACITY-1] = NULL;
                escalator->num_people--;
                mall->total_customers--;
                completed = 1;
            }

            for (int i = MAX_ESCALATOR_CAPACITY-2; i >= 0; i--) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i+1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        } else if (escalator->direction == DOWN) {
            if (escalator->steps[0] != NULL) {
                Customer* c = escalator->steps[0];
                int turnaround_time = mall->current_time - c->arrival_time;
                total_turnaround_time += turnaround_time;
                completed_customers++;

                printf("顾客 %d 完成乘梯(下行)，周转时间: %d 秒\n", c->id, turnaround_time);
                
                free(c);
                escalator->steps[0] = NULL;
                escalator->num_people--;
                mall->total_customers--;
                completed = 1;
            }

            for (int i = 1; i < MAX_ESCALATOR_CAPACITY; i++) {
                if (escalator->steps[i] != NULL) {
                    escalator->steps[i-1] = escalator->steps[i];
                    escalator->steps[i] = NULL;
                }
            }
        }

        if (escalator->num_people == 0) {
            Queue* current_queue = (escalator->direction == UP) ? mall->upQueue : mall->downQueue;
            Queue* opposite_queue = (escalator->direction == UP) ? mall->downQueue : mall->upQueue;

            if ((current_queue->head == NULL && opposite_queue->head != NULL) || 
                should_wait_for_opposite(opposite_queue)) {
                printf("电梯方向切换: %s -> %s\n", 
                       (escalator->direction == UP) ? "上行" : "下行",
                       (escalator->direction == UP) ? "下行" : "上行");
                
                escalator->direction = -escalator->direction;
            } else if (current_queue->head == NULL && opposite_queue->head == NULL) {
                printf("电梯空闲\n");
                escalator->direction = IDLE;
            }
        }
    }
}

// **修改 mall_control_loop() 终止条件**
void mall_control_loop(int simulation_time) {
    int end_time = mall->current_time + simulation_time;
    int stop_generating_time = mall->current_time + 100;

    while (1) {
        printf("\n========== 时间: %d 秒 ==========\n", mall->current_time);
        print_escalator_status();
        operate_escalator();
        print_escalator_status();

        if (mall->current_time < stop_generating_time) {
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
        }

        mall->current_time++;

        printf("当前商场状态: 总人数=%d, 上行队列=%d, 下行队列=%d, 电梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->escalator->num_people);

        if (mall->current_time >= stop_generating_time && mall->total_customers == 0) {
            printf("超过100秒，且商场已空，模拟结束\n");
            break;
        }

        sleep(1);
    }

    if (completed_customers > 0) {
        printf("\n========== 统计数据 ==========\n");
        printf("总完成顾客数: %d\n", completed_customers);
        printf("平均周转时间: %.2f 秒\n", (float)total_turnaround_time / completed_customers);
    } else {
        printf("没有顾客完成乘梯，无法计算平均周转时间\n");
    }
}
