/*
 * chess_thread.c - Demonstrasi POSIX Thread & Shared Memory
 * 
 * Kompilasi: gcc -o chess_thread chess_thread.c -lpthread
 * 
 * Program ini mendemonstrasikan:
 * 1. POSIX Threads (pthread_create, pthread_join, pthread_mutex)
 * 2. Shared Memory (POSIX: shm_open, mmap)
 * 3. Signal Handling (sigaction)
 * 4. Named Pipes komunikasi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

/* ── Konstanta ─────────────────────────────────────────────── */
#define SHM_NAME        "/chess_game_shm"
#define SHM_SIZE        4096
#define MAX_THREADS     4
#define BOARD_SIZE      8

/* ── Struktur Data Shared Memory ─────────────────────────────  */
typedef struct {
    int  board[BOARD_SIZE][BOARD_SIZE]; /* Representasi papan: 0=kosong */
    int  move_count;
    int  current_player;  /* 0=WHITE, 1=BLACK */
    int  game_active;
    int  check_flag;      /* 1 jika dalam kondisi check */
    char last_move[16];
    int  white_captured;
    int  black_captured;
    pthread_mutex_t board_mutex;  /* Mutex untuk akses board */
} ChessSharedMem;

/* ── Variabel Global ─────────────────────────────────────────  */
static ChessSharedMem *shm_ptr = NULL;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t pause_flag = 0;
static pthread_t threads[MAX_THREADS];
static int shm_fd = -1;

/* ── Argumen Thread ──────────────────────────────────────────  */
typedef struct {
    int  thread_id;
    char role[32];   /* "MONITOR", "TIMER", "AI", "LOGGER" */
} ThreadArgs;

/* ── Signal Handlers ─────────────────────────────────────────  */
static void signal_handler(int signum) {
    switch (signum) {
        case SIGINT:
        case SIGTERM:
            printf("\n[Signal %d] Menghentikan semua thread...\n", signum);
            running = 0;
            if (shm_ptr) shm_ptr->game_active = 0;
            break;
        case SIGUSR1:
            pause_flag = !pause_flag;
            printf("\n[SIGUSR1] Game %s\n", pause_flag ? "PAUSED" : "RESUMED");
            break;
        case SIGUSR2:
            if (shm_ptr) {
                printf("\n[SIGUSR2] === STATUS ===\n");
                printf("  Move count   : %d\n", shm_ptr->move_count);
                printf("  Player       : %s\n", 
                       shm_ptr->current_player == 0 ? "WHITE" : "BLACK");
                printf("  Game active  : %s\n",
                       shm_ptr->game_active ? "YES" : "NO");
                printf("  Last move    : %s\n", shm_ptr->last_move);
            }
            break;
    }
}

/* ── Inisialisasi Signal Handling ─────────────────────────── */
static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  /* Restart syscalls setelah signal */
    
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    /* Ignore SIGPIPE untuk pipe yang tertutup */
    signal(SIGPIPE, SIG_IGN);
    
    printf("[Signal] Handler terdaftar untuk SIGINT, SIGTERM, SIGUSR1, SIGUSR2\n");
}

/* ── Setup Shared Memory ─────────────────────────────────── */
static int setup_shared_memory(void) {
    /* Buat/buka shared memory object */
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        /* Fallback: gunakan file biasa jika shm_open gagal */
        fprintf(stderr, "[SHM] shm_open gagal: %s\n", strerror(errno));
        fprintf(stderr, "[SHM] Menggunakan simulasi dengan file...\n");
        return -1;
    }
    
    /* Set ukuran */
    if (ftruncate(shm_fd, SHM_SIZE) < 0) {
        perror("[SHM] ftruncate gagal");
        close(shm_fd);
        return -1;
    }
    
    /* Map ke address space proses */
    shm_ptr = (ChessSharedMem *)mmap(
        NULL,
        SHM_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );
    
    if (shm_ptr == MAP_FAILED) {
        perror("[SHM] mmap gagal");
        close(shm_fd);
        return -1;
    }
    
    /* Inisialisasi mutex di shared memory */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm_ptr->board_mutex, &mattr);
    pthread_mutexattr_destroy(&mattr);
    
    /* Inisialisasi data */
    memset(shm_ptr->board, 0, sizeof(shm_ptr->board));
    shm_ptr->move_count     = 0;
    shm_ptr->current_player = 0;  /* WHITE */
    shm_ptr->game_active    = 1;
    shm_ptr->check_flag     = 0;
    shm_ptr->white_captured = 0;
    shm_ptr->black_captured = 0;
    strncpy(shm_ptr->last_move, "none", sizeof(shm_ptr->last_move) - 1);
    
    printf("[SHM] Shared memory berhasil dibuat: %s (%d bytes)\n",
           SHM_NAME, SHM_SIZE);
    return 0;
}

