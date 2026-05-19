# Game Catur Linux
## Shell + AWK + Signal + Pipes + POSIX Thread + Shared Memory

---

## Deskripsi
Game catur berbasis terminal untuk Rocky Linux / VMware yang mendemonstrasikan
konsep-konsep sistem operasi:

| Konsep | Implementasi |
|--------|-------------|
| **Shell Script** | `chess.sh` - logika utama, UI, kontrol game |
| **AWK** | `chess_engine.awk` - validasi gerakan, logika catur |
| **Signal & Signal Handling** | SIGINT, SIGTERM, SIGUSR1, SIGUSR2 di `chess.sh` & `chess_thread.c` |
| **Named Pipes (FIFO)** | Komunikasi shell ↔ AWK engine |
| **Shared Memory** | State papan catur antar proses |
| **POSIX Thread** | Monitor, Timer, Logger, AI thread di `chess_thread.c` |

---

## Struktur File

```
chess_game/
├── chess.sh           # Main shell script (UI + Game Loop)
├── chess_engine.awk   # AWK Engine (validasi move)
├── chess_thread.c     # POSIX Thread + Shared Memory (C)
├── Makefile           # Build system
├── install.sh         # Script instalasi otomatis
└── README.md          # Dokumentasi ini
```

---

## Instalasi

### Cara Cepat (Rocky Linux / VMware)
```bash
cd chess_game/
chmod +x install.sh
./install.sh
```

### Manual
```bash
# 1. Install GCC (jika belum ada)
sudo dnf install -y gcc glibc-devel   # Rocky Linux / RHEL
# sudo yum install -y gcc              # CentOS 7
# sudo apt install -y gcc              # Ubuntu/Debian

# 2. Kompilasi komponen C
gcc -Wall -std=c99 -D_GNU_SOURCE -o chess_thread chess_thread.c -lpthread -lrt

# 3. Set permission
chmod +x chess.sh chess_engine.awk

# 4. Jalankan
./chess.sh
```

---

## Cara Bermain

### Format Gerakan
```
e2e4    # Pion dari e2 ke e4
g1f3    # Kuda dari g1 ke f3
e2-e4   # Dengan tanda minus (opsional)
```

**Kolom:** `a b c d e f g h` (kiri ke kanan)  
**Baris:** `1-8` (bawah ke atas dari perspektif putih)

### Perintah Game
| Perintah | Fungsi |
|----------|--------|
| `help`   | Tampilkan bantuan |
| `resign` | Menyerah |
| `log`    | Lihat log sistem |
| `quit`   | Keluar dari game |

### Signal (kirim dari terminal lain)
```bash
# Temukan PID game yang berjalan
ps aux | grep chess.sh

# Pause/Resume game
kill -USR1 <PID>

# Tampilkan statistik
kill -USR2 <PID>

# Hentikan game dengan bersih
kill -TERM <PID>
```

---

## Penjelasan Teknis

### 1. Shell Script (`chess.sh`)
- **Signal Handling**: `trap` untuk SIGINT, SIGTERM, SIGUSR1, SIGUSR2
- **Named Pipes**: `mkfifo` untuk komunikasi dua arah
- **Shared Memory**: Implementasi via file `/tmp/chess_shm_<PID>`
- **Background Processes**: Thread monitor via subshell `( ) &`

```bash
# Contoh signal handler
trap 'handle_sigint'  SIGINT
trap 'cleanup'        SIGTERM EXIT
trap 'handle_sigusr1' SIGUSR1
trap 'handle_sigusr2' SIGUSR2
```

### 2. AWK Engine (`chess_engine.awk`)
- Membaca command dari stdin (via pipe)
- Memvalidasi gerakan per jenis buah
- Mendeteksi checkmate
- Output ke stdout (via pipe)

```
# Protocol komunikasi via pipe:
Input:  MOVE WHITE 6 4 4 4
Output: OK:MOVE:1
        atau
        ERROR:INVALID_PAWN_MOVE
```

### 3. POSIX Thread (`chess_thread.c`)
- `pthread_create()` - membuat 4 thread
- `pthread_join()` - menunggu thread selesai
- `pthread_mutex_t` - sinkronisasi akses shared memory
- `sigaction()` - signal handling di C

```c
// 4 Thread yang dibuat:
Thread 0: MONITOR - memantau state game tiap 2 detik
Thread 1: TIMER   - menghitung waktu per giliran
Thread 2: LOGGER  - mencatat riwayat move ke file
Thread 3: AI      - placeholder untuk engine AI
```

### 4. Shared Memory
```c
// POSIX Shared Memory
shm_fd = shm_open("/chess_game_shm", O_CREAT|O_RDWR, 0666);
ftruncate(shm_fd, SHM_SIZE);
shm_ptr = mmap(NULL, SHM_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);

// Mutex di shared memory (process-shared)
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutex_init(&shm_ptr->board_mutex, &attr);
```

### 5. Named Pipes
```bash
# Buat named pipe
mkfifo /tmp/chess_input_$$
mkfifo /tmp/chess_output_$$

# Tulis ke pipe
echo "MOVE WHITE 6 4 4 4" > /tmp/chess_input_$$

# Baca dari pipe
read response < /tmp/chess_output_$$
```

---

## Arsitektur Sistem

```
┌─────────────────────────────────────────┐
│              chess.sh (Main)            │
│  ┌─────────┐  ┌──────────┐  ┌────────┐ │
│  │Signal   │  │Game Loop │  │UI/Draw │ │
│  │Handler  │  │& Input   │  │Board   │ │
│  └────┬────┘  └────┬─────┘  └────────┘ │
└───────┼────────────┼────────────────────┘
        │            │ Named Pipes
        │     ┌──────▼──────┐
        │     │chess_engine │
        │     │   .awk      │
        │     └─────────────┘
        │
        │     Shared Memory (/tmp/chess_shm_PID)
        │     ┌─────────────────────────────┐
        └────►│ board state | move count   │
              │ current player | status    │
              └───────┬─────────────────────┘
                      │
              ┌───────▼──────────────────┐
              │    chess_thread (C)      │
              │  Thread 0: Monitor       │
              │  Thread 1: Timer         │
              │  Thread 2: Logger        │
              │  Thread 3: AI (future)   │
              └──────────────────────────┘
```

---

## Troubleshooting

### Error: mkfifo gagal
```bash
ls -la /tmp/chess_*  # Cek apakah pipe lama masih ada
rm -f /tmp/chess_*   # Hapus jika ada
```

### Error: chess_thread tidak bisa dikompilasi
```bash
# Pastikan gcc dan library ada
gcc --version
rpm -q glibc-devel  # Rocky Linux
# Jika tidak ada: sudo dnf install glibc-devel
```

### Game tidak mau jalan di VMware
```bash
# Pastikan bash tersedia
which bash
bash --version

# Jalankan dengan bash eksplisit
bash chess.sh
```

---

## Pengembangan Lanjutan

Fitur yang bisa ditambahkan:
- [ ] AI komputer menggunakan Minimax + Alpha-Beta pruning di AWK
- [ ] Save/load game via shared memory persistence
- [ ] Multiplayer via network socket + thread per client
- [ ] En passant, castling, pawn promotion pilihan
- [ ] Timer per giliran via SIGALRM

---

*Dibuat untuk pembelajaran konsep Sistem Operasi di Rocky Linux / VMware*
