# Project 2: Deadlock-Free Multi-Threaded Staircase Simulation

## 1. Group Information

- **Group Number**: 3
- **Members**: Irene Cai, Lingping Gu, Yilu Xu

## 2. Project Description

This project simulates a department store with two floors connected by a narrow staircase that allows only one person per step. The goal is to implement a multi-threaded solution using **POSIX threads, semaphores, and mutex locks** to prevent **deadlock** and **starvation** while allowing efficient movement of customers in both directions.

## 3. Implementation Details

Each customer is represented as a thread that moves either **up** or **down** the stairs. The project ensures:

- **Deadlock Prevention**: Customers are prevented from blocking each other indefinitely.
- **Starvation Prevention**: No group of customers is unfairly delayed.
- **Efficiency**: More than one customer can move in the same direction at a time.

### Functions and Their Purpose:

#### Initialization Functions:

- `Queue* init_queue(int dir)`: Initializes a queue for customers waiting in a specific direction.
- `Escalator* init_escalator()`: Initializes the escalator structure.
- `Mall* init_mall()`: Initializes the entire simulation environment.

#### Customer Management:

- `Customer* create_customer_struct(int direction, int arrival_time)`: Creates a new customer structure.
- `void* customer_thread(void* arg)`: Handles customer logic in a separate thread.
- `void create_customer(int direction)`: Spawns a new customer thread.

#### Queue Management:

- `void enqueue(Queue* q, Customer* c)`: Adds a customer to the queue.
- `Customer* dequeue(Queue* q)`: Removes a customer from the queue.

#### Escalator Operations:

- `int can_customer_board(Customer* c)`: Checks if a customer can board the escalator.
- `void board_customer(Customer* c)`: Moves a customer onto the escalator.
- `void operate_escalator()`: Moves customers along the escalator.
- `void print_escalator_status()`: Prints the current status of the escalator.

#### Simulation Control:

- `void mall_control_loop(int simulation_time)`: Runs the main simulation loop.
- `void cleanup_resources()`: Frees allocated memory and cleans up resources.

## 4. Testing and Validation


### Test Cases:

1. **Basic Functionality**: Run the mall simulation for **100 seconds** to test the normal flow of customers entering and exiting.
2. **Entry Restriction**: After **100 seconds**, no new customers are allowed to enter, but the simulation waits until all **30 customers inside have exited** to ensure proper termination.
3. **Randomized Customer Generation**: Customers are generated at a random rate of **0-3 per second**, testing load and customer flow dynamics.
4. **High Load**: Higher traffic was tested, but due to the mall's **maximum capacity of 30 people**, increased load had minimal impact, ensuring the mall does not exceed its limit.
5. **Deadlock and Starvation Prevention**: Validate that under high traffic, the system continues to function smoothly, avoiding deadlock or unfair waiting times.
6. **Performance Optimization**: Observe the mall's efficiency under **various customer flow rates**, optimizing the entry and exit rules.


### Deadlock Handling
Deadlock is prevented using a combination of **mutex locks and semaphores** to ensure exclusive access to shared resources. The escalator capacity is controlled via a semaphore, ensuring that no more than the allowed number of customers are on the escalator at any time. Additionally, a **mutex lock** is used to synchronize queue operations and prevent race conditions when customers attempt to board or leave the escalator. These mechanisms enforce orderly movement and eliminate circular waits, a key cause of deadlock.

### Starvation Handling
If a direction has been served for too long and the opposite direction has waiting customers, the direction is switched.

### Key Starvation Strategy
During our exploration of different starvation prevention strategies, we found that to achieve the lowest **turnaround time**, it is always best to **minimize direction switches**. However, this approach inevitably leads to starvation for customers waiting in the opposite direction. To balance efficiency and fairness, we ultimately adopted a **"five-person batch" strategy**, where a direction change is only considered after at least five people have been served in one direction.**Deadlock Handling**: Deadlock is prevented using a combination of **mutex locks and semaphores** to ensure exclusive access to shared resources. The escalator capacity is controlled via a semaphore, ensuring that no more than the allowed number of customers are on the escalator at any time. Additionally, a **mutex lock** is used to synchronize queue operations and prevent race conditions when customers attempt to board or leave the escalator. These mechanisms enforce orderly movement and eliminate circular waits, a key cause of deadlock.

   **Starvation Handling**: If a direction has been served for too long and the opposite direction has waiting customers, the direction is switched.
6. **Key Starvation Strategy**: During our exploration of different starvation prevention strategies, we found that to achieve the lowest **turnaround time**, it is always best to **minimize direction switches**. However, this approach inevitably leads to starvation for customers waiting in the opposite direction. To balance efficiency and fairness, we ultimately adopted a **"five-person batch" strategy**, where a direction change is only considered after at least five people have been served in one direction.

### Performance Optimization:

- **Turnaround Time Calculation**: The average turnaround time is computed and used to optimize queue management.
- **Semaphore and Mutex Locks**: Ensures safe concurrent access while maintaining efficiency.

## 5. Compilation and Execution

### Compilation:

```sh
gcc -pthread sample7.c -o project2
```

### Running the Program:

```sh
./project2 [initial_customers]
```

Example:

```sh
./project2 10
```

This starts the simulation with 10 initial customers.

## 6. Contributions

- **Starvation Prevention Design**: [Irene] researched and implemented the **five-person batch strategy**, ensuring a balance between efficiency and fairness while preventing starvation.
- **Multi-threading, Locks, and Semaphore Design**: [Yilu] designed and implemented the **mutex locks and semaphore mechanisms**, ensuring safe concurrent access and preventing deadlocks.
- **Turnaround Time Optimization**: [Lingping] analyzed turnaround times and optimized queue processing to minimize delays while maintaining efficient movement through the escalator.

---

This project successfully simulates a **multi-threaded** department store escalator system with an effective **deadlock-free** and **starvation-free** design.

