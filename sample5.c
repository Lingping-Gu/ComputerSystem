#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// --------------- 可调参数 ---------------
#define MAX_CUSTOMERS  200   // 商场最多顾客(也可改大)
#define MAX_STEPS      13    // 楼梯台阶数
#define BATCH_LIMIT    2     // 同方向连续运送批数上限
#define DIFF_THRESHOLD 6     // 队列差额阈值

// --------------- 方向宏 ---------------
#define UP   1
#define DOWN -1
#define IDLE 0

// --------------- 数据结构 ---------------
typedef struct Customer {
    int id;
    int direction;       // UP or DOWN
    time_t arrival_time; // 记录到达商场时刻
    // 也可以记录进入楼梯时刻等
    struct Customer* next;
    struct Customer* prev;
} Customer;

// 简单队列
typedef struct {
    Customer* head;
    Customer* tail;
    int length;
    int direction;  // 该队列代表UP或DOWN
} Queue;

// 楼梯结构
typedef struct {
    Customer* steps[MAX_STEPS];  // 13级台阶
    int direction;               // UP, DOWN, or IDLE
    int num_people;              // 当前楼梯上有多少人
} Stair;

// 商场(全局环境)
typedef struct {
    Queue* upQueue;
    Queue* downQueue;
    Stair* stair;
    int total_customers;    // 包含楼梯上 + 队列中
    int current_time;
} Mall;

// --------------- 全局变量 ---------------
// 用于统计平均周转时间
static long total_turnaround_time = 0; 
static int  completed_customers   = 0;

// 批量计数：记录当前方向已连续完成了多少批(空楼梯一次算1批)
static int  batch_count = 0;

// --------------- 函数声明 ---------------
Queue* init_queue(int dir);
Stair* init_stair();
Mall*  init_mall();
Customer* create_customer(int id, int direction);

void enqueue(Queue* q, Customer* c);
Customer* dequeue(Queue* q);

void print_stair_status(Stair* s);
void operate_stair(Mall* mall);
void mall_control_loop(Mall* mall, int simulation_time);

void cleanup_resources(Mall* mall);

// --------------- 函数定义 ---------------

// 初始化队列
Queue* init_queue(int dir) {
    Queue* q = (Queue*)malloc(sizeof(Queue));
    if(!q) {
        perror("Failed to alloc queue");
        exit(EXIT_FAILURE);
    }
    q->head      = NULL;
    q->tail      = NULL;
    q->length    = 0;
    q->direction = dir; // 1=UP, -1=DOWN
    return q;
}

// 初始化楼梯(13级台阶)
Stair* init_stair() {
    Stair* s = (Stair*)malloc(sizeof(Stair));
    if(!s) {
        perror("Failed to alloc stair");
        exit(EXIT_FAILURE);
    }
    for(int i=0; i<MAX_STEPS; i++){
        s->steps[i] = NULL;
    }
    s->direction = IDLE;
    s->num_people= 0;
    return s;
}

// 初始化商场
Mall* init_mall() {
    Mall* m = (Mall*)malloc(sizeof(Mall));
    if(!m){
        perror("Failed to alloc mall");
        exit(EXIT_FAILURE);
    }
    m->upQueue   = init_queue(UP);
    m->downQueue = init_queue(DOWN);
    m->stair     = init_stair();
    m->total_customers = 0;
    m->current_time    = 0;
    return m;
}

// 创建顾客
Customer* create_customer(int id, int direction){
    Customer* c = (Customer*)malloc(sizeof(Customer));
    if(!c){
        perror("Failed to alloc customer");
        exit(EXIT_FAILURE);
    }
    c->id            = id;
    c->direction     = direction;
    c->arrival_time  = time(NULL);  // 可换成mall->current_time
    c->next          = NULL;
    c->prev          = NULL;
    return c;
}

// --------------- 队列操作 ---------------
void enqueue(Queue* q, Customer* c){
    if(!q->head){
        q->head = c;
        q->tail = c;
    } else {
        q->tail->next = c;
        c->prev       = q->tail;
        q->tail       = c;
    }
    q->length++;
    // 可打印：printf("顾客 %d 入队 %s\n", c->id, (q->direction==UP)?"UP":"DOWN");
}

Customer* dequeue(Queue* q){
    if(!q->head){
        return NULL;
    }
    Customer* c = q->head;
    q->head = c->next;
    if(!q->head){
        q->tail = NULL;
    } else {
        q->head->prev = NULL;
    }
    q->length--;
    return c;
}

// 打印楼梯状态(调试用)
void print_stair_status(Stair* s){
    printf("楼梯 [");
    for(int i=0; i<MAX_STEPS; i++){
        if(s->steps[i]) printf("%d", s->steps[i]->id);
        else printf("0");
        if(i<MAX_STEPS-1) printf(",");
    }
    printf("], 方向: %s, 人数=%d\n",
           (s->direction==UP)?"UP":
           (s->direction==DOWN)?"DOWN":"IDLE",
           s->num_people);
}

