#include <iostream>
#include <stdio.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include "pthread.h"
#include "semaphore.h"

pthread_barrier_t client_barrier; // a barrier to synchronize the starting point of the clients
pthread_barrier_t teller_barrier; // and another for the tellers
bool availability[3]; // a boolean array holding the availability of teller threads
std::vector<bool> seats; // the occupation status of the seats in the hall (true => occupied)
std::ofstream of;
int theater_size;

// declaring and initializing constructs for establishing inter-thread synchronization, these will be explained further
// on when they're used
sem_t teller_semaphore;
pthread_mutex_t seat_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t teller_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond1_a = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond1_b = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond1_c = PTHREAD_COND_INITIALIZER;

// a structure for holding the necessary data associated with a client
typedef struct {
    std::string name;
    int wait_time, service_time, requested_seat;
} Client;

Client head_of_queue; // a buffer that represents the head of the queue, the current client to be served

// the routine that will be performed by teller threads
void *teller_routine(void *arg) {
    std::string name = (char *) arg; // the name of the teller ("Teller A" etc.) is passed in as argument
    pthread_cond_t *cond1;

    // determine the "number" of the thread (which index of the availability array it will act on) and its associated
    // condition variable according to which teller it is
    int thread_no;
    if (name == "Teller A") {
        cond1 = &cond1_a;
        thread_no = 0;
    } else if (name == "Teller B") {
        cond1 = &cond1_b;
        thread_no = 1;
    } else {
        cond1 = &cond1_c;
        thread_no = 2;
    }

    // log the arrival of the teller
    std::cout << name << " has arrived." << std::endl;
    pthread_barrier_wait(&teller_barrier);

    // this is the main part of the routine, this loop will run until all clients are processed, then be blocked
    // forever halfway through when there are no clients left
    while (true) {
        int given_seat = -1;
        // this thread will now be entering a critical section, taking a client from the buffer and assigning them a
        // seat. we must acquire the teller_mutex before entering the critical section
        pthread_mutex_lock(&teller_mutex);
        availability[thread_no] = true;

        // increment teller_semaphore by 1, indicating to the clients that a teller is now available and set thread's
        // own availability status to true (this is needed to preserve the priority order between threads)
        sem_post(&teller_semaphore);

        // each teller thread will now be waiting for its own condition variable to be set, indicating a client has been
        // placed in to the head of the queue and is requesting for this teller to receive them
        pthread_cond_wait(&*cond1, &teller_mutex);

        // take the next client to be processed from the head of the queue and release the lock
        Client c = head_of_queue;
        pthread_mutex_unlock(&teller_mutex);

        // now we will be assigning an appropriate seat. check the requested seat of the client, see if the seat
        // actually exists, and is available. if not, assign the available seat with the lowest number. if no such seat
        // exists, assign nothing. note that we need to acquire another lock because we are accessing a shared resource,
        // namely, the seats array
        pthread_mutex_lock(&seat_mutex);
        int requested_seat = c.requested_seat;
        if (requested_seat <= theater_size && !seats.at(requested_seat)) {
            given_seat = requested_seat;
            seats.at(requested_seat) = true;
        } else {
            for (int i = 1; i < seats.size(); i++) {
                if (!seats.at(i)) {
                    given_seat = i;
                    seats.at(i) = true;
                    break;
                }
            }
        }

        // do not forget to release the lock, as we are leaving the critical section. then sleep for the duration of
        // the processed client's service time
        pthread_mutex_unlock(&seat_mutex);
        std::this_thread::sleep_for(std::chrono::milliseconds(c.service_time));

        // we are entering another critical section, as output stream is a shared resource between the teller threads
        // acquire the lock
        pthread_mutex_lock(&print_mutex);
        if (given_seat != -1) {
            std::cout << c.name << " requests seat " << requested_seat << ", reserves seat " << given_seat << "."
                      << " Signed by " << name << "." << std::endl;
        } else {
            std::cout << c.name << " requests seat " << requested_seat << ", reserves None. Signed by " << name << "."
                      << std::endl;
        }

        // leaving critical section. release the lock
        pthread_mutex_unlock(&print_mutex);
    }

}

