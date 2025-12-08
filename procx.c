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

// IPC kaynaklari
#define SHM_NAME "/procx_shm" //sahared memory ismi
#define SEM_NAME "/procx_sem" //semaphor ismi
#define MSG_Q_NAME "/procx_mq" //mesaj kuyrugu ismi

SharedData *g_shared = NULL; //mmap sonucu buraya atanir
//Hem main thread, hem monitor thread, hem de IPC listener thread bunlara erişmek zorunda bu yüzden global
sem_t *g_sem = NULL; // global semaphore pointer
int msg_queue_id = -1; // global mesaj kuyrugu id'si


// Kullanıcıdan alınan komut satırını parçalayıp:
//  - argv[] dizisine argümanları yerleştirir
//  - komutun attached mı detached mi olduğunu belirler (& işaretine bakarak)
//  - argüman sayısını (argc) döndürür

//komut satirini parcalayarak execv icin uygun hale getiren fonksiyon
int parse_command(char*line, char **argv, int maxArgs, ProcessMode *mode_out){
    int argc = 0; //su ana kadar bulunan arguman sayisi
    char *token; //strtok icin bulunacak her bir kelime icin gecici pointer

    //satirin sonunda varsa '\n' karakterini kaldir
    line[strcspn(line, "\n")] = '\0';
     *mode_out = ATTACHED; // varsayilan olarak attached

     //ilk tokeni al 
     token = strtok(line, " "); //bosluklara gore parcalama

     // Tokenlar bitene kadar ve argv kapasitesi dolmayana kadar dön
     while(token != NULL && argc < maxArgs -1){
        //son token kontrol et
        if(strcmp(token, "&") == 0){
            *mode_out = DETACHED;
            token = strtok(NULL, " "); //sonraki token e gec
            continue;

        }
            argv[argc++] = token;

            token = strtok(NULL, " "); 
     }
        argv[argc] = NULL; // execvp gibi fonksiyonlar icin arguman lisetsinin sonu null olmalı

        return argc;
}

// IPC mekanizmalari
void init_ipc(){
    //shm_open() file descriptor döndürür
    int shm_fd; // shared memory ram e baglamak icin kimlik

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666); // shared memory olusturuyoruz

    if(shm_fd == -1){ // hata kontrolu
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    // bellek boyutu ayarliyoruz (başlangıc boyutu 0, içine veri yazabileceğimiz fiziksel alan)
    //mmap ile bağlamaya çalışsan bile iş mantıksız olur (okuyacak/yazacak yer yok)
    if(ftruncate(shm_fd, sizeof(SharedData))== -1){
        perror("ftruncate failed");
        exit(EXIT_FAILURE);

    }
    // mmap ile shared memory i process in adres alanina bagliyoruz
    //mmap = Kernel’deki bir dosya / shared memory nesnesini, benim process’imin sanal adres uzayına eşlemek ve bana pointer’ını vermek
    g_shared = (SharedData *)mmap(0, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd,0); // MAP_SHARED: Yaptığın değişiklikler gerçek kaynağa (shared memory nesnesine) yansıtılır
    if(g_shared == MAP_FAILED){
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }

    close(shm_fd); // artık fd'ye ihtiyacimiz yok kapatiyoruz

    //mesaj kuyrugu olusturma
    key_t key = ftok("/tmp/procx_msgfile", 65); // ortak key olusturuyoruz
    if(key == -1){
        perror("ftok failed");
        exit(EXIT_FAILURE);
    }
    msg_queue_id = msgget(key, 0666 | IPC_CREAT); //mesaj kuyrugu olusturuyoruz
    if(msg_queue_id == -1){
        perror("msgget failed");
        exit(EXIT_FAILURE);
    }

    //semaphore olusturma
    g_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1); //1 ile baslangicta 1 kaynak var demis oluyoruz (1 -> binary semaphore, mutex davranis)
    if(g_sem == SEM_FAILED){
        //hata durumunda temizleme yapmasi gerekir
        munmap(g_shared, sizeof(SharedData));
        perror("sem_open failed");
        exit(EXIT_FAILURE);
    }

    printf("IPC mekanizmalari basariyla olusturuldu.\n");
    
}

//program kapanırken ipc temizleme 
void cleanup_ipc(){
    //shared memory temizleme
    if(g_shared != NULL){
        if(munmap(g_shared, sizeof(SharedData)) == -1){
        perror("munmap failed");
        }
        g_shared = NULL;
    }
    // semaphore kapatma
    if(g_sem != SEM_FAILED && g_sem != NULL){
        if(sem_close(g_sem) == -1){
            perror("sem_close failed");
        }
    }
}

// mesaj kuyruguna bildirim gonderme (process baslatma/sonlandirma)
void send_notification(int command, pid_t target_pid){

    if(msg_queue_id == -1){
        fprintf(stderr, "mesaj kuyrugu olusturulmamis. \n");
        return;
    }

    Message msg;
    msg.msg_type = 1; // mesaj tipi
    msg.command = command; // komut
    msg.sender_pid = getpid(); // gönderen pid
    msg.target_pid = target_pid; // hedef pid

    if(msgsnd(msg_queue_id, &msg, sizeof(Message), 0)== -1){
        perror("magsnd failed");
    }
}



// Process Baslatma
void process_baslat(const char *command, ProcessMode mode){
    pid_t child_pid = fork();

    int status;

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
        if(mode == ATTACHED){
            // attached (0) ise parent process child process'in bitmesini bekler
            waitpid(child_pid, &status, 0); //child process bitene kadar bekler

        }

    }
}

// process listeliyoruz
void process_listeleme(){
    
}

// kullanici istegi ile process sonlandirma
void process_sonlandir(){}

void display_menu() {
    printf("\n");
    printf("╔════════════════════════════════════╗\n");
    printf("║         ProcX v1.0                 ║\n");
    printf("╠════════════════════════════════════╣\n");
    printf("║ 1. Yeni Program Çalıştır           ║\n");
    printf("║ 2. Çalışan Programları Listele     ║\n");
    printf("║ 3. Program Sonlandır               ║\n");
    printf("║ 0. Çıkış                           ║\n");
    printf("╚════════════════════════════════════╝\n");
    printf("Seçiminiz: ");
}


