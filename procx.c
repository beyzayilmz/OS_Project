//Created by Beyza Yılmaz on 03.12.2025

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
#include <stdbool.h>
#include <pthread.h>
#include <errno.h> // errno kullanmak için 


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
#define MSGSZ (sizeof(Message) - sizeof(long))

SharedData *g_shared = NULL; //mmap sonucu buraya atanir
//Hem main thread, hem monitor thread, hem de IPC listener thread bunlara erişmek zorunda bu yüzden global
sem_t *g_sem = NULL; // global semaphore pointer
int msg_queue_id = -1; // global mesaj kuyrugu id'si
// volatile: Derleyiciye "bu değişken her an dışarıdan değişebilir, optimize etme" der.
// sig_atomic_t: Kesintiye uğramadan okunup yazılabilen veri tipidir
volatile sig_atomic_t stop_flag = 0;


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
void init_ipc() {
    int shm_fd;
    int is_creator = 0; // Biz mi yarattık kontrolü

    // 1. Önce "Sadece yoksa yarat" modunda açmayı dene (O_EXCL)
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);

    if (shm_fd != -1) {
        // Dosyayı biz yarattık, demek ki İLK biziz.
        is_creator = 1;
        printf("[IPC] İlk process başlatıldı (Creator).\n");

        // Sadece ilk yaratan boyutlandırma (ftruncate) yapar
        if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
            perror("ftruncate failed");
            shm_unlink(SHM_NAME); // Hata varsa temizle
            exit(EXIT_FAILURE);
        }
    } 
    else {
        // Bir hata aldık. Eğer hata "Zaten var (EEXIST)" ise sorun yok, bağlanacağız.
        if (errno == EEXIST) {
            printf("[IPC] Var olan kaynağa bağlanılıyor (Client)...\n");
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666); // O_CREAT yok
            if (shm_fd == -1) {
                perror("shm_open (connect) failed");
                exit(EXIT_FAILURE);
            }
        } else {
            // EEXIST dışında bir hataysa gerçekten sorun vardır
            perror("shm_open (create) failed");
            exit(EXIT_FAILURE);
        }
    }

    // 2. Map işlemi 
    g_shared = (SharedData *)mmap(0, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shared == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }

    // Eğer ilk yaratansa, içinin temiz olduğundan emin ol (sıfırla)
    if (is_creator) {
        // SharedData yapısının içini sıfırla ki çöp veri kalmasın
        memset(g_shared, 0, sizeof(SharedData));
    }

    close(shm_fd);

    // --- MESSAGE QUEUE ---
    // Dosya yoksa oluştur (ftok için)
    int fd = open("/tmp/procx_msgfile", O_CREAT | O_RDWR, 0666);
    close(fd);

    key_t key = ftok("/tmp/procx_msgfile", 65);
    msg_queue_id = msgget(key, 0666 | IPC_CREAT);
    if (msg_queue_id == -1) {
        perror("msgget failed");
        exit(EXIT_FAILURE);
    }
    //program her başladığında kuyrukta kalan eski mesajları sil 
    Message temp;
    while(msgrcv(msg_queue_id, &temp, MSGSZ, 0, IPC_NOWAIT) != -1){
        printf("[IPC] Eski oturumdan kalan mesaj temizlendi (PID: %d)\n", temp.sender_pid);
    }
    // --- SEMAPHORE ---
    // Semaphore zaten varsa bağlanır, yoksa 1 değeriyle oluşturur.
    g_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (g_sem == SEM_FAILED) {
        perror("sem_open failed");
        exit(EXIT_FAILURE);
    }
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
        sem_unlink(SEM_NAME);
    }
    printf("Process kaynaklardan ayrıldı (Kaynaklar hala aktif).\n");
}

//program kapanırken attached processleri temizleme
void kill_child_process(){
    sem_wait(g_sem);
    pid_t my_pid = getpid();

    //aktifse, attached ise tablodan silelim 
    for(int i = 0; i<50; i++){
        if(g_shared->processes[i].is_active &&
            g_shared->processes[i].mode == ATTACHED &&
            g_shared->processes[i].owner_pid == my_pid){
                 printf("Çıkış öncesi ATTACHED process sonlandırılıyor. %d\n", g_shared->processes[i].pid);
                 kill(g_shared->processes[i].pid, SIGTERM);

                 g_shared->processes[i].is_active = 0;
                 g_shared->processes[i].status = TERMINATED;
                 if(g_shared->process_count > 0) g_shared->process_count--;
            }
    }
    sem_post(g_sem);
}

