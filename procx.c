#include <stdio.h>
#include <stdlib.h> //exit malloc free
#include <string.h>
#include <sys/ipc.h> // IPC icin gerekli
#include <sys/msg.h> // Mesaj kurugu icin gerekli
#include <sys/shm.h> // Paylasilan bellek icin gerekli
#include <unistd.h> // PID icin gerekli
#include <time.h> // Zaman bilgisi icin gerekli
#include <signal.h> //pid_t, kill, sinyal tipleri
#include <fcntl.h>      // O_CREAT, O_RDWR
#include <sys/mman.h>   // shm_open + mmap
#include <semaphore.h>  // sem_t, sem_open, sem_wait, sem_post


// VERI YAPILARI

// ProcessMode: Attached (0) veya Detached (1)
typedef enum {
    ATTACHED = 0,
    DETACHED = 1,
}ProcessMode;

// ProcessStatus: Running (0) veya Terminated (1)
typedef enum {
    RUNNING = 0,
    TERMINATED = 1,
}ProcessStatus;

// Process bilgisi 
typedef struct {
    pid_t pid; // process id
    pid_t owner_pid; // baslatilan instance'in pid'si
    char command[256]; // calistirilan komut
    ProcessMode mode; 
    ProcessStatus status;
    time_t start_time; // baslangic zamani
    int is_active; // Aktif mi? (1: Evet, 0: Hayır)
}ProcessInfo;

// Paylasilan bellek yapisi
typedef struct{
    ProcessInfo processes[50]; // Maksimum 50 process bilgisi
    int process_count; // aktif process sayisi
}SharedData;

// mesaj yapisi
typedef struct {
    long msg_type; // mesaj tipi
    int command; // komut (start/terminate)
    pid_t sender_pid; // mesaj gonderen process'in pid'si
    pid_t target_pid; // hedef process'in pid'si (terminate icin)
}Message;

// IPC MEKANIZMALARI 

// shared memory ve semaphore 
#define SHM_NAME "/procx_shm"
#define SEM_NAME "/procx_sem"

SharedData *g_shared = NULL; //mmap sonucu buraya atanir
//Hem main thread, hem monitor thread, hem de IPC listener thread bunlara erişmek zorunda bu yüzden global
sem_t *g_sem = NULL; // semaphore pointer

// Process Baslatma
void process_baslat(const char *command, ProcessMode mode){
    pid_t child_pid = fork();

    if(child_pid < 0){
        perror("Fork failed");
        return;
    }

    else if (child_pid == 0){
        // CHILD PROCESS
        if(mode == DETACHED){
            // detached (1) olduğu icin setsid() cagirilir (bagimsiz yeni bi oturum acmasi)
            if(setsid() < 0){ 
                perror("setsid failed"); // yeni oturum acilmazsa hata verir
                //continue
            }
        }

    }
    else{
        // PARENT PROCESS

    }
}

