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
#include <sys/wait.h>


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

    int fd = open("/tmp/procx_msgfile", O_CREAT | O_RDWR, 0666); //ftok tan önce boş bir dosya hazırlıyoruz
        if (fd == -1) {
            perror("open msgfile");
            exit(EXIT_FAILURE);
    }
        close(fd);

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

//yeni process eklemek için bos hücre bulma (index dondurur) (kayit ekleme fonksiyonu icin gerekli)
int find_empty_process_slot(){

    for(int i = 0; i < 50; i++){
        if(g_shared->processes[i].is_active == 0){
            return i; //bos hucre bulundu
        }
    }
    return -1; //bos hucre bulunamadi
}

//tabloda verilen pid'ye sahip processin indexini bulma 
int find_by_pid(pid_t pid){

    for (int i = 0; i< 50; i++){
        if(g_shared -> processes[i].is_active &&
            g_shared -> processes[i].pid == pid){
                return i;
            }
    }
    return -1;
}

//shared memory kayıt ekleme fonksiyonu (paren process tarafinda cagirilacak)
int add_process_record(pid_t child_pid, const char* command, ProcessMode mode){
    sem_wait(g_sem); //kritik bolgeye giris yaptık (lock)

    int index = find_empty_process_slot(); // bos hucre buluyoruz (index:yeni processin yazilacagi yerin indexi)

    if(index != -1){
        ProcessInfo *p = &g_shared -> processes[index]; // yeni process kaydi icin pointer
        p->pid = child_pid; // processInfodaki process id
        p->owner_pid = getpid(); // bu processi baslatan parent processin pid'si
        strncpy(p->command, command, sizeof(p->command)-1); //command p->command' a kopyalanir, buffer tasmasin diye -1
        p->command[sizeof(p->command)-1] = '\0'; //son karakter null
        p->mode = mode; // listelemede kullanilacak
        p->status = RUNNING; 
        p->start_time = time(NULL);
        p->is_active = 1;
        g_shared ->process_count++; // kac tane aktif process var sayaci
        printf("[BAŞLATILDI] PID: %d | Mod: %s | Komut: %s\n", 
               child_pid, mode == ATTACHED ? "ATTACHED" : "DETACHED", command);
        sem_post(g_sem); //kritik bolgeden cikis yapıldı (unlock)       
        return index; //başarılı       
    } else {
        fprintf(stderr,"Maksimum process sayisina ulasildi!\n");
        sem_post(g_sem);
        return -1; //başarısız
    }
}


// Process Baslatma
void process_baslat(const char *command, ProcessMode mode){ //command: kullanicinin yazdiği komut "sleep 10" gibi, mode: attached/detached

    char *cmd_copy = strdup(command); //komutu degistirmemek icin kopyaliyoruz
    if(cmd_copy == NULL){
        perror("srtdup failed");
        return;
    }

    char *argv[64]; //arguman listesi
    int argc = parse_command(cmd_copy, argv, 64, &mode); //komutu parcalayip argv ye atiyoruz

    if(argc == 0){
        fprintf(stderr, "Gecersiz komut!\n");
        free(cmd_copy); // kopyayi serbest birak
        return;
    }

    pid_t child_pid = fork(); //yeni process olustur

    if(child_pid < 0){
        perror("Fork failed");
        free(cmd_copy); 
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
        //int execvp(const char *dosya_adi, char *const argv[]);
        execvp(argv[0], argv); // komutu calistir
        perror("execvp failed");
        exit(EXIT_FAILURE); // hata durumunda child process sonlandir

    }

    else{
        //burada kritik bölge mevzuları olacak!!!
        // PARENT PROCESS

        //shared memory'ye process kaydi ekle
        add_process_record(child_pid, command, mode);

        //process baslatma bildirimi gonder
        send_notification(1, child_pid); //1: start komutu

        free(cmd_copy);

    }
}
//Monitor Thread: periyodik olarak process durumlarını kontrol eder (health check, logging, dynamic scaling, cleanup)
void *monitor_thread(void *arg){
    int status;

    while(1){
        sleep(2);

        sem_wait(g_sem); //shared memory erişimi için kilit

        for(int i = 0; i<50; i++){
            //sadece aktif ve bu instance tarafından başlatılmış processler 
            if(g_shared->processes[i].is_active &&
                g_shared->processes[i].owner_pid == getpid()){

                    pid_t result = waitpid(g_shared->processes[i].pid, &status, WNOHANG);

                    if(result > 0){
                    // Process sonlanmış!
                    printf("\n[MONITOR] Process %d sonlandı. Exit Code: %d\n", 
                           result, WEXITSTATUS(status));

                           //shared memory temizleyelim
                           g_shared->processes[i].is_active = 0;
                           g_shared->processes[i].status = TERMINATED;

                           if(g_shared->process_count > 0){
                            g_shared->process_count--;
                           }

                           //bildirim gönderelim
                           send_notification(2, result);
                    }
                }
        }
        sem_post(g_sem);
    }
    return NULL;
}

// process listeliyoruz
void process_listele() {
    sem_wait(g_sem);

    printf("\nAktif Process Listesi:\n");
    printf("+----------+----------------------+-----------+-----------+----------+\n");
    printf("| PID      | Komut                | Mod       | OwnerPID  | Elapsed  |\n");
    printf("+----------+----------------------+-----------+-----------+----------+\n");

    time_t now = time(NULL);
    int active_count = 0;

    for (int i = 0; i < 50; i++) {
        if (g_shared->processes[i].is_active) {
            ProcessInfo *p = &g_shared->processes[i];
            double elapsed = difftime(now, p->start_time);

            printf("| %-8d | %-20s | %-9s | %-9d | %-8.0f |\n",
                   p->pid,
                   p->command,
                   (p->mode == DETACHED) ? "Detached" : "Attached",
                   p->owner_pid,
                   elapsed);

            active_count++;
        }
    }

    printf("+----------+----------------------+-----------+-----------+----------+\n");
    printf("Toplam aktif process: %d\n\n", active_count);

    sem_post(g_sem);
}

// kullanici istegi ile process sonlandirma
void process_sonlandir(){
    pid_t target_pid;
    printf("Sonlandirilacak process PID: ");
    scanf("%d", &target_pid);

    sem_wait(g_sem);

    //active ve pid target_pid ye eşitse kill yap ve bilidim gönder
}

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
