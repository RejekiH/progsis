/*
 * ============================================================
 * chess_shm.c — Game Catur Linux
 * Fitur IPC Lengkap (Sesuai Modul PrakW12S02):
 *
 *   1. Shared Memory (shm_open + mmap) → board[8][8] array
 *   2. POSIX Semaphore                 → sinkronisasi giliran
 *   3. Message Queue (System V IPC)    → notifikasi & chat
 *      - msgget()  : membuat/mengakses message queue
 *      - msgsnd()  : mengirim pesan ke queue
 *      - msgrcv()  : menerima pesan dari queue
 *      - msgctl()  : mengelola dan menghapus queue
 *      - Tipe pesan berbeda per client (Tugas 2 modul)
 *      - Dialog dua arah antar proses (Tugas 1 modul)
 *   4. POSIX Thread                    → receiver MQ background
 *
 * Kompilasi:
 *   gcc -Wall -std=c99 -D_GNU_SOURCE -o chess_shm chess_shm.c -lpthread -lrt
 *
 * Jalankan (3 terminal):
 *   ./chess_shm server   ← Terminal 1
 *   ./chess_shm white    ← Terminal 2 (Layer 1)
 *   ./chess_shm black    ← Terminal 3 (Layer 2)
 *
 * Monitor Message Queue (sesuai modul):
 *   ipcs -q              ← Lihat status message queue
 *   ipcrm -q <msqid>     ← Hapus message queue manual
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>        /* System V Message Queue — sesuai modul */

/* ═══════════════════════════════════════════════════════════
   1. KONSTANTA & NAMA RESOURCE IPC
   ═══════════════════════════════════════════════════════════ */
#define SHM_NAME      "/chess_shm_v2"
#define SEM_WHITE     "/chess_sem_w"
#define SEM_BLACK     "/chess_sem_b"
#define SEM_MUTEX     "/chess_sem_mx"
#define MQ_KEY_PATH   "/tmp"
#define MQ_KEY_ID     'Z'
#define MAX_TEXT      512     /* Sesuai modul: #define MAX_TEXT 512 */

/*
 * === TIPE PESAN MESSAGE QUEUE (Tugas 2 Modul) ===
 *
 * Sesuai modul: "Gunakan tipe message yang berbeda untuk setiap client"
 *
 * Server dapat berkomunikasi secara private dengan masing-masing client
 * menggunakan tipe yang berbeda pada satu message queue yang sama.
 *
 * MTYPE_WHITE  = 1L  → Hanya dibaca oleh Pemain WHITE
 * MTYPE_BLACK  = 2L  → Hanya dibaca oleh Pemain BLACK
 * MTYPE_SERVER = 3L  → Hanya dibaca oleh Server (log & monitoring)
 * MTYPE_BCAST  = 4L  → Broadcast ke semua (dibaca siapa saja)
 *
 * Mekanisme privasi:
 *   msgsnd(mq_id, &msg, sz, 0)           → server kirim ke MTYPE_WHITE
 *   msgrcv(mq_id, &msg, sz, MTYPE_WHITE, 0) → hanya WHITE yang terima
 */
#define MTYPE_WHITE    1L   /* → Pesan privat untuk Pemain WHITE  */
#define MTYPE_BLACK    2L   /* → Pesan privat untuk Pemain BLACK  */
#define MTYPE_SERVER   3L   /* → Pesan ke Server (log/monitor)    */
#define MTYPE_BCAST    4L   /* → Broadcast semua pemain           */

/* Jenis event dalam pesan MQ */
typedef enum {
    EVT_YOUR_TURN  = 10,  /* giliran Anda dimulai          */
    EVT_OPP_MOVED  = 20,  /* lawan sudah bergerak          */
    EVT_CHECK      = 30,  /* raja Anda dalam skak          */
    EVT_CHECKMATE  = 40,  /* skakmat — game selesai        */
    EVT_GAMEOVER   = 50,  /* game selesai (resign/draw)    */
    EVT_CHAT       = 60,  /* pesan chat dari lawan         */
    EVT_OFFER_DRAW = 70,  /* lawan menawarkan seri         */
    EVT_ACCEPT_DRAW= 71,  /* lawan menerima tawaran seri   */
    EVT_MOVE_ERROR = 80,  /* gerakan tidak valid (feedback)*/
    EVT_MOVE_OK    = 90,  /* gerakan berhasil (konfirmasi) */
    EVT_CONNECTED  = 100, /* client baru terhubung         */
    EVT_DISCONNECT = 110, /* client terputus               */
    EVT_SERVER_LOG = 120, /* log dari server               */
    EVT_DIALOG     = 130, /* pesan dialog (Tugas 1 modul)  */
} EventType;

/*
 * === STRUKTUR PESAN MESSAGE QUEUE ===
 *
 * Sesuai modul, struktur pesan MQ harus dimulai dengan `long int my_msg_type`
 * sebagai field pertama (mandatory untuk msgsnd/msgrcv System V).
 *
 * Referensi modul:
 *   struct my_msg_st {
 *       long int my_msg_type;
 *       char some_text[MAX_TEXT];
 *   };
 */
typedef struct {
    long      mtype;              /* WAJIB field pertama (sesuai modul) */
    EventType event;              /* jenis event                         */
    char      notation[8];       /* notasi gerakan: "e2e4\0"            */
    char      text[MAX_TEXT];    /* teks bebas (sesuai MAX_TEXT modul)  */
    int       extra;             /* data tambahan                        */
    time_t    ts;                /* timestamp                            */
} ChessMsg;

/* ═══════════════════════════════════════════════════════════
   2. REPRESENTASI BIDAK
   ═══════════════════════════════════════════════════════════ */
#define EMPTY    0
#define W_PAWN   1
#define W_ROOK   2
#define W_KNIGHT 3
#define W_BISHOP 4
#define W_QUEEN  5
#define W_KING   6
#define B_PAWN  -1
#define B_ROOK  -2
#define B_KNIGHT -3
#define B_BISHOP -4
#define B_QUEEN -5
#define B_KING  -6

/* ═══════════════════════════════════════════════════════════
   3. STRUKTUR SHARED MEMORY — board[8][8]
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    int    board[8][8];
    int    move_count;
    int    current_player;    /* 1=WHITE, -1=BLACK */
    int    game_active;
    int    in_check;
    int    winner;
    int    white_connected;
    int    black_connected;
    int    white_captured[16];
    int    white_cap_count;
    int    black_captured[16];
    int    black_cap_count;
    char   last_notation[8];
    time_t last_move_time;
    int    draw_offered_by;
    int    mq_id_stored;      /* MQ ID disimpan di SHM agar client bisa baca */
} ChessBoard;

/* ═══════════════════════════════════════════════════════════
   VARIABEL GLOBAL
   ═══════════════════════════════════════════════════════════ */
static ChessBoard *shm       = NULL;
static sem_t      *sem_white = NULL;
static sem_t      *sem_black = NULL;
static sem_t      *sem_mutex = NULL;
static int         shm_fd    = -1;
static int         mq_id     = -1;
static volatile sig_atomic_t g_running = 1;

/* ═══════════════════════════════════════════════════════════
   SIGNAL HANDLER
   ═══════════════════════════════════════════════════════════ */
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (shm) shm->game_active = 0;
    if (sem_white) sem_post(sem_white);
    if (sem_black) sem_post(sem_black);
}

/* ═══════════════════════════════════════════════════════════
   SHARED MEMORY — setup & teardown
   ═══════════════════════════════════════════════════════════ */