// the routine that will be performed by client threads
void *client_routine(void *arg) {

    // a client struct will be passed in as argument
    Client *client = (Client *) arg;

    // get the wait time of the client from the struct and wait for that duration
    int wait_time = client->wait_time;

    // all client threads will wait for each other after first being created here before moving on
    pthread_barrier_wait(&client_barrier);

    // wait for the client's wait time
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));

    // now entering a critical section. we will be moving to the head of the waiting queue! acquire the lock
    pthread_mutex_lock(&buffer_mutex);

    // write yourself to the head of the queue
    head_of_queue = *client;

    // wait on a teller thread to become available
    sem_wait(&teller_semaphore);

    // since in case where more than one teller thread is available, we want a priority ordering of teller A > teller B
    // > teller C, we have the following control flow: we check the availabilities of the teller threads in order and
    // signal them accordingly, in priority order. we are guaranteed that one and only one of them will be singnaled,
    // since the semaphore guarantees the availability of at least one and the mutually exclusive control flow (the if-
    // else statements) guarantees the activation of at most one. also set the activated thread's availability to false
    if (availability[0]) {
        pthread_cond_signal(&cond1_a);
        availability[0] = false;
    }
    else if (availability[1]) {
        pthread_cond_signal(&cond1_b);
        availability[1] = false;
    }
    else {
        pthread_cond_signal(&cond1_c);
        availability[2] = false;
    }

    // leaving critical section, release lock
    pthread_mutex_unlock(&buffer_mutex);

    // sleep for the service time of the client, then exit
    std::this_thread::sleep_for(std::chrono::milliseconds(client->service_time));
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {

    // this part is pretty self explanatory, initializing several variables, getting the input etc.
    std::ios_base::sync_with_stdio(false);
    std::string theater_name;
    int no_clients;
    sem_init(&teller_semaphore, 0, 0);
    pthread_barrier_init(&teller_barrier, NULL, 4); // main + 3 teller threads = 4
    std::vector<Client> clients; // a vector to hold the clients' data

    std::string input_path = argv[1];
    std::string output_path = argv[2];

    of.open(output_path);
    std::cout.rdbuf(of.rdbuf()); // redirect cout to output to the file, this is very convenient!

    std::cout << "Welcome to the Sync-Ticket!" << std::endl;

    std::ifstream input_file(input_path);

    // get the first 2 singular lines...
    if (input_file.is_open()) {
        std::string line;
        getline(input_file, line);
        theater_name = line;
        getline(input_file, line);
        no_clients = stoi(line);

        // now we know the number of clients, so we can initialize the client barrier
        pthread_barrier_init(&client_barrier, NULL, no_clients);

        // ... then the following no_clients lines and split the information in them into 4 separate variables
        for (int i = 0; i < no_clients; i++) {
            getline(input_file, line);
            std::stringstream split_line(line);
            std::string temp;
            Client c;
            std::getline(split_line, temp, ',');
            c.name = temp;
            std::getline(split_line, temp, ',');
            c.wait_time = stoi(temp);
            std::getline(split_line, temp, ',');
            c.service_time = stoi(temp);
            std::getline(split_line, temp, ',');
            c.requested_seat = stoi(temp);
            clients.push_back(c);
        }
    }

    // determine the number of seats from the theater name and resize the seats vector accordingly
    if (theater_name == "OdaTiyatrosu") {
        theater_size = 60;
        seats.resize(61);
    } else if (theater_name == "UskudarStudyoSahne") {
        theater_size = 80;
        seats.resize(81);
    } else {
        theater_size = 200;
        seats.resize(201);
    }

    // creating the teller threads, waits in between to ensure correct order of arrival logs. also, we pass the name of
    // the teller as argument
    pthread_t teller_a, teller_b, teller_c;

    pthread_create(&teller_a, NULL, &teller_routine, (void *) "Teller A");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pthread_create(&teller_b, NULL, &teller_routine, (void *) "Teller B");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pthread_create(&teller_c, NULL, &teller_routine, (void *) "Teller C");

    pthread_barrier_wait(&teller_barrier);


    // creating the client threads. we pass in corresponding clients as argument
    pthread_t tids[no_clients];
    for (int i = 0; i < no_clients; i++) {
        pthread_t client;
        pthread_create(&client, NULL, &client_routine, &clients.at(i));
        tids[i] = client;
    }

    // we have to wait for each client thread to exit, join them to main, which blocks main until all clients exit
    for (int i = 0; i < no_clients; i++) {
        pthread_join(tids[i], NULL);
    }

    // another wait for synchronization reasons
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "All clients received service." << std::endl;
    of.close();
    return 0;
}