// --------------- 楼梯每秒的运作 ---------------
void operate_stair(Mall* mall){
    Stair* s = mall->stair;
    if(s->direction == IDLE){
        // 如果楼梯空闲，就啥也不做(等后面逻辑决定是否进人)
        return;
    }
    if(s->num_people>0){
        // 根据方向让最顶/最底的人离开
        if(s->direction == UP){
            // 顶部 = steps[12]，如果有人
            if(s->steps[MAX_STEPS-1]){
                Customer* c = s->steps[MAX_STEPS-1];
                // 计算周转时间
                long t = mall->current_time - c->arrival_time;
                total_turnaround_time += t;
                completed_customers++;
                // 打印
                printf("顾客 %d 完成上行, 周转时间: %ld秒\n", c->id, t);

                free(c);
                s->steps[MAX_STEPS-1] = NULL;
                s->num_people--;
                mall->total_customers--;
            }
            // 其它人往上挪
            for(int i=MAX_STEPS-2; i>=0; i--){
                if(s->steps[i]){
                    s->steps[i+1] = s->steps[i];
                    s->steps[i]   = NULL;
                }
            }
            // 底部 steps[0] 如果空 -> 从 upQueue 进1人
            if(s->steps[0] == NULL){
                if(mall->upQueue->head){ 
                    Customer* c = dequeue(mall->upQueue);
                    s->steps[0] = c;
                    s->num_people++;
                }
            }
        }
        else if(s->direction == DOWN){
            // 底部=steps[0], 若有人
            if(s->steps[0]){
                Customer* c = s->steps[0];
                long t = mall->current_time - c->arrival_time;
                total_turnaround_time += t;
                completed_customers++;
                printf("顾客 %d 完成下行, 周转时间: %ld秒\n", c->id, t);

                free(c);
                s->steps[0] = NULL;
                s->num_people--;
                mall->total_customers--;
            }
            // 其余往下挪
            for(int i=1; i<MAX_STEPS; i++){
                if(s->steps[i]){
                    s->steps[i-1] = s->steps[i];
                    s->steps[i]   = NULL;
                }
            }
            // 顶部 steps[12]若空 -> 从 downQueue 进1人
            if(s->steps[MAX_STEPS-1] == NULL){
                if(mall->downQueue->head){
                    Customer* c = dequeue(mall->downQueue);
                    s->steps[MAX_STEPS-1] = c;
                    s->num_people++;
                }
            }
        }
    }

    // 如果楼梯此刻空了 => 完成一批
    if(s->num_people == 0){
        batch_count++;
        printf("楼梯空了，完成一批(batches=%d)\n", batch_count);
        Queue* upQ   = mall->upQueue;
        Queue* downQ = mall->downQueue;
        int upLen   = upQ->length;
        int downLen = downQ->length;

        // 若两边都没人 => 设IDLE
        if(upLen==0 && downLen==0){
            s->direction = IDLE;
            batch_count=0; 
            printf("楼梯空闲\n");
        }
        else {
            // 检查是否需要切换
            if(s->direction == UP){
                // 1) 若已达BATCH_LIMIT && 下行有人 -> 切
                if(batch_count>=BATCH_LIMIT && downLen>0){
                    printf("已达BATCH_LIMIT=%d, 切换到下行\n", BATCH_LIMIT);
                    s->direction = DOWN;
                    batch_count=0;
                }
                // 2) 若(downLen - upLen >= DIFF_THRESHOLD) -> 切
                else if(downLen - upLen >= DIFF_THRESHOLD){
                    printf("下行比上行多 >=%d, 切到下行\n", DIFF_THRESHOLD);
                    s->direction = DOWN;
                    batch_count=0;
                }
                // 否则保持 UP
            }
            else if(s->direction == DOWN){
                if(batch_count>=BATCH_LIMIT && upLen>0){
                    printf("已达BATCH_LIMIT=%d, 切换到上行\n", BATCH_LIMIT);
                    s->direction = UP;
                    batch_count=0;
                }
                else if(upLen - downLen >= DIFF_THRESHOLD){
                    printf("上行比下行多 >=%d, 切到上行\n", DIFF_THRESHOLD);
                    s->direction = UP;
                    batch_count=0;
                }
                // 否则保持 DOWN
            }
        }
    }
}