static int shm_setup_server(void) {
    shm_unlink(SHM_NAME);
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open server"); return -1; }
    if (ftruncate(shm_fd, sizeof(ChessBoard)) < 0) {
        perror("ftruncate"); return -1;
    }
    shm = (ChessBoard *)mmap(NULL, sizeof(ChessBoard),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap server"); return -1; }
    return 0;
}

static int shm_setup_client(void) {
    for (int i = 0; i < 20; i++) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd >= 0) break;
        printf("  [SHM] Menunggu server (%d/20)...\n", i+1);
        sleep(1);
    }
    if (shm_fd < 0) { perror("shm_open client"); return -1; }
    shm = (ChessBoard *)mmap(NULL, sizeof(ChessBoard),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap client"); return -1; }
    return 0;
}

static void shm_destroy(void) {
    if (shm && shm != MAP_FAILED) munmap(shm, sizeof(ChessBoard));
    if (shm_fd >= 0) close(shm_fd);
    shm_unlink(SHM_NAME);
}

/* ═══════════════════════════════════════════════════════════
   MESSAGE QUEUE (System V IPC) — sesuai modul PrakW12S02
   ═══════════════════════════════════════════════════════════

   Fungsi-fungsi di bawah ini mengimplementasikan operasi
   message queue sesuai definisi fungsi dari modul:

     #include <sys/msg.h>
     int msgctl(int msqid, int cmd, struct msqid_ds *buf);
     int msgget(key_t key, int msgflg);
     int msgrcv(int msqid, void *msg_ptr, size_t msg_sz,
                long int msgtype, int msgflg);
     int msgsnd(int msqid, const void *msg_ptr, size_t msg_sz,
                int msgflg);
*/

/*
 * mq_setup_server() — Membuat message queue baru
 * Menggunakan msgget() dengan IPC_CREAT | IPC_EXCL
 * sesuai contoh modul.
 */
static int mq_setup_server(void) {
    /*
     * ftok() menghasilkan key_t unik dari path + id
     * Ini standar cara generate key untuk System V IPC
     */
    key_t key = ftok(MQ_KEY_PATH, MQ_KEY_ID);
    if (key == -1) { perror("ftok"); return -1; }

    /* Hapus queue lama jika tertinggal (seperti ipcrm di modul) */
    int old = msgget(key, 0666);
    if (old >= 0) {
        msgctl(old, IPC_RMID, NULL);
        printf("  [MQ] Queue lama dihapus (cleanup)\n");
    }

    /*
     * msgget(key, IPC_CREAT | IPC_EXCL | 0666)
     * Membuat queue baru. Persis seperti contoh di modul:
     *   msgid = msgget((key_t)1234, 0666 | IPC_CREAT);
     */
    mq_id = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    if (mq_id < 0) { perror("msgget server"); return -1; }
    return 0;
}

/*
 * mq_setup_client() — Membuka queue yang sudah dibuat server
 * Client menggunakan msgget() dengan key yang sama untuk
 * mendapatkan ID queue yang sudah ada.
 */
static int mq_setup_client(void) {
    for (int i = 0; i < 20; i++) {
        key_t key = ftok(MQ_KEY_PATH, MQ_KEY_ID);
        if (key != -1) {
            mq_id = msgget(key, 0666);
            if (mq_id >= 0) return 0;
        }
        printf("  [MQ] Menunggu message queue dari server (%d/20)...\n", i+1);
        sleep(1);
    }
    perror("msgget client");
    return -1;
}

/*
 * mq_destroy() — Menghapus message queue
 * Menggunakan msgctl(mq_id, IPC_RMID, 0) sesuai modul:
 *   "Untuk menghapus message queue dengan msgid 32768
 *    dapat menggunakan command: ipcrm -q <id>"
 *   Atau secara programatik: msgctl(msgid, IPC_RMID, 0)
 */
static void mq_destroy(void) {
    if (mq_id >= 0) {
        /*
         * msgctl dengan IPC_RMID menghapus queue dari sistem
         * Setara dengan: ipcrm -q <mq_id> di command line
         */
        if (msgctl(mq_id, IPC_RMID, NULL) == 0) {
            printf("  [MQ] Message queue ID=%d berhasil dihapus\n", mq_id);
        } else {
            perror("  [MQ] msgctl IPC_RMID gagal");
        }
        mq_id = -1;
    }
}

/*
 * mq_send() — Kirim pesan ke queue
 *
 * Menggunakan msgsnd() sesuai modul:
 *   msgsnd(msgid, (void *)&some_data, MAX_TEXT, 0)
 *
 * Parameter mtype menentukan penerima pesan (privasi per client):
 *   MTYPE_WHITE  → hanya WHITE yang bisa terima
 *   MTYPE_BLACK  → hanya BLACK yang bisa terima
 *   MTYPE_SERVER → hanya server yang bisa terima
 *
 * Ini implementasi Tugas 2 modul:
 *   "server dapat berkomunikasi secara private dengan
 *    masing-masing individu client via message queue tunggal"
 */
static void mq_send(long mtype, EventType evt,
                    const char *notif, const char *text, int extra) {
    if (mq_id < 0) return;

    ChessMsg m;
    memset(&m, 0, sizeof(m));
    m.mtype = mtype;      /* Tipe penerima — kunci privasi */
    m.event = evt;
    m.extra = extra;
    m.ts    = time(NULL);
    if (notif) strncpy(m.notation, notif, 7);
    if (text)  strncpy(m.text,     text,  MAX_TEXT - 1);

    /*
     * msgsnd() — kirim pesan ke queue
     * Ukuran pesan = sizeof(ChessMsg) - sizeof(long)
     * (tidak termasuk field mtype yang sudah diset terpisah)
     * Flag IPC_NOWAIT agar tidak blocking jika queue penuh
     */
    if (msgsnd(mq_id, &m, sizeof(m) - sizeof(long), IPC_NOWAIT) < 0) {
        if (errno != EAGAIN && errno != EINTR)
            perror("[MQ] msgsnd gagal");
    }
}

/*
 * mq_recv_block() — Terima pesan secara blocking
 *
 * Menggunakan msgrcv() sesuai modul:
 *   msgrcv(msgid, (void *)&some_data, BUFSIZ, msg_to_receive, 0)
 *
 * Flag 0 = blocking: proses akan berhenti hingga pesan dengan
 * mtype yang diminta tersedia di queue.
 *
 * Parameter mtype diisi MTYPE_WHITE atau MTYPE_BLACK sehingga
 * setiap client hanya menerima pesannya sendiri (privasi).
 */
static int mq_recv_block(long mtype, ChessMsg *out) {
    if (mq_id < 0) return -1;
    /*
     * msgrcv dengan flag 0 = blocking
     * Proses tidur hingga ada pesan dengan tipe 'mtype'
     */
    ssize_t r = msgrcv(mq_id, out,
                       sizeof(ChessMsg) - sizeof(long),
                       mtype, 0);
    return (r < 0) ? -1 : 0;
}

/*
 * mq_recv_nowait() — Terima pesan non-blocking
 *
 * Menggunakan msgrcv() dengan flag IPC_NOWAIT.
 * Jika tidak ada pesan, langsung return -1 (EAGAIN).
 * Digunakan untuk polling di server monitor thread.
 */
static int mq_recv_nowait(long mtype, ChessMsg *out) {
    if (mq_id < 0) return -1;
    ssize_t r = msgrcv(mq_id, out,
                       sizeof(ChessMsg) - sizeof(long),
                       mtype, IPC_NOWAIT);
    return (r < 0) ? -1 : 0;
}

