# ProcX – IPC Tabanlı Process Yönetim Sistemi (nohup-benzeri)

ProcX, Linux ortamında çalışan; kullanıcıların terminal üzerinden yeni programlar başlatmasına, çalışan süreçleri listelemesine ve süreç sonlandırmasına olanak sağlayan **nohup benzeri** bir süreç yönetim sistemidir.  
Birden fazla terminal oturumundan aynı anda çalıştırılabilir ve instance’lar (ayrı terminalde açılan ProcX’ler) **IPC** mekanizmaları ile birbirinden haberdar olur.

---

## 1) Proje Tanımı

- ProcX ile kullanıcı:
  - Yeni bir programı **Attached** (ön planda bekleyerek) veya **Detached** (arka planda) başlatabilir.
  - Paylaşılan tablo üzerinden aktif süreçleri listeleyebilir.
  - PID ile süreç sonlandırabilir.
- Birden fazla ProcX instance’ı aynı IPC kaynaklarını kullanır:
  - **Shared Memory**: süreç kayıt tablosu
  - **Semaphore**: kritik bölgeler
  - **Message Queue**: instance’lar arası olay bildirimi (start/terminate)

---

## 2) Proje Hedefleri

- Process oluşturma ve yönetimi (**fork, exec**)
- Thread programlama (**pthread**)
- Süreçler arası iletişim (**IPC**)
- Senkronizasyon mekanizmaları (**semaphore**)
- Sinyal yönetimi (**SIGINT, SIGTERM**)  
  (SIGCHLD handler yerine süreçler **waitpid** ile toplanır / izlenir.)

---

## 3) Öğrenim Çıktıları

Bu projeyi tamamladığınızda şu kavramlar pekişir:

1. `fork()` ile yeni process oluşturma  
2. `exec()` ailesi ile program çalıştırma (`execvp`)  
3. `pthread` ile thread yönetimi  
4. POSIX **Shared Memory** (`shm_open`, `mmap`) ile veri paylaşımı  
5. POSIX **Semaphore** (`sem_open`, `sem_wait`, `sem_post`) ile senkronizasyon  
6. System V **Message Queue** (`msgget`, `msgsnd`, `msgrcv`) ile mesajlaşma  
7. **Signal** yönetimi (`sigaction` + `SIGINT`, süreç sonlandırmada `SIGTERM`)  
8. Detached modda `setsid()` kullanımı ile **session izolasyonu** (daemon-benzeri çalışma)

---

## 4) Proje Yapısı

```text
procx/
├── procx.c         # Ana kaynak kod dosyası (tek dosya)
├── Makefile        # Derleme dosyası
└── README.md       # Bu dosya
```

---

## 5) Derleme ve Çalıştırma

### Derleme (GCC)

```bash
gcc -o procx procx.c -pthread
```

> Not: Bazı sistemlerde POSIX shared memory / semaphore için ek flag gerekebilir. Gerekirse:

```bash
gcc -o procx procx.c -pthread -lrt
```

### Çalıştırma

```bash
./procx
```

---

## 6) Menü Seçenekleri

Uygulama açıldığında aşağıdaki gibi bir menü sizi karşılar:

Plaintext
╔════════════════════════════════════╗
║         ProcX v1.0                 ║
╠════════════════════════════════════╣
║ 1. Yeni Program Çalıştır           ║
║ 2. Çalışan Programları Listele     ║
║ 3. Program Sonlandır               ║
║ 0. Çıkış                           ║
╚════════════════════════════════════╝
#### Yeni Program Çalıştır:

Komutu girin (Örn: sleep 100 veya ls -la).

Mod seçin: 0 (Beklemeli) veya 1 (Arka Plan).

#### Çalışan Programları Listele:

O an sistemde aktif olan ve ProcX tarafından yönetilen süreçleri tablo halinde gösterir.

#### Program Sonlandır:

Listeden bir PID seçin ve sonlandırın.

#### Çıkış:

PID girilerek hedef sürece `SIGTERM` gönderilir.  
Detached süreçlerin kapanışı çoğunlukla **monitor thread** tarafından tespit edilip tabloda güncellenir.

---

## 7) Mimari ve Modüller (Özet)

### 7.1 Veri Yapıları

- `ProcessInfo`: PID, owner PID, komut, mod, durum, başlama zamanı, aktiflik
- `SharedData`: 50 sürece kadar kayıt (`processes[50]`) ve `process_count`
- `Message`: IPC bildirimleri (command + sender_pid + target_pid)

### 7.2 IPC Kaynakları

- **Shared Memory (POSIX)**
  - Ad: `/procx_shm`
  - `shm_open` + `ftruncate` + `mmap`
  - İlk instance (creator) belleği `memset` ile sıfırlar.

- **Semaphore (POSIX)**
  - Ad: `/procx_sem`
  - Paylaşılan tabloya yazma/okuma işlemlerini kilitler.

- **Message Queue (System V)**
  - `ftok("/tmp/procx_msgfile", 65)` ile key üretir.
  - Start/Terminate olayları diğer instance’lara bildirilir.

### 7.3 Thread’ler

- **monitor_thread**
  - Periyodik kontrol (2 sn)
  - Sadece **bu instance’ın başlattığı** detached süreçleri `waitpid(..., WNOHANG)` ile izler
  - Biten süreçleri tabloda pasif yapar ve terminate bildirimi gönderir

- **ipc_listener_thread**
  - `msgrcv` ile kuyruktan mesaj bekler
  - Start/Terminate bildirimlerini ekrana basar

### 7.4 Signal Yönetimi

- `sigaction(SIGINT, ...)` ile Ctrl+C yakalanır.
- Handler içinde `stop_flag = 1` yapılır.
- Main loop `stop_flag` kontrolü ile güvenli çıkış yapar.
- Çıkışta:
  - `kill_child_process()` → bu instance’ın ATTACHED süreçlerini sonlandırır
  - `cleanup_ipc()` → `munmap`, `sem_close`, `sem_unlink` vb.

---

## 8) Örnek Senaryo (Çoklu Terminal)

Terminal 1:

```bash
./procx
# 1 -> sleep 30 -> 1 (detached)
```

Terminal 2:

```bash
./procx
# 2 (listele) -> PID’yi gör
# 3 (sonlandır) -> PID gir
```

Beklenen:
- Terminal 2’de listener “process sonlandırıldı” bildirimi görebilir.
- Shared memory tablosu güncellenir.

---

## 9) Bilinen Notlar / Kısıtlar

- Maksimum süreç kaydı: **50**
- Detached mod: `setsid()` ile session ayrıştırma yapılır.  
  (Tam daemonization adımları: `chdir("/")`, stdio kapatma vb. yapılmamıştır.)
- SIGCHLD için ayrı bir handler yoktur; süreçler **waitpid** ile izlenip toplanır.
- IPC message queue temizliği için program başında “eski mesajları çekme” döngüsü bulunur.

---

## 10) Yazar

- Created by **Beyza Yılmaz** (03.12.2025)

---

## 11) Lisans

Ders projesi kapsamında hazırlanmıştır.

---

## ⚠️ Önemli Notlar
Program ilk çalıştırıldığında "Creator" (Oluşturucu) modunda açılır ve paylaşımlı belleği oluşturur.

Sonraki açılan terminaller "Client" modunda bağlanır.

Programdan çıkarken 0 tuşunu kullanmanız veya Ctrl+C yapmanız önerilir; bu sayede shm_unlink ve sem_unlink işlemleri tetiklenir.
