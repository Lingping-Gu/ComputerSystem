/*
Basic codes with no starvation prevention
Run command line: 
gcc -o sample1 sample1.c -pthread
./sample1 30 12 
*/
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

#define MAX_CUSTOMERS 30
#define MAX_STEPS 13

int num_customers, num_steps;
int current_direction = 0; // 1 for up, -1 for down, 0 for none
int customers_on_stairs = 0;

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
        if (customers_on_stairs == 0 || current_direction == 0 || current_direction == customer->direction) {
            current_direction = customer->direction;
            customers_on_stairs++;
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);
    }

    sem_wait(&stairs);
    printf("Customer %d is crossing the stairs in direction %s\n", customer->id, (customer->direction == 1) ? "up" : "down");
    sleep(1);
    sem_post(&stairs);
    
    pthread_mutex_lock(&mutex);
    customers_on_stairs--;
    if (customers_on_stairs == 0) {
        current_direction = 0;
    }
    pthread_mutex_unlock(&mutex);

    time_t end_time = time(NULL);
    printf("Customer %d finished crossing. Turnaround time: %ld seconds\n", customer->id, end_time - customer->start_time);
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