/*
 * mq_print_status() — Tampilkan status queue seperti 'ipcs -q'
 *
 * Menggunakan msgctl() dengan IPC_STAT untuk mengambil info queue.
 * Sesuai modul bagian C.4.c:
 *   "Untuk melihat informasi message queue dapat menggunakan
 *    command ipcs -q"
 *
 * Output format menyerupai output ipcs -q:
 *   key  msqid  owner  perms  used-bytes  messages
 */
static void mq_print_status(void) {
    if (mq_id < 0) return;

    struct msqid_ds info;
    /*
     * msgctl dengan IPC_STAT mengisi struct msqid_ds
     * berisi informasi lengkap tentang queue
     */
    if (msgctl(mq_id, IPC_STAT, &info) == 0) {
        printf("\n  ┌─────────────────────────────────────────────────────┐\n");
        printf("  │  STATUS MESSAGE QUEUE (setara: ipcs -q)             │\n");
        printf("  ├──────────┬──────────┬────────┬────────────┬─────────┤\n");
        printf("  │ %-8s │ %-8s │ %-6s │ %-10s │ %-7s │\n",
               "key", "msqid", "perms", "used-bytes", "messages");
        printf("  ├──────────┼──────────┼────────┼────────────┼─────────┤\n");
        printf("  │ 0x%06lx │ %-8d │ %-6o │ %-10lu │ %-7lu │\n",
               (unsigned long)info.msg_perm.__key,
               mq_id,
               info.msg_perm.mode & 0777,
               (unsigned long)info.msg_cbytes,
               (unsigned long)info.msg_qnum);
        printf("  └──────────┴──────────┴────────┴────────────┴─────────┘\n");
        printf("  Untuk hapus manual: ipcrm -q %d\n\n", mq_id);
    }
}

/* ═══════════════════════════════════════════════════════════
   INISIALISASI PAPAN
   ═══════════════════════════════════════════════════════════ */
static void board_init(void) {
    memset(shm, 0, sizeof(ChessBoard));

    shm->board[0][0] = B_ROOK;   shm->board[0][1] = B_KNIGHT;
    shm->board[0][2] = B_BISHOP; shm->board[0][3] = B_QUEEN;
    shm->board[0][4] = B_KING;   shm->board[0][5] = B_BISHOP;
    shm->board[0][6] = B_KNIGHT; shm->board[0][7] = B_ROOK;
    for (int c = 0; c < 8; c++) shm->board[1][c] = B_PAWN;
    for (int c = 0; c < 8; c++) shm->board[6][c] = W_PAWN;
    shm->board[7][0] = W_ROOK;   shm->board[7][1] = W_KNIGHT;
    shm->board[7][2] = W_BISHOP; shm->board[7][3] = W_QUEEN;
    shm->board[7][4] = W_KING;   shm->board[7][5] = W_BISHOP;
    shm->board[7][6] = W_KNIGHT; shm->board[7][7] = W_ROOK;

    shm->current_player  = 1;
    shm->game_active     = 1;
    shm->last_move_time  = time(NULL);
    shm->mq_id_stored    = -1;
}

/* ═══════════════════════════════════════════════════════════
   TAMPILAN TERMINAL
   ═══════════════════════════════════════════════════════════ */
static const char *psym(int p) {
    switch (p) {
        case W_KING:   return "\033[1;37m♚\033[0m";
        case W_QUEEN:  return "\033[1;37m♛\033[0m";
        case W_ROOK:   return "\033[1;37m♜\033[0m";
        case W_BISHOP: return "\033[1;37m♝\033[0m";
        case W_KNIGHT: return "\033[1;37m♞\033[0m";
        case W_PAWN:   return "\033[1;37m♟\033[0m";
        case B_KING:   return "\033[0;31m♚\033[0m";
        case B_QUEEN:  return "\033[0;31m♛\033[0m";
        case B_ROOK:   return "\033[0;31m♜\033[0m";
        case B_BISHOP: return "\033[0;31m♝\033[0m";
        case B_KNIGHT: return "\033[0;31m♞\033[0m";
        case B_PAWN:   return "\033[0;31m♟\033[0m";
        default:       return " ";
    }
}

typedef struct {
    int  board[8][8];
    int  mc, cur, chk, winner, active, wcc, bcc;
    int  wcap[16], bcap[16];
    char notation[8];
    int  draw_by, mq_id;
} Snap;

static void take_snap(Snap *s) {
    sem_wait(sem_mutex);
    memcpy(s->board, shm->board,          sizeof(s->board));
    memcpy(s->wcap,  shm->white_captured, sizeof(s->wcap));
    memcpy(s->bcap,  shm->black_captured, sizeof(s->bcap));
    s->mc      = shm->move_count;
    s->cur     = shm->current_player;
    s->chk     = shm->in_check;
    s->winner  = shm->winner;
    s->active  = shm->game_active;
    s->wcc     = shm->white_cap_count;
    s->bcc     = shm->black_cap_count;
    s->draw_by = shm->draw_offered_by;
    s->mq_id   = shm->mq_id_stored;
    strncpy(s->notation, shm->last_notation, 7);
    sem_post(sem_mutex);
}

static void render(int my_color, const char *msg) {
    Snap s;
    take_snap(&s);

    printf("\033[2J\033[H");
    printf("\033[1;36m");
    printf("  ╔══════════════════════════════════════════════════════╗\n");
    printf("  ║    ♟  GAME CATUR LINUX — IPC + MSG QUEUE  ♟          ║\n");
    printf("  ║   SHM board[8][8] + Semaphore + MsgQueue + Thread   ║\n");
    printf("  ╚══════════════════════════════════════════════════════╝\033[0m\n\n");

    printf("  \033[2mGiliran :\033[0m \033[1m%s\033[0m%s",
           s.cur > 0 ? "WHITE" : "BLACK",
           s.chk ? "  \033[1;31m⚠ SKAK!\033[0m" : "");
    printf("   Move #%d   MQ_ID:\033[33m%d\033[0m\n", s.mc, mq_id);
    printf("  \033[2mAnda    :\033[0m \033[1m%s\033[0m",
           my_color > 0 ? "WHITE" : "BLACK");
    if (s.notation[0])
        printf("   \033[2mGerakan terakhir:\033[0m \033[33m%s\033[0m", s.notation);
    printf("\n\n");

    printf("  ┌───────────────────────────────────────────────────┐\n");
    printf("  │ \033[1mBidak Ditangkap\033[0m                                    │\n");
    printf("  │ \033[1;37mPutih tangkap:\033[0m ");
    if (s.wcc > 0)
        for (int i = 0; i < s.wcc; i++) printf("%s ", psym(s.wcap[i]));
    else printf("\033[2m(belum ada)\033[0m");
    printf("\n");
    printf("  │ \033[0;31mHitam tangkap:\033[0m ");
    if (s.bcc > 0)
        for (int i = 0; i < s.bcc; i++) printf("%s ", psym(s.bcap[i]));
    else printf("\033[2m(belum ada)\033[0m");
    printf("\n");
    printf("  └───────────────────────────────────────────────────┘\n\n");

    printf("       a   b   c   d   e   f   g   h\n");
    printf("     ┌───┬───┬───┬───┬───┬───┬───┬───┐\n");

    int rs = 0, re = 8, step = 1;
    if (my_color < 0) { rs = 7; re = -1; step = -1; }

    for (int r = rs; r != re; r += step) {
        int rank = 8 - r;
        printf("   %d │", rank);
        for (int c = 0; c < 8; c++) {
            int p    = s.board[r][c];
            int dark = (r + c) % 2;
            if (p == EMPTY)
                printf("%s   \033[0m│", dark ? "\033[2m" : "");
            else
                printf("%s %s \033[0m│", dark ? "\033[2m" : "", psym(p));
        }
        printf(" %d\n", rank);
        if (step ==  1 && r < 7) printf("     ├───┼───┼───┼───┼───┼───┼───┼───┤\n");
        if (step == -1 && r > 0) printf("     ├───┼───┼───┼───┼───┼───┼───┼───┤\n");
    }
    printf("     └───┴───┴───┴───┴───┴───┴───┴───┘\n");
    printf("       a   b   c   d   e   f   g   h\n\n");

    if (s.draw_by != 0 && s.draw_by != my_color)
        printf("  \033[1;33m🤝 Lawan menawarkan SERI! Ketik 'draw' untuk terima.\033[0m\n");

    if (msg && msg[0])
        printf("  %s\n", msg);

    printf("\n");
    printf("  \033[2mPerintah: [gerakan] e2e4 | chat:<pesan> | draw | resign | quit\033[0m\n");
    printf("  \033[2mMonitor MQ: ipcs -q (di terminal lain)   Hapus: ipcrm -q %d\033[0m\n\n", mq_id);
}

