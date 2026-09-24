#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dao.h>

// Forward declarations for functions that will replace dao/she library
// You'll need to implement these based on your library
// # void* initialize_shm(const char* filename);
// # void* get_shm_data(void* shm, int check);
// # int get_data_size(void* shm);
// # // End of forward declarations

int running = 1;

void handle_sigint(int sig) {
    printf("\nExiting\n");
    running = 0;
}

int main(int argc, char* argv[])
{

    if (argc != 4) 
    {
        printf("Usage: %s <SHM> <IP> <PORT>\n", argv[0]);
        return 1;
    }

    char* shm_filename = argv[1];
    char* ip = argv[2];
    int port = atoi(argv[3]);

    printf("Reading file: %s\n", shm_filename);
    

    IMAGE *dmImg = (IMAGE*) malloc(sizeof(IMAGE));
    daoShmOpen(inShmName, &dmImg[0]);
    int nActs = dmImg[0].md[0].size[0]*inShm[0].md[0].size[0];
    printf("nActs: %d\n", nActs);

    printf("Sending on %s:%d\n", ip, port);

    // Create UDP socket
    int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) {
        perror("socket creation failed");
        return 1;
    }

    // Set up destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dest_addr.sin_addr) <= 0) {
        perror("inet_pton failed");
        close(udp_socket);
        return 1;
    }

    // Set up signal handler for graceful exit
    signal(SIGINT, handle_sigint);

    // Timing variables
    struct timespec timeout;
    struct timespec t[3];
    double elapsedTime;
    clock_gettime(CLOCK_REALTIME, &t[1]);
    int count = 0;
    int tCount = 0;

    // Main loop
    size_t data_size = sizeof(float) * nActs;
    while (running)
    {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1; // 1 second timeout
        // Get data
        if (daoShmWaitSemTimeout(inShm, 2, &timeout) != -1)
        {
            // Send data
            sendto(udp_socket, dmImg[0].array.F, data_size, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            clock_gettime(CLOCK_REALTIME, &t[1]);
            elapsedTime = (t[1].tv_sec - t[0].tv_sec) * 1e3;
            elapsedTime += (t[1].tv_nsec - t[0].tv_nsec) / 1e6;
            printf("\r fps = %8.3f Hz, %d in=[%6.3f,%6.3f,...,%6.3f]", 1e6/(1000*elapsedTime), 
                                                                                  nActs, dmImg[0].array.F[0],
                                                                                  dmImg[0].array.F[1],
                                                                                  dmImg[0].array.F[nActs-1]);
        }
        else
        {
            waitCounter += 1;
            printf("\rWAIT %d", waitCounter);
        }
        fflush(stdout);
    }

    // Clean up
    close(udp_socket);
    return 0;
}