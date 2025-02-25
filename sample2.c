/*
Codes with time slice starvation prevention
Run command line: 
gcc -o sample2 sample2.c -pthread
./sample2 30 12  
*/
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30
#define MAX_STEPS 13
#define MAX_CONSECUTIVE 5 // Maximum consecutive customers before switching

int num_customers, num_steps;
int current_direction = 0; // 1 for up, -1 for down, 0 for none
int customers_on_stairs = 0;
int up_consecutive = 0, down_consecutive = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t stairs;

typedef struct {
    int id;
    int direction; // 1 for up, -1 for down
    time_t start_time;
} Customer;

void* customer_thread(void* arg) {
    Customer* customer = (Customer*)arg;
    customer->start_time = time(NULL);
    printf("Customer %d wants to go %s\n", customer->id, (customer->direction == 1) ? "up" : "down");

    while (1) {
        pthread_mutex_lock(&mutex);
        if ((customers_on_stairs == 0 || current_direction == 0 || current_direction == customer->direction) &&
            ((customer->direction == 1 && up_consecutive < MAX_CONSECUTIVE) ||
             (customer->direction == -1 && down_consecutive < MAX_CONSECUTIVE))) {
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
        usleep(100000); // Small delay to prevent busy waiting
    }

    sem_wait(&stairs);
    printf("Customer %d is crossing the stairs in direction %s\n", customer->id, (customer->direction == 1) ? "up" : "down");
    sleep(1);
    sem_post(&stairs);
    
    pthread_mutex_lock(&mutex);
    customers_on_stairs--;
    
    if (customers_on_stairs == 0) {
        if (up_consecutive >= MAX_CONSECUTIVE || down_consecutive >= MAX_CONSECUTIVE) {
            printf("Switch direction from %s to %s, up_consecutive: %d, down_consecutive: %d\n", (current_direction == 1) ? "up" : "down", (-current_direction == 1) ? "up" : "down", up_consecutive, down_consecutive);
            current_direction = -current_direction; // Force a direction switch
        } 
    }
    pthread_mutex_unlock(&mutex);

    time_t end_time = time(NULL);
    printf("Customer %d finished crossing in direction %s\n. Turnaround time: %ld seconds\n", customer->id, (customer->direction == 1) ? "up" : "down", end_time - customer->start_time);
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

    if (num_customers > MAX_CUSTOMERS || num_steps > MAX_STEPS) {
        printf("Error: Max customers = 30, Max steps = 13\n");
        return 1;
    }

    pthread_t customers[num_customers];
    sem_init(&stairs, 0, num_steps);

    srand(time(NULL));
    
    for (int i = 0; i < num_customers; i++) {
        Customer* customer = malloc(sizeof(Customer));
        customer->id = i + 1;
        customer->direction = (rand() % 2 == 0) ? 1 : -1;
        pthread_create(&customers[i], NULL, customer_thread, customer);
        sleep(rand() % 2);
    }

    for (int i = 0; i < num_customers; i++) {
        pthread_join(customers[i], NULL);
    }

    sem_destroy(&stairs);
    pthread_mutex_destroy(&mutex);
    return 0;
}