/* ═══════════════════════════════════════════════════════════
   VALIDASI & EKSEKUSI MOVE
   ═══════════════════════════════════════════════════════════ */
static int parse_nota(const char *s, int *fr, int *fc, int *tr, int *tc) {
    if (!s || strlen(s) < 4) return 0;
    if (s[0]<'a'||s[0]>'h'||s[2]<'a'||s[2]>'h') return 0;
    if (s[1]<'1'||s[1]>'8'||s[3]<'1'||s[3]>'8') return 0;
    *fc = s[0]-'a';  *fr = 8-(s[1]-'0');
    *tc = s[2]-'a';  *tr = 8-(s[3]-'0');
    return 1;
}

static const char *do_move(int player, int fr, int fc, int tr, int tc,
                            char *out_nota) {
    int p    = shm->board[fr][fc];
    int dest = shm->board[tr][tc];
    if (p == EMPTY) return "EMPTY_SOURCE";
    int pc = (p > 0) ? 1 : -1;
    if (pc != player) return "WRONG_COLOR";
    if (dest != EMPTY && ((dest>0)==(p>0))) return "DEST_OCCUPIED";

    int lp = abs(p), dr = tr-fr, dc = tc-fc;

    if (lp == 1) {
        if (player == 1) {
            if (dc==0&&dr==-1&&dest==EMPTY) {}
            else if (dc==0&&dr==-2&&fr==6&&dest==EMPTY&&shm->board[fr-1][fc]==EMPTY) {}
            else if (abs(dc)==1&&dr==-1&&dest!=EMPTY&&dest<0) {}
            else return "INVALID_PAWN";
        } else {
            if (dc==0&&dr==1&&dest==EMPTY) {}
            else if (dc==0&&dr==2&&fr==1&&dest==EMPTY&&shm->board[fr+1][fc]==EMPTY) {}
            else if (abs(dc)==1&&dr==1&&dest!=EMPTY&&dest>0) {}
            else return "INVALID_PAWN";
        }
    } else if (lp == 2) {
        if (fr!=tr && fc!=tc) return "INVALID_ROOK";
        int ddr=(tr>fr)?1:(tr<fr)?-1:0, ddc=(tc>fc)?1:(tc<fc)?-1:0;
        for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
            if (shm->board[r][c]!=EMPTY) return "ROOK_BLOCKED";
    } else if (lp == 4) {
        if (abs(dr)!=abs(dc)) return "INVALID_BISHOP";
        int ddr=(dr>0)?1:-1, ddc=(dc>0)?1:-1;
        for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
            if (shm->board[r][c]!=EMPTY) return "BISHOP_BLOCKED";
    } else if (lp == 3) {
        if (!((abs(dr)==2&&abs(dc)==1)||(abs(dr)==1&&abs(dc)==2)))
            return "INVALID_KNIGHT";
    } else if (lp == 5) {
        if (fr==tr||fc==tc) {
            int ddr=(tr>fr)?1:(tr<fr)?-1:0, ddc=(tc>fc)?1:(tc<fc)?-1:0;
            for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
                if (shm->board[r][c]!=EMPTY) return "QUEEN_BLOCKED";
        } else if (abs(dr)==abs(dc)) {
            int ddr=(dr>0)?1:-1, ddc=(dc>0)?1:-1;
            for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
                if (shm->board[r][c]!=EMPTY) return "QUEEN_BLOCKED";
        } else return "INVALID_QUEEN";
    } else if (lp == 6) {
        if (abs(dr)>1||abs(dc)>1) return "INVALID_KING";
    }

    if (dest != EMPTY) {
        if (player == 1) shm->white_captured[shm->white_cap_count++] = dest;
        else             shm->black_captured[shm->black_cap_count++] = dest;
        if (abs(dest) == 6) {
            shm->winner      = player;
            shm->game_active = 0;
        }
    }
    shm->board[tr][tc] = p;
    shm->board[fr][fc] = EMPTY;

    if (p == W_PAWN && tr == 0) shm->board[tr][tc] = W_QUEEN;
    if (p == B_PAWN && tr == 7) shm->board[tr][tc] = B_QUEEN;

    shm->move_count++;
    shm->current_player = -player;
    shm->draw_offered_by = 0;
    shm->last_move_time  = time(NULL);

    snprintf(out_nota, 8, "%c%c%c%c",
             'a'+fc, '0'+(8-fr), 'a'+tc, '0'+(8-tr));
    strncpy(shm->last_notation, out_nota, 7);
    return "OK";
}

/* ═══════════════════════════════════════════════════════════
   POSIX THREAD — Receiver Message Queue (background)
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    int   my_color;
    char  buf[256];
    int   has_msg;
    int   game_over;
    pthread_mutex_t lock;
} MQRecvArg;

static void *thread_mq_recv(void *arg) {
    MQRecvArg *a = (MQRecvArg *)arg;
    long mtype = (a->my_color > 0) ? MTYPE_WHITE : MTYPE_BLACK;

    while (g_running) {
        ChessMsg m;
        /*
         * msgrcv blocking — thread tidur sampai ada pesan
         * dengan tipe MTYPE_WHITE atau MTYPE_BLACK (privat)
         */
        if (mq_recv_block(mtype, &m) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!g_running) break;

        char tmp[600] = "";
        switch (m.event) {
            case EVT_YOUR_TURN:
                snprintf(tmp, sizeof(tmp), "\033[32m► Giliran Anda!\033[0m");
                break;
            case EVT_OPP_MOVED:
                snprintf(tmp, sizeof(tmp),
                    "\033[36m← Lawan bermain: \033[1m%s\033[0m", m.notation);
                break;
            case EVT_CHECK:
                snprintf(tmp, sizeof(tmp),
                    "\033[1;31m⚠ SKAK! Raja Anda terancam! (setelah %s)\033[0m",
                    m.notation);
                break;
            case EVT_CHECKMATE:
                snprintf(tmp, sizeof(tmp),
                    "\033[1;31m✗ SKAKMAT! Game selesai. %s\033[0m", m.text);
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_GAMEOVER:
                snprintf(tmp, sizeof(tmp),
                    "\033[1;33m● Game selesai: %s\033[0m", m.text);
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_CHAT:
                snprintf(tmp, sizeof(tmp), "\033[35m💬 Lawan: %s\033[0m", m.text);
                break;
            case EVT_OFFER_DRAW:
                snprintf(tmp, sizeof(tmp),
                    "\033[33m🤝 Lawan menawarkan SERI! Ketik 'draw'.\033[0m");
                break;
            case EVT_ACCEPT_DRAW:
                snprintf(tmp, sizeof(tmp),
                    "\033[33m🤝 Tawaran seri diterima! Game SERI.\033[0m");
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_MOVE_ERROR:
                snprintf(tmp, sizeof(tmp), "\033[31m✗ %s\033[0m", m.text);
                break;
            case EVT_MOVE_OK:
                snprintf(tmp, sizeof(tmp),
                    "\033[32m✓ Gerakan %s berhasil\033[0m", m.notation);
                break;
            case EVT_CONNECTED:
                snprintf(tmp, sizeof(tmp),
                    "\033[2m[Info] %s terhubung ke game\033[0m", m.text);
                break;
            case EVT_DIALOG:
                /* Tugas 1: Dialog dua arah */
                snprintf(tmp, sizeof(tmp),
                    "\033[33m[Dialog] %s\033[0m", m.text);
                break;
            default:
                snprintf(tmp, sizeof(tmp),
                    "\033[2m[MQ evt=%d] %s\033[0m", m.event, m.text);
                break;
        }

        pthread_mutex_lock(&a->lock);
        strncpy(a->buf, tmp, sizeof(a->buf)-1);
        a->has_msg = 1;
        pthread_mutex_unlock(&a->lock);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
   LAYER 1 & 2 — Proses Pemain
   ═══════════════════════════════════════════════════════════ */