// --------------- 主循环 ---------------
void mall_control_loop(Mall* mall, int simulation_time){
    int next_id = 1; // 给新顾客分配ID
    srand(time(NULL));

    while(1){
        printf("\n----- 时间 %d 秒 -----\n", mall->current_time);

        // 1. 楼梯每秒运作
        operate_stair(mall);

        // 2. 打印楼梯状态(可注释掉以减少输出)
        // print_stair_status(mall->stair);

        // 3. 若楼梯方向是 IDLE，则尝试决定一个方向(若队列有人)
        //    (这能保证楼梯空闲时也会启用某方向)
        if(mall->stair->direction == IDLE){
            int upLen   = mall->upQueue->length;
            int downLen = mall->downQueue->length;
            if(upLen>0 && downLen==0){
                mall->stair->direction = UP;
                printf("楼梯方向设为 UP(只有上行有人)\n");
            }
            else if(downLen>0 && upLen==0){
                mall->stair->direction = DOWN;
                printf("楼梯方向设为 DOWN(只有下行有人)\n");
            }
            else if(upLen>0 && downLen>0){
                // 可以任选一方或做个比较:
                if(upLen>=downLen){
                    mall->stair->direction = UP;
                    printf("楼梯方向设为 UP(上行人数较多or相等)\n");
                } else {
                    mall->stair->direction = DOWN;
                    printf("楼梯方向设为 DOWN(下行人数较多)\n");
                }
            }
        }

        // 4. 若当前时间 < simulation_time，则可能随机生成顾客
        if(mall->current_time < simulation_time){
            // 随机生成 0~2 人
            int new_cust = rand()%3;
            if(new_cust>0){
                printf("本秒生成 %d 个新顾客\n", new_cust);
                for(int i=0; i<new_cust; i++){
                    // 若商场未满
                    if(mall->total_customers < MAX_CUSTOMERS){
                        int dir = (rand()%2==0)? UP:DOWN;
                        Customer* c = create_customer(next_id++, dir);
                        mall->total_customers++;
                        if(dir==UP)   enqueue(mall->upQueue, c);
                        else          enqueue(mall->downQueue, c);
                    }
                    else{
                        printf("商场已满，不再接收新顾客\n");
                    }
                }
            } else {
                printf("本秒没有新顾客到达\n");
            }
        }
        else {
            printf("已达或超过 %d 秒，不再生成新顾客\n", simulation_time);
        }

        // 5. 打印商场状态
        printf("商场状态: 总人数=%d, upQ=%d, downQ=%d, 楼梯上=%d\n",
               mall->total_customers,
               mall->upQueue->length,
               mall->downQueue->length,
               mall->stair->num_people);

        // 6. 结束条件：若当前时间>=100 且 total_customers==0
        if(mall->current_time >= simulation_time && mall->total_customers==0){
            printf("超%d秒且顾客清空，模拟结束\n", simulation_time);
            break;
        }

        // 每秒推进
        mall->current_time++;
        sleep(1);
    }

    // 最终打印平均周转时间
    if(completed_customers>0){
        double avg_turn = (double)total_turnaround_time/completed_customers;
        printf("\n所有顾客平均周转时间: %.2f 秒\n", avg_turn);
    } else {
        printf("\n没有任何顾客完成乘梯?\n");
    }
}

// --------------- 清理资源 ---------------
void cleanup_resources(Mall* mall){
    // 队列
    Customer* tmp;
    while( (tmp=dequeue(mall->upQueue))!=NULL ) {
        free(tmp);
    }
    while( (tmp=dequeue(mall->downQueue))!=NULL ) {
        free(tmp);
    }
    // 楼梯上
    for(int i=0; i<MAX_STEPS; i++){
        if(mall->stair->steps[i]){
            free(mall->stair->steps[i]);
        }
    }
    free(mall->upQueue);
    free(mall->downQueue);
    free(mall->stair);
    free(mall);
}

// --------------- main函数 ---------------
int main(int argc, char* argv[]){
    // 命令行: ./escalator_batch <initial_customers> <num_steps>
    // 例如: ./escalator_batch 10 13
    // 在本示例中, num_steps暂时不影响楼梯大小(固定13), 仅是你想传的参数
    int init_customers = 10;
    int steps = 13;

    if(argc>1){
        init_customers = atoi(argv[1]);
    }
    if(argc>2){
        steps = atoi(argv[2]);
    }

    printf("开始模拟: 初始顾客数=%d, 楼梯台阶(固定13)=%d\n", init_customers, steps);

    // 初始化商场
    Mall* mall = init_mall();

    // 随机方向给初始顾客
    srand(time(NULL));
    for(int i=0; i<init_customers; i++){
        int dir = (rand()%2==0)? UP:DOWN;
        Customer* c = create_customer(i+1, dir);
        mall->total_customers++;
        if(dir==UP)   enqueue(mall->upQueue, c);
        else          enqueue(mall->downQueue, c);
    }

    // 至少模拟100秒,之后等清空
    mall_control_loop(mall, 100);

    cleanup_resources(mall);
    return 0;
}