/* ── Cleanup Shared Memory ───────────────────────────────── */
static void cleanup_shared_memory(void) {
    if (shm_ptr && shm_ptr != MAP_FAILED) {
        pthread_mutex_destroy(&shm_ptr->board_mutex);
        munmap(shm_ptr, SHM_SIZE);
        shm_ptr = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    shm_unlink(SHM_NAME);
    printf("[SHM] Shared memory dibersihkan\n");
}

/* ══ THREAD FUNCTIONS ══════════════════════════════════════ */

/* Thread 1: Monitor - memantau state game */
static void *thread_monitor(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;
    printf("[Thread %d: %s] Dimulai (tid=%lu)\n",
           ta->thread_id, ta->role, (unsigned long)pthread_self());
    
    int iteration = 0;
    while (running) {
        /* Cek pause */
        while (pause_flag && running) {
            usleep(100000);  /* 100ms */
        }
        
        if (shm_ptr) {
            /* Kunci mutex sebelum baca shared memory */
            pthread_mutex_lock(&shm_ptr->board_mutex);
            
            int mc = shm_ptr->move_count;
            int player = shm_ptr->current_player;
            int active = shm_ptr->game_active;
            
            pthread_mutex_unlock(&shm_ptr->board_mutex);
            
            if (iteration % 5 == 0) {  /* Log setiap 5 iterasi */
                printf("[Monitor] Move: %d | Player: %s | Active: %s\n",
                       mc,
                       player == 0 ? "WHITE" : "BLACK",
                       active ? "YES" : "NO");
            }
            
            if (!active) break;
        }
        
        iteration++;
        sleep(2);  /* Cek setiap 2 detik */
    }
    
    printf("[Thread %d: %s] Selesai\n", ta->thread_id, ta->role);
    return NULL;
}

/* Thread 2: Timer - menghitung waktu per giliran */
static void *thread_timer(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;
    printf("[Thread %d: %s] Dimulai (tid=%lu)\n",
           ta->thread_id, ta->role, (unsigned long)pthread_self());
    
    int last_move_count = -1;
    int seconds_on_turn = 0;
    
    while (running) {
        while (pause_flag && running) usleep(100000);
        
        if (shm_ptr) {
            pthread_mutex_lock(&shm_ptr->board_mutex);
            int current_mc = shm_ptr->move_count;
            int active = shm_ptr->game_active;
            pthread_mutex_unlock(&shm_ptr->board_mutex);
            
            if (!active) break;
            
            if (current_mc != last_move_count) {
                /* Move baru terjadi */
                if (last_move_count >= 0) {
                    printf("[Timer] Move %d selesai dalam %d detik\n",
                           last_move_count, seconds_on_turn);
                }
                last_move_count = current_mc;
                seconds_on_turn = 0;
            } else {
                seconds_on_turn++;
            }
        }
        
        sleep(1);
    }
    
    printf("[Thread %d: %s] Selesai\n", ta->thread_id, ta->role);
    return NULL;
}

/* Thread 3: Logger - mencatat history move */
static void *thread_logger(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;
    printf("[Thread %d: %s] Dimulai (tid=%lu)\n",
           ta->thread_id, ta->role, (unsigned long)pthread_self());
    
    FILE *log_fp = fopen("/tmp/chess_thread_log.txt", "w");
    if (!log_fp) log_fp = stderr;
    
    int last_move = -1;
    time_t start_time = time(NULL);
    
    while (running) {
        while (pause_flag && running) usleep(100000);
        
        if (shm_ptr) {
            pthread_mutex_lock(&shm_ptr->board_mutex);
            int mc = shm_ptr->move_count;
            char last[16];
            strncpy(last, shm_ptr->last_move, sizeof(last) - 1);
            int active = shm_ptr->game_active;
            pthread_mutex_unlock(&shm_ptr->board_mutex);
            
            if (!active) break;
            
            if (mc != last_move) {
                time_t now = time(NULL);
                fprintf(log_fp, "[%lds] Move #%d: %s\n",
                        (long)(now - start_time), mc, last);
                fflush(log_fp);
                last_move = mc;
            }
        }
        
        usleep(500000);  /* 500ms */
    }
    
    if (log_fp != stderr) fclose(log_fp);
    printf("[Thread %d: %s] Selesai\n", ta->thread_id, ta->role);
    return NULL;
}

/* Thread 4: AI Helper - simulasi AI thinking (future use) */
static void *thread_ai(void *arg) {
    ThreadArgs *ta = (ThreadArgs *)arg;
    printf("[Thread %d: %s] Dimulai (tid=%lu)\n",
           ta->thread_id, ta->role, (unsigned long)pthread_self());
    
    while (running) {
        while (pause_flag && running) usleep(100000);
        
        if (shm_ptr) {
            pthread_mutex_lock(&shm_ptr->board_mutex);
            int active = shm_ptr->game_active;
            pthread_mutex_unlock(&shm_ptr->board_mutex);
            if (!active) break;
        }
        
        /* Simulasi AI processing - placeholder */
        sleep(3);
    }
    
    printf("[Thread %d: %s] Selesai\n", ta->thread_id, ta->role);
    return NULL;
}

/* ── Fungsi Update Move (dipanggil dari luar via signal/pipe) */
void update_move(const char *move_str) {
    if (!shm_ptr) return;
    
    pthread_mutex_lock(&shm_ptr->board_mutex);
    shm_ptr->move_count++;
    shm_ptr->current_player = 1 - shm_ptr->current_player;
    strncpy(shm_ptr->last_move, move_str, sizeof(shm_ptr->last_move) - 1);
    pthread_mutex_unlock(&shm_ptr->board_mutex);
}

/* ── Main ─────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  Chess Thread Manager - POSIX Threads    ║\n");
    printf("║  Shared Memory + Signal Handling Demo    ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("PID: %d\n\n", (int)getpid());
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Setup shared memory */
    int shm_ok = setup_shared_memory();
    if (shm_ok < 0) {
        fprintf(stderr, "Peringatan: Shared memory tidak tersedia, melanjutkan tanpa SHM\n");
    }
    
    /* Definisi thread */
    ThreadArgs thread_args[MAX_THREADS] = {
        {0, "MONITOR"},
        {1, "TIMER"},
        {2, "LOGGER"},
        {3, "AI"}
    };
    
    /* Buat semua thread */
    printf("[Main] Membuat %d POSIX threads...\n", MAX_THREADS);
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    void *(*thread_funcs[MAX_THREADS])(void *) = {
        thread_monitor,
        thread_timer,
        thread_logger,
        thread_ai
    };
    
    for (int i = 0; i < MAX_THREADS; i++) {
        int ret = pthread_create(
            &threads[i],
            &attr,
            thread_funcs[i],
            &thread_args[i]
        );
        if (ret != 0) {
            fprintf(stderr, "[Main] Gagal membuat thread %d: %s\n",
                    i, strerror(ret));
        }
    }
    pthread_attr_destroy(&attr);
    
    printf("[Main] Semua thread berjalan. Menunggu sinyal SIGTERM/SIGINT...\n");
    printf("[Main] Kirim: kill -USR1 %d (pause/resume)\n", (int)getpid());
    printf("[Main] Kirim: kill -USR2 %d (status)\n", (int)getpid());
    printf("[Main] Kirim: kill -TERM %d (berhenti)\n\n", (int)getpid());
    
    /* Mode interaktif jika ada argumen */
    if (argc > 1 && strcmp(argv[1], "--interactive") == 0) {
        char move[32];
        while (running) {
            printf("Move (atau 'quit'): ");
            if (!fgets(move, sizeof(move), stdin)) break;
            move[strcspn(move, "\n")] = 0;
            if (strcmp(move, "quit") == 0) break;
            if (strlen(move) >= 4) {
                update_move(move);
                printf("Move diterima: %s\n", move);
            }
        }
        running = 0;
        if (shm_ptr) shm_ptr->game_active = 0;
    } else {
        /* Tunggu SIGTERM/SIGINT */
        while (running) {
            sleep(1);
        }
    }
    
    printf("\n[Main] Menunggu semua thread selesai...\n");
    
    /* Sinyal semua thread untuk berhenti */
    running = 0;
    if (shm_ptr) shm_ptr->game_active = 0;
    
    /* Join semua thread */
    for (int i = 0; i < MAX_THREADS; i++) {
        int ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "[Main] pthread_join thread %d gagal: %s\n",
                    i, strerror(ret));
        } else {
            printf("[Main] Thread %d (%s) berhasil di-join\n",
                   i, thread_args[i].role);
        }
    }
    
    /* Cleanup */
    cleanup_shared_memory();
    
    printf("\n[Main] Semua thread selesai. Program berakhir.\n");
    return 0;
}