static void run_player(int my_color) {
    const char *cn     = (my_color > 0) ? "WHITE" : "BLACK";
    sem_t      *my_sem = (my_color > 0) ? sem_white : sem_black;
    sem_t      *op_sem = (my_color > 0) ? sem_black : sem_white;
    long        my_mt  = (my_color > 0) ? MTYPE_WHITE : MTYPE_BLACK;
    long        op_mt  = (my_color > 0) ? MTYPE_BLACK : MTYPE_WHITE;

    printf("\033[2J\033[H");
    printf("\033[1;32m");
    printf("  ╔═════════════════════════════════════════════════╗\n");
    printf("  ║  Terhubung sebagai %-8s                    ║\n", cn);
    printf("  ║  Message Queue ID  : %-6d                  ║\n", mq_id);
    printf("  ║  Shared Mem        : %-20s     ║\n", SHM_NAME);
    printf("  ╚═════════════════════════════════════════════════╝\033[0m\n\n");
    printf("  \033[1mKomunikasi Message Queue (Modul W12S2):\033[0m\n");
    printf("  Tipe pesan Anda : \033[33mMTYPE=%ld\033[0m (privat untuk %s)\n",
           my_mt, cn);
    printf("  Tipe pesan lawan: \033[33mMTYPE=%ld\033[0m\n\n", op_mt);
    printf("  \033[2mPerintah:\033[0m\n");
    printf("  \033[32me2e4\033[0m          → Gerakan catur\n");
    printf("  \033[32mchat:<pesan>\033[0m  → Kirim pesan privat via MQ\n");
    printf("  \033[32mdraw\033[0m          → Tawarkan / terima seri\n");
    printf("  \033[32mresign\033[0m        → Menyerah\n");
    printf("  \033[32mquit\033[0m          → Keluar\n\n");
    printf("  Monitor MQ: \033[33mipcs -q\033[0m (buka terminal lain)\n\n");
    printf("  Menunggu pemain lain bergabung...\n\n");

    sem_wait(sem_mutex);
    if (my_color > 0) shm->white_connected = 1;
    else              shm->black_connected  = 1;
    sem_post(sem_mutex);

    /*
     * Kirim notifikasi koneksi ke server via MQ
     * msgsnd() dengan mtype=MTYPE_SERVER
     */
    mq_send(MTYPE_SERVER, EVT_CONNECTED, "", cn, my_color);
    /* Beritahu lawan via MQ dengan tipe berbeda (privasi) */
    char info[64];
    snprintf(info, 63, "%s bergabung ke game!", cn);
    mq_send(op_mt, EVT_CONNECTED, "", info, 0);

    /* ── Buat thread MQ receiver ── */
    MQRecvArg targ;
    memset(&targ, 0, sizeof(targ));
    targ.my_color = my_color;
    pthread_mutex_init(&targ.lock, NULL);
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, thread_mq_recv, &targ);

    char last_msg[256] = "";
    snprintf(last_msg, 255, "\033[2mMenunggu giliran pertama...\033[0m");

    /* ── Game loop ── */
    while (g_running) {

        /* SEMAPHORE WAIT — tunggu giliran */
        sem_wait(my_sem);
        if (!g_running || !shm->game_active) break;

        sem_wait(sem_mutex);
        int cur = shm->current_player;
        sem_post(sem_mutex);
        if (cur != my_color) continue;

        pthread_mutex_lock(&targ.lock);
        if (targ.has_msg) {
            strncpy(last_msg, targ.buf, 255);
            targ.has_msg = 0;
        }
        pthread_mutex_unlock(&targ.lock);
        if (targ.game_over) break;

        render(my_color, last_msg);
        last_msg[0] = '\0';

        if (!shm->game_active) break;

        char    inp[MAX_TEXT];
        int     fr, fc, tr, tc;
        const char *err = NULL;

        do {
            printf("  \033[1mGiliran %s\033[0m\n  > ", cn);
            fflush(stdout);

            if (!fgets(inp, sizeof(inp), stdin)) { g_running = 0; break; }
            inp[strcspn(inp, "\n")] = '\0';
            for (int i = 0; inp[i]; i++)
                if (inp[i] >= 'A' && inp[i] <= 'Z') inp[i] += 32;

            /* ── quit ── */
            if (!strcmp(inp,"quit") || !strcmp(inp,"q")) {
                g_running = 0; break;
            }

            /* ── resign ── */
            if (!strcmp(inp, "resign")) {
                sem_wait(sem_mutex);
                shm->winner      = -my_color;
                shm->game_active = 0;
                sem_post(sem_mutex);
                char txt[80];
                snprintf(txt, 79, "%s menyerah! %s MENANG!",
                         cn, my_color > 0 ? "BLACK" : "WHITE");
                mq_send(op_mt,       EVT_GAMEOVER, "", txt, 0);
                mq_send(my_mt,       EVT_GAMEOVER, "", txt, 0);
                mq_send(MTYPE_SERVER, EVT_GAMEOVER, "", txt, 0);
                sem_post(op_sem);
                g_running = 0; break;
            }

            /* ── draw ── */
            if (!strcmp(inp, "draw")) {
                sem_wait(sem_mutex);
                int db = shm->draw_offered_by;
                sem_post(sem_mutex);

                if (db == -my_color) {
                    sem_wait(sem_mutex);
                    shm->game_active = 0;
                    shm->winner      = 0;
                    sem_post(sem_mutex);
                    mq_send(op_mt, EVT_ACCEPT_DRAW, "", "Seri diterima!", 0);
                    mq_send(my_mt, EVT_ACCEPT_DRAW, "", "Seri diterima!", 0);
                    sem_post(op_sem);
                    g_running = 0; break;
                } else {
                    sem_wait(sem_mutex);
                    shm->draw_offered_by = my_color;
                    sem_post(sem_mutex);
                    mq_send(op_mt, EVT_OFFER_DRAW, "", cn, 0);
                    snprintf(last_msg, 255,
                        "\033[33mTawaran seri dikirim. Lanjut bermain...\033[0m");
                    err = "draw_pending"; continue;
                }
            }

            /* ── ipcs — tampilkan status queue ── */
            if (!strcmp(inp, "ipcs")) {
                mq_print_status();
                err = "ipcs"; continue;
            }

            /* ── chat:<pesan> ── */
            if (!strncmp(inp, "chat:", 5)) {
                const char *txt = inp + 5;
                /*
                 * Kirim pesan chat ke lawan via Message Queue
                 * dengan tipe pesan lawan (komunikasi privat)
                 *
                 * Sesuai modul Tugas 1:
                 *   Process 1 → msgsnd ke queue → Process 2 menerima
                 *   Process 2 → msgsnd balasan   → Process 1 menerima
                 */
                mq_send(op_mt,       EVT_CHAT, "", txt, 0);
                mq_send(MTYPE_SERVER, EVT_CHAT, "", txt, 0); /* log ke server */
                snprintf(last_msg, 255,
                    "\033[35m💬 Anda → %s: %s\033[0m", my_color > 0 ? "BLACK" : "WHITE", txt);
                err = "chat"; continue;
            }

            /* ── gerakan catur ── */
            if (!strlen(inp)) { err = "empty"; continue; }

            if (!parse_nota(inp, &fr, &fc, &tr, &tc)) {
                snprintf(last_msg, 255,
                    "\033[31m✗ Format salah! Contoh: e2e4\033[0m");
                err = "fmt"; continue;
            }

            char nota[8] = "";
            sem_wait(sem_mutex);
            err = do_move(my_color, fr, fc, tr, tc, nota);
            int w  = shm->winner;
            int ac = shm->game_active;
            sem_post(sem_mutex);

            if (!strcmp(err, "OK")) {
                if (w != 0 || !ac) {
                    char txt[80];
                    snprintf(txt, 79, "%s MENANG dengan gerakan %s!", cn, nota);
                    mq_send(op_mt,       EVT_GAMEOVER, nota, txt, 0);
                    mq_send(my_mt,       EVT_GAMEOVER, nota, txt, 0);
                    mq_send(MTYPE_SERVER, EVT_GAMEOVER, nota, txt, 0);
                    sem_post(op_sem);
                    g_running = 0; break;
                }

                /*
                 * Beritahu lawan giliran mereka via MQ
                 * msgsnd ke tipe lawan (privat)
                 */
                mq_send(op_mt, EVT_OPP_MOVED, nota, cn, 0);
                mq_send(op_mt, EVT_YOUR_TURN,  nota, "",  0);

                /* Konfirmasi ke diri sendiri via MQ */
                mq_send(my_mt, EVT_MOVE_OK, nota, "", 0);

                snprintf(last_msg, 255,
                    "\033[32m✓ Gerakan \033[1m%s\033[0;32m berhasil\033[0m", nota);

            } else {
                const char *em = err;
                if (!strcmp(err,"EMPTY_SOURCE"))  em="Tidak ada bidak di petak itu!";
                if (!strcmp(err,"WRONG_COLOR"))    em="Itu bidak lawan!";
                if (!strcmp(err,"DEST_OCCUPIED"))  em="Petak tujuan sudah ada bidak Anda!";
                if (!strcmp(err,"INVALID_PAWN"))   em="Pion: maju lurus, makan diagonal!";
                if (!strcmp(err,"ROOK_BLOCKED"))   em="Benteng: terhalang bidak lain!";
                if (!strcmp(err,"BISHOP_BLOCKED")) em="Gajah: terhalang bidak lain!";
                if (!strcmp(err,"INVALID_KNIGHT")) em="Kuda: gerakan L (2+1 kotak)!";
                if (!strcmp(err,"QUEEN_BLOCKED"))  em="Ratu: terhalang bidak lain!";
                if (!strcmp(err,"INVALID_KING"))   em="Raja: maksimal 1 kotak!";
                if (!strcmp(err,"INVALID_ROOK"))   em="Benteng: hanya lurus!";
                if (!strcmp(err,"INVALID_BISHOP")) em="Gajah: hanya diagonal!";
                if (!strcmp(err,"INVALID_QUEEN"))  em="Ratu: lurus atau diagonal!";

                /* Kirim error privat ke diri sendiri via MQ */
                mq_send(my_mt, EVT_MOVE_ERROR, "", em, 0);
                snprintf(last_msg, 255, "\033[31m✗ %s\033[0m", em);
            }

        } while (strcmp(err,"OK")!=0
                 && strcmp(err,"draw_pending")!=0
                 && strcmp(err,"chat")!=0
                 && strcmp(err,"ipcs")!=0
                 && g_running && shm->game_active);

        if (!g_running || !shm->game_active) break;

        sleep(1);
        sem_post(op_sem);
    }

    /* Papan akhir */
    if (shm) {
        Snap s; take_snap(&s);
        render(my_color, last_msg);
        if      (s.winner == my_color)
            printf("\033[1;32m╔══════════════════════╗\n║   SELAMAT MENANG!    ║\n╚══════════════════════╝\033[0m\n");
        else if (s.winner == -my_color)
            printf("\033[1;31m╔══════════════════════╗\n║   ANDA KALAH.        ║\n╚══════════════════════╝\033[0m\n");
        else if (!s.active)
            printf("\033[1;33m╔══════════════════════╗\n║   SERI (DRAW).       ║\n╚══════════════════════╝\033[0m\n");
    }

    g_running = 0;
    pthread_cancel(recv_tid);
    pthread_join(recv_tid, NULL);
    pthread_mutex_destroy(&targ.lock);
    printf("\n\033[33mTerima kasih sudah bermain! [%s]\033[0m\n", cn);
}