void siginit_handler(int sig){
    stop_flag = 1;
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

    if(msgsnd(msg_queue_id, &msg, MSGSZ, 0)== -1){
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
    int status;

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

        if(mode == ATTACHED){ //çalışan process bitene kadar terminal kitlenir
            // attached (0) ise parent process child process'in bitmesini bekler
            waitpid(child_pid, &status, 0); //child process bitene kadar bekler
            printf("[BİTTİ] PID: %d | Komut: %s | Exit Status: %d\n", child_pid, command, WEXITSTATUS(status));
            //child bitti guncelleme yap
            sem_wait(g_sem); //kritik bolgeye giris

            int index = find_by_pid(child_pid);
            if(index != -1){
                g_shared->processes[index].is_active =0;
                g_shared->processes[index].status = TERMINATED;
                if(g_shared->process_count >0){
                    g_shared->process_count--;
                }
            }
            sem_post(g_sem); //kritik bolgeden cikis
            //process sonlandirma bildirimi gonder
            send_notification(2, child_pid); //2: terminate komutu
        }
        free(cmd_copy);
    }
}

//kullanıcıdan komut alıp process_baslat() ı çağıran fonksiyon
void yeni_program_baslat(){
    char cmd_buffer[256];
    char mode_buffer[64];

    ProcessMode secilen_mode;
    //1. komutu al
    printf("Çalıştırılacak komutu girin (örn: sleep 10) : ");

    //gücenli input alma
    if(fgets(cmd_buffer, sizeof(cmd_buffer), stdin) != NULL){
        //sondaki \n karakterini temizleyelim
        cmd_buffer[strcspn(cmd_buffer, "\n")] = 0;
    }else{
        return;
        }//boş giriş kontrolü
        if(strlen(cmd_buffer) == 0){
            printf("Hata: boş komut girdiniz.\n");
            return;
        }
        //2. Modu sor
        while(true){
            printf("Mod seçin (0: Attached, 1: Detached):");
            if(fgets(mode_buffer, sizeof(mode_buffer), stdin) != NULL){
                mode_buffer[strcspn(mode_buffer, "\n")] = 0; //enter karakterini temizle

                if(strcmp(mode_buffer, "0") == 0){
                    secilen_mode = ATTACHED;
                    break;
                }
                else if(strcmp(mode_buffer, "1") == 0){
                    secilen_mode = DETACHED;
                    break;
                }
                else{
                    printf("Hata: Lütfen sadece 0 veya 1 giriniz.\n");
                }
            }else{
                return;
            }
        }
        //3. Process başlat
        process_baslat(cmd_buffer, secilen_mode);
        
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

                    // Eğer process ATTACHED ise, main thread zaten onu waitpid ile bekliyor.
                    // Monitor thread buna karışmasın, yoksa çakışma olur ve araya yazı girer.
                    if(g_shared->processes[i].mode == ATTACHED) {
                        continue; 
                    }

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
void *ipc_listener_thread(void *args){
    Message msg;
    while(1){
        //msgrcv bloklayan bir çağrı , mesaj gelene kadar bekler
        if(msgrcv(msg_queue_id, &msg, MSGSZ,0,0) == -1){
            perror("msgrcs failed");
            sleep(1); //hata olursa döngü cpu yemesin
            continue;
        }

        if(msg.sender_pid == getpid()){
            msgsnd(msg_queue_id, &msg, MSGSZ, 0);
            usleep(100000);
            continue;
        } 

        if(msg.command == 1){
            printf("\n[IPC] Yeni process başlatıldı: PID %d (Gönderen: %d)\n",
                    msg.target_pid, msg.sender_pid);
        }
        else if(msg.command == 2){
            printf("\n[IPC] Process sonlandırıldı: PID %d (Gönderen: %d)\n",
                    msg.target_pid, msg.sender_pid);
        }
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
            //process gerçekten hayatta mı kontrol et(kill(pid, 0) sinyal göndermez, kontrol eder)
            if(kill(g_shared->processes[i].pid, 0) == -1){
                if (errno == ESRCH) { // ESRCH: No such process (Process ölmüş)
                    g_shared->processes[i].is_active = 0;
                    g_shared->processes[i].status = TERMINATED;
                    if(g_shared->process_count > 0) g_shared->process_count--;
                    //ölü olduğu için yazdırma döngünün başına dön
                    continue;
                }
            }
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
    char input[64];
    pid_t target_pid;

    printf("Sonlandirilacak process PID: ");
    //scanf("%d", &target_pid); (buffer sorunu)

    //scanf yerine fgets kullanarak buffer sorununu çözüyoruz
    if(fgets(input, sizeof(input), stdin) != NULL){
        target_pid = atoi(input); //stringi int e çevirme
    }else{
        return;
    }

    sem_wait(g_sem);

    int index = -1;
    //active ve pid target_pid ye eşitse kill yap ve bilidim gönder
    for(int i = 0; i<50; i++){
        //sadece kendi başlattığımız processleri silebiliriz
        if(g_shared->processes[i].is_active && g_shared->processes[i].pid == target_pid){
            index = i;
            break;
        }  
    }if(index != -1){
        //SIGTERM gönder
        if(kill(target_pid, SIGTERM) == -1){
            perror("kill failed");
            sem_post(g_sem);
            return;
        }
        printf("[INFO] Process %d'e SIGTERM sinyali gönderildi.\n", target_pid);
        printf("# Monitor thread sonlanmayı tespit edip raporlayacak...\n");

        sem_post(g_sem);
    }else{
        sem_post(g_sem);
        printf("Bu PID'e ait aktif process bulunamadi.\n");
    }
}

int display_menu() {
    char input[64];

    while (true) {
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

        if (!fgets(input, sizeof(input), stdin)) {
        // Eğer sinyal geldiyse veya hata olduysa -1 dönelim ki main döngüsü anlasın
        return -1; 
        }
        // baştaki boşlukları temizle
        char *p = input;
        while (*p == ' ' || *p == '\t') p++;

        // sadece tek hane kabul: '0'..'3' + newline
        if (p[0] < '0' || p[0] > '3') {
            printf("Invalid selection! Please use 0, 1, 2 or 3.\n");
            continue;
        }
        if (p[1] != '\n' && p[1] != '\0') {
            printf("Please enter a single digit.\n");
            continue;
        }

        return p[0] - '0';
    }
}

int main() {
    // signal() yerine sigaction kullanıyoruz.
    // Bu sayede fgets() gibi sistem çağrılarının "Restart" etmesini engelliyoruz.
    struct sigaction sa;
    sa.sa_handler = siginit_handler;
    sa.sa_flags = 0; // ÖNEMLİ: SA_RESTART flag'ini koymuyoruz! fgets yarıda kesilsin.
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Sigaction hatası");
        exit(EXIT_FAILURE);
    }
    // 1. IPC kaynaklarını hazırla
    init_ipc();

    // 2. Threadleri oluştur
    pthread_t monitor_tid, listener_tid;

    // Monitor thread başlat
    if (pthread_create(&monitor_tid, NULL, monitor_thread, NULL) != 0) {
        perror("Monitor thread oluşturulamadı");
        exit(EXIT_FAILURE);
    }

    // IPC Listener thread başlat
    if (pthread_create(&listener_tid, NULL, ipc_listener_thread, NULL) != 0) {
        perror("Listener thread oluşturulamadı");
        exit(EXIT_FAILURE);
    }

    // Threadlerin sistemden bağımsız çalışabilmesi için detach edebiliriz(main bitince kendi hallerinde ölsünler veya temizlensinler)
    pthread_detach(monitor_tid);
    pthread_detach(listener_tid);

    bool running = true;
    //3. UI Menüsünü göster (Main Thread burada dönecek) (stop_flag kontrolü eklendi)
    while(running && !stop_flag){ 
        int choice = display_menu(); 

        // Eğer kullanıcı Ctrl+C'ye menüdeyken bastıysa:
        if (stop_flag) {
            printf("\n[Sistem] Çıkış sinyali alındı...\n");
            break; // Döngüden çık
        }

        if(choice == -1 ) continue; // Hatalı giriş

        switch (choice)
        {
        case 0: // ÇIKIŞ
            printf("Program sonlandiriliyor...\n");
            running = false;
            break;
        case 1: // YENİ PROGRAM
            yeni_program_baslat();
            break;
        case 2: // LİSTELE
            process_listele();
            break;
        case 3: // SONLANDIR
            process_sonlandir();
            break; 
        default:
            break;
        }
    }

    // 4. Çıkış ve Temizlik
    kill_child_process();
    cleanup_ipc();
    
    return 0;
}