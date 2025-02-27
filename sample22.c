#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

// 不再限制 MAX_CUSTOMERS = 30; 这里只保留 MAX_STEPS = 13 
#define MAX_STEPS 13

// 你原先的最大连续批次 
#define MAX_CONSECUTIVE 5

int num_customers, num_steps;
int current_direction = 0;    // 1=up, -1=down, 0=none
int customers_on_stairs = 0;
int up_consecutive = 0, down_consecutive = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t stairs; // 信号量，容量 = num_steps

// --- 用于计算平均周转时间 ---
static long total_turnaround_time = 0;
static int completed_customers = 0;

// 顾客结构
typedef struct {
    int id;
    int direction;    // 1=up, -1=down
    time_t start_time;
} Customer;

// 线程函数
void* customer_thread(void* arg) {
    Customer* customer = (Customer*)arg;
    customer->start_time = time(NULL);
    printf("Customer %d wants to go %s\n", 
           customer->id, 
           (customer->direction == 1)?"up":"down");

    // 等待进入楼梯(确保方向或连续性条件满足)
    while (1) {
        pthread_mutex_lock(&mutex);
        // 条件：
        //  1) 楼梯上没人 OR 当前方向=我的方向 OR 当前方向=0(还未定)
        //  2) 如果我要上，up_consecutive < MAX_CONSECUTIVE；我要下，down_consecutive < MAX_CONSECUTIVE
        if ( (customers_on_stairs==0 || current_direction==0 || current_direction==customer->direction)
             && ((customer->direction==1 && up_consecutive<MAX_CONSECUTIVE)
                 ||(customer->direction==-1 && down_consecutive<MAX_CONSECUTIVE)) ) 
        {
            current_direction = customer->direction;
            customers_on_stairs++;
            if (customer->direction == 1) {
                up_consecutive++;
                down_consecutive = 0;
            } else {
                down_consecutive++;
                up_consecutive = 0;
            }
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);
        usleep(100000); // 阻塞等待，避免busy-wait
    }

    // 信号量 -> 表示楼梯的台阶数量
    sem_wait(&stairs);
    printf("Customer %d is crossing the stairs in direction %s\n",
           customer->id,
           (customer->direction==1)?"up":"down");
    sleep(1);
    sem_post(&stairs);

    // 离开楼梯
    pthread_mutex_lock(&mutex);
    customers_on_stairs--;
    if (customers_on_stairs==0) {
        // 如果已达最大连续批次 => 强制切方向
        if (up_consecutive>=MAX_CONSECUTIVE || down_consecutive>=MAX_CONSECUTIVE) {
            printf("Switch direction from %s to %s, up_consecutive: %d, down_consecutive: %d\n", 
                   (current_direction==1)?"up":"down", 
                   (-current_direction==1)?"up":"down",
                   up_consecutive,
                   down_consecutive
                  );
            current_direction = -current_direction; 
        }
    }
    pthread_mutex_unlock(&mutex);

    time_t end_time = time(NULL);
    long turnaround = end_time - customer->start_time;
    printf("Customer %d finished crossing in direction %s. Turnaround time: %ld seconds\n",
           customer->id, 
           (customer->direction == 1)?"up":"down",
           turnaround);

    // --- 统计周转时间 ---
    pthread_mutex_lock(&mutex);
    total_turnaround_time += turnaround;
    completed_customers++;
    pthread_mutex_unlock(&mutex);

    free(customer);
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <number_of_customers> <number_of_steps>\n", argv[0]);
        return 1;
    }

    num_customers = atoi(argv[1]);
    num_steps = atoi(argv[2]);

    // 不再限制 30/13；或仅提示而不退出
    if (num_steps > MAX_STEPS) {
        printf("Warning: recommended steps <= %d, but we proceed anyway.\n", MAX_STEPS);
    }

    pthread_t* customers = malloc(sizeof(pthread_t)*num_customers);
    sem_init(&stairs, 0, num_steps);

    srand(time(NULL));

    // 一次性创建 num_customers 个线程
    for (int i=0; i<num_customers; i++) {
        Customer* c = malloc(sizeof(Customer));
        c->id        = i+1;
        c->direction = (rand()%2==0)? 1:-1;
        pthread_create(&customers[i], NULL, customer_thread, c);

        // 随机让下一个顾客稍微晚一点到
        sleep(rand()%2);
    }

    // 等待所有线程结束
    for (int i=0; i<num_customers; i++) {
        pthread_join(customers[i], NULL);
    }
    free(customers);

    sem_destroy(&stairs);
    pthread_mutex_destroy(&mutex);

    // --- 打印最终平均周转时间 ---
    if (completed_customers>0) {
        double avg_turn = (double)total_turnaround_time / completed_customers;
        printf("\nAll customers done. Average Turnaround Time = %.2f seconds\n", avg_turn);
    }
    else {
        printf("No customers completed?\n");
    }
    return 0;
}