/* ═══════════════════════════════════════════════════════════
   TUGAS 1 — Dialog Dua Proses via Message Queue
   ═══════════════════════════════════════════════════════════
 *
 * Implementasi Tugas 1 modul:
 *   a. (Process 1) Mengirimkan pesan "Are you hearing me?"
 *   b. (Process 2) Menerima pesan dan membalas "Loud and Clear"
 *   c. (Process 1) Menerima balasan dan mengirim "I can hear you too"
 *
 * Dijalankan dengan: ./chess_shm dialog1 atau ./chess_shm dialog2
   ═══════════════════════════════════════════════════════════ */
static void run_dialog_process1(void) {
    printf("\033[1;36m");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║    TUGAS 1 MODUL — Dialog Dua Proses (MQ)        ║\n");
    printf("  ║    Process 1: Pengirim Pertama                   ║\n");
    printf("  ╚══════════════════════════════════════════════════╝\033[0m\n\n");

    /*
     * Buat message queue baru
     * msgget() dengan IPC_CREAT sesuai modul
     */
    key_t key = ftok(MQ_KEY_PATH, 'D');
    if (key == -1) { perror("ftok dialog"); return; }

    /* Bersihkan queue lama */
    int old = msgget(key, 0666);
    if (old >= 0) msgctl(old, IPC_RMID, NULL);

    int dialog_mq = msgget(key, IPC_CREAT | 0666);
    if (dialog_mq == -1) {
        fprintf(stderr, "msgget failed with error: %d\n", errno);
        return;
    }

    printf("  [Process 1] Message Queue ID: %d\n", dialog_mq);
    printf("  [Process 1] Jalankan 'dialog2' di terminal lain!\n\n");

    /* ── Langkah a: Kirim pesan pertama ── */
    ChessMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = MTYPE_BLACK;   /* Dikirim ke Process 2 */
    msg.event = EVT_DIALOG;
    strncpy(msg.text, "Are you hearing me?", MAX_TEXT-1);
    msg.ts = time(NULL);

    printf("  [Process 1] Mengirim  → \"Are you hearing me?\"\n");
    if (msgsnd(dialog_mq, &msg, sizeof(msg)-sizeof(long), 0) == -1) {
        fprintf(stderr, "msgsnd failed: %d\n", errno);
        msgctl(dialog_mq, IPC_RMID, NULL);
        return;
    }

    /* ── Langkah c: Tunggu balasan dari Process 2 ── */
    ChessMsg reply;
    printf("  [Process 1] Menunggu balasan dari Process 2...\n");
    if (msgrcv(dialog_mq, &reply, sizeof(reply)-sizeof(long),
               MTYPE_WHITE, 0) == -1) {
        fprintf(stderr, "msgrcv failed: %d\n", errno);
        msgctl(dialog_mq, IPC_RMID, NULL);
        return;
    }
    printf("  [Process 1] Menerima  ← \"%s\"\n", reply.text);

    /* ── Kirim pesan terakhir ── */
    memset(&msg, 0, sizeof(msg));
    msg.mtype = MTYPE_BLACK;
    msg.event = EVT_DIALOG;
    strncpy(msg.text, "I can hear you too", MAX_TEXT-1);
    msg.ts = time(NULL);

    printf("  [Process 1] Mengirim  → \"I can hear you too\"\n");
    if (msgsnd(dialog_mq, &msg, sizeof(msg)-sizeof(long), 0) == -1) {
        fprintf(stderr, "msgsnd failed: %d\n", errno);
    }

    /* Tampilkan status queue sebelum dihapus */
    printf("\n");
    struct msqid_ds info;
    if (msgctl(dialog_mq, IPC_STAT, &info) == 0) {
        printf("  [ipcs -q] msqid=%-6d perms=%o messages=%lu\n",
               dialog_mq,
               info.msg_perm.mode & 0777,
               (unsigned long)info.msg_qnum);
    }

    sleep(1);

    /*
     * Hapus message queue setelah selesai
     * msgctl(msgid, IPC_RMID, 0) sesuai modul
     */
    if (msgctl(dialog_mq, IPC_RMID, 0) == -1) {
        fprintf(stderr, "msgctl(IPC_RMID) failed\n");
    } else {
        printf("  [Process 1] Message queue ID=%d dihapus.\n", dialog_mq);
    }

    printf("\n  \033[32m✓ Dialog selesai!\033[0m\n");
}

static void run_dialog_process2(void) {
    printf("\033[1;35m");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║    TUGAS 1 MODUL — Dialog Dua Proses (MQ)        ║\n");
    printf("  ║    Process 2: Penerima & Pembalas                ║\n");
    printf("  ╚══════════════════════════════════════════════════╝\033[0m\n\n");

    /* Buka queue yang sudah dibuat Process 1 */
    key_t key = ftok(MQ_KEY_PATH, 'D');
    if (key == -1) { perror("ftok dialog"); return; }

    int dialog_mq = -1;
    for (int i = 0; i < 15; i++) {
        dialog_mq = msgget(key, 0666);
        if (dialog_mq >= 0) break;
        printf("  [Process 2] Menunggu Process 1 membuat queue (%d/15)...\n", i+1);
        sleep(1);
    }
    if (dialog_mq == -1) {
        fprintf(stderr, "  Gagal: jalankan dialog1 terlebih dahulu!\n");
        return;
    }

    printf("  [Process 2] Terhubung ke Message Queue ID: %d\n\n", dialog_mq);

    /* ── Langkah b: Terima pesan dari Process 1 ── */
    ChessMsg recv_msg;
    printf("  [Process 2] Menunggu pesan dari Process 1...\n");
    if (msgrcv(dialog_mq, &recv_msg, sizeof(recv_msg)-sizeof(long),
               MTYPE_BLACK, 0) == -1) {
        fprintf(stderr, "msgrcv failed: %d\n", errno);
        return;
    }
    printf("  [Process 2] Menerima  ← \"%s\"\n", recv_msg.text);

    /* ── Kirim balasan ke Process 1 ── */
    ChessMsg reply;
    memset(&reply, 0, sizeof(reply));
    reply.mtype = MTYPE_WHITE;  /* Balasan ke Process 1 */
    reply.event = EVT_DIALOG;
    strncpy(reply.text, "Loud and Clear", MAX_TEXT-1);
    reply.ts = time(NULL);

    printf("  [Process 2] Membalas  → \"Loud and Clear\"\n");
    if (msgsnd(dialog_mq, &reply, sizeof(reply)-sizeof(long), 0) == -1) {
        fprintf(stderr, "msgsnd reply failed: %d\n", errno);
        return;
    }

    /* ── Tunggu pesan terakhir dari Process 1 ── */
    printf("  [Process 2] Menunggu pesan terakhir...\n");
    if (msgrcv(dialog_mq, &recv_msg, sizeof(recv_msg)-sizeof(long),
               MTYPE_BLACK, 0) == -1) {
        fprintf(stderr, "msgrcv final failed: %d\n", errno);
        return;
    }
    printf("  [Process 2] Menerima  ← \"%s\"\n", recv_msg.text);

    printf("\n  \033[35m✓ Dialog selesai! Queue akan dihapus oleh Process 1.\033[0m\n");
}

/* ═══════════════════════════════════════════════════════════
   SERVER — inisialisasi semua resource IPC + monitor
   ═══════════════════════════════════════════════════════════ */
static void *thread_server_mq(void *arg) {
    (void)arg;
    printf("  [MQ-Thread] Mendengarkan semua pesan masuk...\n\n");
    while (g_running) {
        ChessMsg m;
        if (mq_recv_nowait(MTYPE_SERVER, &m) == 0) {
            char ts[20];
            struct tm *t = localtime(&m.ts);
            strftime(ts, 20, "%H:%M:%S", t);
            const char *evname = "UNKNOWN";
            switch (m.event) {
                case EVT_CONNECTED:  evname="CONNECTED";  break;
                case EVT_CHAT:       evname="CHAT";        break;
                case EVT_SERVER_LOG: evname="LOG";         break;
                case EVT_GAMEOVER:   evname="GAMEOVER";    break;
                case EVT_MOVE_OK:    evname="MOVE_OK";     break;
                default: break;
            }
            printf("  [MQ %s] %-10s | \"%s\"\n", ts, evname, m.text);
            fflush(stdout);
        }
        usleep(300000);
    }
    return NULL;
}

static void run_server(void) {
    printf("\033[2J\033[H");
    printf("\033[1;36m");
    printf("  ╔════════════════════════════════════════════════════╗\n");
    printf("  ║   CHESS SERVER — IPC LENGKAP (Modul W12S2)         ║\n");
    printf("  ║   Shared Memory + Semaphore + Message Queue        ║\n");
    printf("  ╚════════════════════════════════════════════════════╝\033[0m\n\n");

    /* ── 1. POSIX Shared Memory ── */
    if (shm_setup_server() < 0) exit(1);
    board_init();
    printf("  \033[32m✓ Shared Memory\033[0m  : %s (%lu bytes)\n",
           SHM_NAME, (unsigned long)sizeof(ChessBoard));

    /* ── 2. POSIX Semaphore ── */
    sem_unlink(SEM_WHITE); sem_unlink(SEM_BLACK); sem_unlink(SEM_MUTEX);
    sem_white = sem_open(SEM_WHITE, O_CREAT, 0666, 1);
    sem_black = sem_open(SEM_BLACK, O_CREAT, 0666, 0);
    sem_mutex = sem_open(SEM_MUTEX, O_CREAT, 0666, 1);
    if (sem_white==SEM_FAILED||sem_black==SEM_FAILED||sem_mutex==SEM_FAILED) {
        perror("sem_open"); exit(1);
    }
    printf("  \033[32m✓ Semaphore POSIX\033[0m: %s (w=1) %s (b=0) %s (mx=1)\n",
           SEM_WHITE, SEM_BLACK, SEM_MUTEX);

    /* ── 3. System V Message Queue ── */
    if (mq_setup_server() < 0) exit(1);
    shm->mq_id_stored = mq_id;

    printf("  \033[32m✓ Message Queue\033[0m  : ID=%d (key via ftok)\n\n", mq_id);

    printf("  \033[1mStruktur Tipe Pesan (Tugas 2 Modul):\033[0m\n");
    printf("  ┌───────────────┬──────┬──────────────────────────────┐\n");
    printf("  │ Tipe Pesan    │ mtype│ Penerima                     │\n");
    printf("  ├───────────────┼──────┼──────────────────────────────┤\n");
    printf("  │ MTYPE_WHITE   │  %ld    │ Hanya Pemain WHITE           │\n", MTYPE_WHITE);
    printf("  │ MTYPE_BLACK   │  %ld    │ Hanya Pemain BLACK           │\n", MTYPE_BLACK);
    printf("  │ MTYPE_SERVER  │  %ld    │ Hanya Server (log)           │\n", MTYPE_SERVER);
    printf("  │ MTYPE_BCAST   │  %ld    │ Broadcast ke semua           │\n", MTYPE_BCAST);
    printf("  └───────────────┴──────┴──────────────────────────────┘\n\n");

    printf("  \033[1mMonitor queue:\033[0m \033[33mipcs -q\033[0m\n");
    printf("  \033[1mHapus manual :\033[0m \033[33mipcrm -q %d\033[0m\n\n", mq_id);

    printf("  \033[1mJalankan client:\033[0m\n");
    printf("  \033[32m./chess_shm white\033[0m   ← Terminal 2 (Pemain WHITE)\n");
    printf("  \033[32m./chess_shm black\033[0m   ← Terminal 3 (Pemain BLACK)\n\n");
    printf("  \033[1mDemo Dialog (Tugas 1):\033[0m\n");
    printf("  \033[32m./chess_shm dialog1\033[0m ← Terminal A (Process 1)\n");
    printf("  \033[32m./chess_shm dialog2\033[0m ← Terminal B (Process 2)\n\n");
    printf("  \033[2mMonitor aktif... (Ctrl+C untuk stop)\033[0m\n");
    printf("  ───────────────────────────────────────────────────────\n");

    pthread_t mq_tid;
    pthread_create(&mq_tid, NULL, thread_server_mq, NULL);

    int last_mc = -1;
    while (g_running) {
        sleep(3);
        if (!shm->game_active) break;

        int mc = shm->move_count;
        if (mc != last_mc) {
            /* Tampilkan status MQ setiap ada move baru */
            mq_print_status();
            printf("  [SHM] Move:%-3d | Giliran:%-6s | WCap:%d BCap:%d\n",
                   mc,
                   shm->current_player > 0 ? "WHITE" : "BLACK",
                   shm->white_cap_count,
                   shm->black_cap_count);
            fflush(stdout);
            last_mc = mc;
        }
    }

    printf("\n  [SERVER] Game selesai. ");
    if (shm->winner > 0)       printf("Pemenang: WHITE\n");
    else if (shm->winner < 0)  printf("Pemenang: BLACK\n");
    else                       printf("Hasil: SERI\n");

    g_running = 0;
    pthread_cancel(mq_tid);
    pthread_join(mq_tid, NULL);

    printf("  [SERVER] Membersihkan semua resource IPC...\n");
    shm_destroy();
    sem_close(sem_white); sem_unlink(SEM_WHITE);
    sem_close(sem_black); sem_unlink(SEM_BLACK);
    sem_close(sem_mutex); sem_unlink(SEM_MUTEX);

    /*
     * Hapus message queue saat server shutdown
     * Ini penting agar tidak meninggalkan resource
     * (seperti yang dibahas di modul: cleanup MQ)
     */
    mq_destroy();
    printf("  [SERVER] Selesai.\n");
}

/* ═══════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (argc < 2) {
        printf("\n");
        printf("  \033[1;36mGame Catur Linux — IPC Lengkap (Modul W12S2)\033[0m\n\n");
        printf("  \033[1mGame Catur (3 terminal):\033[0m\n");
        printf("    %s server  ← Terminal 1: Server + MQ monitor\n", argv[0]);
        printf("    %s white   ← Terminal 2: Pemain WHITE\n", argv[0]);
        printf("    %s black   ← Terminal 3: Pemain BLACK\n\n", argv[0]);
        printf("  \033[1mTugas 1 — Dialog Dua Proses (2 terminal):\033[0m\n");
        printf("    %s dialog1 ← Terminal A: Process 1 (kirim pertama)\n", argv[0]);
        printf("    %s dialog2 ← Terminal B: Process 2 (terima & balas)\n\n", argv[0]);
        printf("  \033[1mMonitoring Message Queue (sesuai modul):\033[0m\n");
        printf("    ipcs -q              ← Lihat semua message queue\n");
        printf("    ipcs -s              ← Lihat semaphore\n");
        printf("    ipcs -m              ← Lihat shared memory\n");
        printf("    ipcrm -q <msqid>    ← Hapus message queue\n\n");
        printf("  \033[1mFitur IPC yang diimplementasikan:\033[0m\n");
        printf("    %-20s msgget/msgsnd/msgrcv/msgctl\n", "Message Queue:");
        printf("    %-20s shm_open/mmap\n", "Shared Memory:");
        printf("    %-20s sem_open/sem_wait/sem_post\n", "Semaphore:");
        printf("    %-20s pthread_create/pthread_join\n\n", "POSIX Thread:");
        return 1;
    }

    if (!strcmp(argv[1], "server")) {
        run_server();
        return 0;
    }

    if (!strcmp(argv[1], "dialog1")) {
        run_dialog_process1();
        return 0;
    }

    if (!strcmp(argv[1], "dialog2")) {
        run_dialog_process2();
        return 0;
    }

    /* Client — buka resource yang sudah dibuat server */
    if (shm_setup_client()  < 0) return 1;
    if (mq_setup_client()   < 0) return 1;

    sem_white = sem_open(SEM_WHITE, 0);
    sem_black = sem_open(SEM_BLACK, 0);
    sem_mutex = sem_open(SEM_MUTEX, 0);
    if (sem_white==SEM_FAILED||sem_black==SEM_FAILED||sem_mutex==SEM_FAILED) {
        perror("sem_open client"); return 1;
    }

    if      (!strcmp(argv[1], "white")) run_player( 1);
    else if (!strcmp(argv[1], "black")) run_player(-1);
    else {
        printf("\033[31mArgumen tidak dikenal: %s\033[0m\n", argv[1]);
        printf("Gunakan: server | white | black | dialog1 | dialog2\n");
        return 1;
    }

    sem_close(sem_white);
    sem_close(sem_black);
    sem_close(sem_mutex);
    if (shm && shm != MAP_FAILED) munmap(shm, sizeof(ChessBoard));
    if (shm_fd >= 0) close(shm_fd);
    return 0;
}
