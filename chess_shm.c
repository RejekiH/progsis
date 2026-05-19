/*
 * ============================================================
 * chess_shm.c — Game Catur Linux
 * Fitur IPC Lengkap:
 *   1. Shared Memory (shm_open + mmap) → board[8][8] array
 *   2. POSIX Semaphore                 → sinkronisasi giliran
 *   3. Message Queue (System V IPC)    → notifikasi & chat
 *   4. POSIX Thread                    → receiver MQ background
 *
 * Kompilasi:
 *   gcc -Wall -std=c99 -D_GNU_SOURCE -o chess_shm chess_shm.c -lpthread -lrt
 *
 * Jalankan (3 terminal):
 *   ./chess_shm server   ← Terminal 1
 *   ./chess_shm white    ← Terminal 2 (Layer 1)
 *   ./chess_shm black    ← Terminal 3 (Layer 2)
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
#include <sys/msg.h>        /* System V Message Queue */

/* ═══════════════════════════════════════════════════════════
   1. KONSTANTA & NAMA RESOURCE IPC
   ═══════════════════════════════════════════════════════════ */
#define SHM_NAME      "/chess_shm_v2"
#define SEM_WHITE     "/chess_sem_w"
#define SEM_BLACK     "/chess_sem_b"
#define SEM_MUTEX     "/chess_sem_mx"
#define MQ_KEY_PATH   "/tmp"
#define MQ_KEY_ID     'Z'

/* Tipe penerima pesan di Message Queue */
#define MTYPE_WHITE    1L   /* → Pemain WHITE  */
#define MTYPE_BLACK    2L   /* → Pemain BLACK  */
#define MTYPE_SERVER   3L   /* → Server (log)  */

/* Jenis event */
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
} EventType;

/* ── Struktur satu pesan MQ ──────────────────────────────── */
typedef struct {
    long      mtype;           /* tipe penerima (WAJIB field pertama) */
    EventType event;           /* jenis event                         */
    char      notation[8];    /* notasi gerakan: "e2e4\0"            */
    char      text[200];      /* teks bebas: chat / error / info     */
    int       extra;          /* data tambahan (nilai bidak dll)      */
    time_t    ts;             /* timestamp                            */
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
   3. STRUKTUR SHARED MEMORY — board[8][8] sebagai array
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    /* Papan diformat ke array 2 dimensi — nilai integer */
    int    board[8][8];       /* board[row][col] */

    /* Status game */
    int    move_count;
    int    current_player;    /* 1=WHITE, -1=BLACK */
    int    game_active;       /* 1=berjalan, 0=selesai */
    int    in_check;
    int    winner;            /* 1=WHITE, -1=BLACK, 0=seri/belum */
    int    white_connected;
    int    black_connected;

    /* Bidak yang ditangkap (disimpan dalam array) */
    int    white_captured[16];
    int    white_cap_count;
    int    black_captured[16];
    int    black_cap_count;

    /* Informasi tambahan */
    char   last_notation[8];
    time_t last_move_time;
    int    draw_offered_by;   /* 0=none, 1=WHITE, -1=BLACK */
    int    mq_id_stored;      /* simpan MQ ID di SHM agar client bisa baca */
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
    /* Unblock semaphore agar proses tidak hang */
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
   MESSAGE QUEUE (System V IPC) — setup & operasi
   ═══════════════════════════════════════════════════════════ */

/* Buat message queue baru (server) */
static int mq_setup_server(void) {
    key_t key = ftok(MQ_KEY_PATH, MQ_KEY_ID);
    if (key == -1) { perror("ftok"); return -1; }

    /* Hapus queue lama jika ada */
    int old = msgget(key, 0666);
    if (old >= 0) msgctl(old, IPC_RMID, NULL);

    /* Buat queue baru */
    mq_id = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
    if (mq_id < 0) { perror("msgget server"); return -1; }
    return 0;
}

/* Buka queue yang sudah ada (client) */
static int mq_setup_client(void) {
    for (int i = 0; i < 20; i++) {
        key_t key = ftok(MQ_KEY_PATH, MQ_KEY_ID);
        if (key != -1) {
            mq_id = msgget(key, 0666);
            if (mq_id >= 0) return 0;
        }
        printf("  [MQ] Menunggu message queue (%d/20)...\n", i+1);
        sleep(1);
    }
    perror("msgget client");
    return -1;
}

/* Hapus queue (server saat shutdown) */
static void mq_destroy(void) {
    if (mq_id >= 0) {
        msgctl(mq_id, IPC_RMID, NULL);
        mq_id = -1;
    }
}

/* Kirim pesan ke queue */
static void mq_send(long mtype, EventType evt,
                    const char *notif, const char *text, int extra) {
    ChessMsg m;
    memset(&m, 0, sizeof(m));
    m.mtype = mtype;
    m.event = evt;
    m.extra = extra;
    m.ts    = time(NULL);
    if (notif) strncpy(m.notation, notif, 7);
    if (text)  strncpy(m.text,     text,  199);

    if (msgsnd(mq_id, &m, sizeof(m) - sizeof(long), IPC_NOWAIT) < 0) {
        if (errno != EAGAIN && errno != EINTR)
            perror("[MQ] msgsnd");
    }
}

/* Terima pesan — blocking */
static int mq_recv_block(long mtype, ChessMsg *out) {
    ssize_t r = msgrcv(mq_id, out,
                       sizeof(ChessMsg) - sizeof(long),
                       mtype, 0);
    return (r < 0) ? -1 : 0;
}

/* Terima pesan — non-blocking */
static int mq_recv_nowait(long mtype, ChessMsg *out) {
    ssize_t r = msgrcv(mq_id, out,
                       sizeof(ChessMsg) - sizeof(long),
                       mtype, IPC_NOWAIT);
    return (r < 0) ? -1 : 0;
}

/* Tampilkan info queue (ipcs-style) */
static void mq_print_info(void) {
    struct msqid_ds info;
    if (msgctl(mq_id, IPC_STAT, &info) == 0) {
        printf("  [MQ] ID=%-6d | Pesan antri=%-4lu | Byte=%-6lu\n",
               mq_id,
               (unsigned long)info.msg_qnum,
               (unsigned long)info.msg_cbytes);
    }
}

/* ═══════════════════════════════════════════════════════════
   INISIALISASI PAPAN
   ═══════════════════════════════════════════════════════════ */
static void board_init(void) {
    memset(shm, 0, sizeof(ChessBoard));

    /* Baris 0 — buah hitam belakang */
    shm->board[0][0] = B_ROOK;   shm->board[0][1] = B_KNIGHT;
    shm->board[0][2] = B_BISHOP; shm->board[0][3] = B_QUEEN;
    shm->board[0][4] = B_KING;   shm->board[0][5] = B_BISHOP;
    shm->board[0][6] = B_KNIGHT; shm->board[0][7] = B_ROOK;

    /* Baris 1 — pion hitam */
    for (int c = 0; c < 8; c++) shm->board[1][c] = B_PAWN;

    /* Baris 6 — pion putih */
    for (int c = 0; c < 8; c++) shm->board[6][c] = W_PAWN;

    /* Baris 7 — buah putih belakang */
    shm->board[7][0] = W_ROOK;   shm->board[7][1] = W_KNIGHT;
    shm->board[7][2] = W_BISHOP; shm->board[7][3] = W_QUEEN;
    shm->board[7][4] = W_KING;   shm->board[7][5] = W_BISHOP;
    shm->board[7][6] = W_KNIGHT; shm->board[7][7] = W_ROOK;

    shm->current_player  = 1;     /* WHITE mulai */
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

/* Snapshot aman dari shared memory */
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

    printf("\033[2J\033[H"); /* clear screen */
    printf("\033[1;36m");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║    ♟  GAME CATUR LINUX — IPC COMPLETE  ♟         ║\n");
    printf("  ║   SHM board[8][8] + Semaphore + MsgQueue + Thread║\n");
    printf("  ╚══════════════════════════════════════════════════╝\033[0m\n\n");

    /* Info baris 1 */
    printf("  \033[2mGiliran :\033[0m \033[1m%s\033[0m%s",
           s.cur > 0 ? "WHITE" : "BLACK",
           s.chk ? "  \033[1;31m⚠ SKAK!\033[0m" : "");
    printf("   Move #%d   MQ_ID:\033[33m%d\033[0m\n", s.mc, mq_id);

    /* Info baris 2 */
    printf("  \033[2mAnda    :\033[0m \033[1m%s\033[0m",
           my_color > 0 ? "WHITE" : "BLACK");
    if (s.notation[0])
        printf("   \033[2mGerakan terakhir:\033[0m \033[33m%s\033[0m", s.notation);
    printf("\n\n");

    /* Panel bidak yang ditangkap */
    printf("  ┌─────────────────────────────────────────────────┐\n");
    printf("  │ \033[1mBidak Ditangkap\033[0m                                  │\n");
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
    printf("  └─────────────────────────────────────────────────┘\n\n");

    /* Papan catur dari array board[8][8] */
    printf("       a   b   c   d   e   f   g   h\n");
    printf("     ┌───┬───┬───┬───┬───┬───┬───┬───┐\n");

    int rs = 0, re = 8, step = 1;
    if (my_color < 0) { rs = 7; re = -1; step = -1; } /* BLACK: terbalik */

    for (int r = rs; r != re; r += step) {
        int rank = 8 - r;
        printf("   %d │", rank);
        for (int c = 0; c < 8; c++) {
            int p = s.board[r][c];
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

    /* Tawaran seri */
    if (s.draw_by != 0 && s.draw_by != my_color)
        printf("  \033[1;33m🤝 Lawan menawarkan SERI! Ketik 'draw' untuk terima.\033[0m\n");

    /* Pesan terakhir (dari MQ) */
    if (msg && msg[0])
        printf("  %s\n", msg);

    printf("\n");
    printf("  \033[2mPerintah: [gerakan] e2e4 | chat:<pesan> | draw | resign | quit\033[0m\n");
}

/* ═══════════════════════════════════════════════════════════
   VALIDASI & EKSEKUSI MOVE DI board[8][8]
   ═══════════════════════════════════════════════════════════ */
static int parse_nota(const char *s, int *fr, int *fc, int *tr, int *tc) {
    if (!s || strlen(s) < 4) return 0;
    if (s[0]<'a'||s[0]>'h'||s[2]<'a'||s[2]>'h') return 0;
    if (s[1]<'1'||s[1]>'8'||s[3]<'1'||s[3]>'8') return 0;
    *fc = s[0]-'a';  *fr = 8-(s[1]-'0');
    *tc = s[2]-'a';  *tr = 8-(s[3]-'0');
    return 1;
}

/* Validasi & eksekusi — harus dipanggil saat memegang sem_mutex */
static const char *do_move(int player, int fr, int fc, int tr, int tc,
                           char *out_nota) {
    int p    = shm->board[fr][fc];
    int dest = shm->board[tr][tc];
    if (p == EMPTY) return "EMPTY_SOURCE";
    int pc = (p > 0) ? 1 : -1;
    if (pc != player) return "WRONG_COLOR";
    if (dest != EMPTY && ((dest>0)==(p>0))) return "DEST_OCCUPIED";

    int lp = abs(p), dr = tr-fr, dc = tc-fc;

    /* Validasi per jenis bidak */
    if (lp == 1) { /* PION */
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
    } else if (lp == 2) { /* BENTENG */
        if (fr!=tr && fc!=tc) return "INVALID_ROOK";
        int ddr=(tr>fr)?1:(tr<fr)?-1:0, ddc=(tc>fc)?1:(tc<fc)?-1:0;
        for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
            if (shm->board[r][c]!=EMPTY) return "ROOK_BLOCKED";
    } else if (lp == 4) { /* GAJAH */
        if (abs(dr)!=abs(dc)) return "INVALID_BISHOP";
        int ddr=(dr>0)?1:-1, ddc=(dc>0)?1:-1;
        for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
            if (shm->board[r][c]!=EMPTY) return "BISHOP_BLOCKED";
    } else if (lp == 3) { /* KUDA */
        if (!((abs(dr)==2&&abs(dc)==1)||(abs(dr)==1&&abs(dc)==2)))
            return "INVALID_KNIGHT";
    } else if (lp == 5) { /* RATU */
        if (fr==tr||fc==tc) {
            int ddr=(tr>fr)?1:(tr<fr)?-1:0, ddc=(tc>fc)?1:(tc<fc)?-1:0;
            for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
                if (shm->board[r][c]!=EMPTY) return "QUEEN_BLOCKED";
        } else if (abs(dr)==abs(dc)) {
            int ddr=(dr>0)?1:-1, ddc=(dc>0)?1:-1;
            for (int r=fr+ddr,c=fc+ddc; r!=tr||c!=tc; r+=ddr,c+=ddc)
                if (shm->board[r][c]!=EMPTY) return "QUEEN_BLOCKED";
        } else return "INVALID_QUEEN";
    } else if (lp == 6) { /* RAJA */
        if (abs(dr)>1||abs(dc)>1) return "INVALID_KING";
    }

    /* ── Eksekusi: update array board[8][8] ── */
    if (dest != EMPTY) {
        if (player == 1) shm->white_captured[shm->white_cap_count++] = dest;
        else             shm->black_captured[shm->black_cap_count++] = dest;
        if (abs(dest) == 6) {          /* Raja dimakan → game over */
            shm->winner      = player;
            shm->game_active = 0;
        }
    }
    shm->board[tr][tc] = p;
    shm->board[fr][fc] = EMPTY;

    /* Promosi pion */
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
   POSIX THREAD — Receiver Message Queue (berjalan di background)
   Ini adalah thread yang terus mendengarkan pesan MQ masuk
   ═══════════════════════════════════════════════════════════ */
typedef struct {
    int   my_color;
    char  buf[256];       /* buffer pesan terakhir dari MQ */
    int   has_msg;        /* flag ada pesan baru */
    int   game_over;
    pthread_mutex_t lock; /* mutex untuk proteksi buf */
} MQRecvArg;

static void *thread_mq_recv(void *arg) {
    MQRecvArg *a = (MQRecvArg *)arg;
    long mtype = (a->my_color > 0) ? MTYPE_WHITE : MTYPE_BLACK;

    while (g_running) {
        ChessMsg m;
        /* blocking recv — thread tidur sampai ada pesan masuk */
        if (mq_recv_block(mtype, &m) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!g_running) break;

        /* Format pesan ke buffer */
        char tmp[256] = "";
        switch (m.event) {
            case EVT_YOUR_TURN:
                snprintf(tmp, 255, "\033[32m► Giliran Anda!\033[0m");
                break;
            case EVT_OPP_MOVED:
                snprintf(tmp, 255,
                    "\033[36m← Lawan bermain: \033[1m%s\033[0m", m.notation);
                break;
            case EVT_CHECK:
                snprintf(tmp, 255,
                    "\033[1;31m⚠ SKAK! Raja Anda terancam! (setelah %s)\033[0m",
                    m.notation);
                break;
            case EVT_CHECKMATE:
                snprintf(tmp, 255,
                    "\033[1;31m✗ SKAKMAT! Game selesai. %s\033[0m", m.text);
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_GAMEOVER:
                snprintf(tmp, 255,
                    "\033[1;33m● Game selesai: %s\033[0m", m.text);
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_CHAT:
                snprintf(tmp, 255,
                    "\033[35m💬 Lawan: %s\033[0m", m.text);
                break;
            case EVT_OFFER_DRAW:
                snprintf(tmp, 255,
                    "\033[33m🤝 Lawan menawarkan SERI! Ketik 'draw'.\033[0m");
                break;
            case EVT_ACCEPT_DRAW:
                snprintf(tmp, 255,
                    "\033[33m🤝 Tawaran seri diterima! Game SERI.\033[0m");
                a->game_over = 1; g_running = 0;
                sem_post((a->my_color>0)?sem_white:sem_black);
                break;
            case EVT_MOVE_ERROR:
                snprintf(tmp, 255, "\033[31m✗ %s\033[0m", m.text);
                break;
            case EVT_MOVE_OK:
                snprintf(tmp, 255, "\033[32m✓ Gerakan %s berhasil\033[0m", m.notation);
                break;
            case EVT_CONNECTED:
                snprintf(tmp, 255,
                    "\033[2m[Info] %s terhubung ke game\033[0m", m.text);
                break;
            case EVT_SERVER_LOG:
                snprintf(tmp, 255, "\033[2m[Server] %s\033[0m", m.text);
                break;
            default:
                snprintf(tmp, 255, "\033[2m[MQ evt=%d] %s\033[0m",
                         m.event, m.text);
                break;
        }

        /* Simpan ke buffer dengan lock */
        pthread_mutex_lock(&a->lock);
        strncpy(a->buf, tmp, 255);
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

    /* Banner */
    printf("\033[2J\033[H");
    printf("\033[1;32m");
    printf("  ╔═════════════════════════════════════════════╗\n");
    printf("  ║  Terhubung sebagai %-8s                 ║\n", cn);
    printf("  ║  Message Queue ID  : %-6d               ║\n", mq_id);
    printf("  ║  Shared Mem        : %-20s  ║\n", SHM_NAME);
    printf("  ╚═════════════════════════════════════════════╝\033[0m\n\n");
    printf("  \033[2mPerintah:\033[0m\n");
    printf("  \033[32me2e4\033[0m          → Gerakan catur\n");
    printf("  \033[32mchat:<pesan>\033[0m  → Kirim pesan ke lawan via MQ\n");
    printf("  \033[32mdraw\033[0m          → Tawarkan / terima seri\n");
    printf("  \033[32mresign\033[0m        → Menyerah\n");
    printf("  \033[32mquit\033[0m          → Keluar\n\n");
    printf("  Menunggu pemain lain...\n\n");

    /* Tandai koneksi */
    sem_wait(sem_mutex);
    if (my_color > 0) shm->white_connected = 1;
    else              shm->black_connected  = 1;
    sem_post(sem_mutex);

    /* Beritahu server via MQ */
    mq_send(MTYPE_SERVER, EVT_CONNECTED, "", cn, my_color);
    /* Beritahu lawan */
    char info[64];
    snprintf(info, 63, "%s bergabung!", cn);
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

        /* SEMAPHORE WAIT — blokir sampai giliran tiba */
        sem_wait(my_sem);
        if (!g_running || !shm->game_active) break;

        /* Verifikasi giliran */
        sem_wait(sem_mutex);
        int cur = shm->current_player;
        sem_post(sem_mutex);
        if (cur != my_color) continue;

        /* Ambil pesan terbaru dari thread receiver */
        pthread_mutex_lock(&targ.lock);
        if (targ.has_msg) {
            strncpy(last_msg, targ.buf, 255);
            targ.has_msg = 0;
        }
        pthread_mutex_unlock(&targ.lock);
        if (targ.game_over) break;

        /* Render papan */
        render(my_color, last_msg);
        last_msg[0] = '\0';

        if (!shm->game_active) break;

        /* ── Input pemain ── */
        char    inp[128];
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
                mq_send(op_mt,  EVT_GAMEOVER, "", txt, 0);
                mq_send(my_mt,  EVT_GAMEOVER, "", txt, 0);
                sem_post(op_sem);
                g_running = 0; break;
            }

            /* ── draw ── */
            if (!strcmp(inp, "draw")) {
                sem_wait(sem_mutex);
                int db = shm->draw_offered_by;
                sem_post(sem_mutex);

                if (db == -my_color) {
                    /* Terima tawaran seri lawan */
                    sem_wait(sem_mutex);
                    shm->game_active = 0;
                    shm->winner      = 0;
                    sem_post(sem_mutex);
                    mq_send(op_mt, EVT_ACCEPT_DRAW, "", "Seri diterima!", 0);
                    mq_send(my_mt, EVT_ACCEPT_DRAW, "", "Seri diterima!", 0);
                    sem_post(op_sem);
                    g_running = 0; break;
                } else {
                    /* Tawarkan seri */
                    sem_wait(sem_mutex);
                    shm->draw_offered_by = my_color;
                    sem_post(sem_mutex);
                    mq_send(op_mt, EVT_OFFER_DRAW, "", cn, 0);
                    snprintf(last_msg, 255,
                        "\033[33mTawaran seri dikirim. Lanjut bermain...\033[0m");
                    err = "draw_pending"; continue;
                }
            }

            /* ── chat:<pesan> ── */
            if (!strncmp(inp, "chat:", 5)) {
                const char *txt = inp + 5;
                /* Kirim pesan chat ke lawan via Message Queue */
                mq_send(op_mt, EVT_CHAT, "", txt, 0);
                /* Log ke server */
                char log[220];
                snprintf(log, 219, "[%s] %s", cn, txt);
                mq_send(MTYPE_SERVER, EVT_CHAT, "", log, 0);
                snprintf(last_msg, 255, "\033[35m💬 Anda: %s\033[0m", txt);
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
            /* LOCK mutex — akses array board[8][8] */
            sem_wait(sem_mutex);
            err = do_move(my_color, fr, fc, tr, tc, nota);
            int w = shm->winner;
            int ac = shm->game_active;
            sem_post(sem_mutex);

            if (!strcmp(err, "OK")) {
                /* Game over via tangkap raja */
                if (w != 0 || !ac) {
                    char txt[80];
                    if (w != 0)
                        snprintf(txt, 79, "%s MENANG dengan gerakan %s!",
                                 cn, nota);
                    else
                        snprintf(txt, 79, "Game selesai.");
                    mq_send(op_mt, EVT_GAMEOVER, nota, txt, 0);
                    mq_send(my_mt, EVT_GAMEOVER, nota, txt, 0);
                    sem_post(op_sem);
                    g_running = 0; break;
                }

                /* Kirim event ke lawan via Message Queue */
                mq_send(op_mt, EVT_OPP_MOVED, nota, cn,  0);
                mq_send(op_mt, EVT_YOUR_TURN,  nota, "",  0);

                /* Kirim konfirmasi ke diri sendiri */
                mq_send(my_mt, EVT_MOVE_OK, nota, "", 0);
                snprintf(last_msg, 255,
                    "\033[32m✓ Gerakan \033[1m%s\033[0;32m berhasil\033[0m", nota);

            } else {
                /* Terjemahkan error */
                const char *em = err;
                if (!strcmp(err,"EMPTY_SOURCE"))  em="Tidak ada bidak di petak itu!";
                if (!strcmp(err,"WRONG_COLOR"))    em="Itu bidak lawan!";
                if (!strcmp(err,"DEST_OCCUPIED"))  em="Petak tujuan sudah ada bidak Anda!";
                if (!strcmp(err,"INVALID_PAWN"))   em="Pion: maju lurus, makan diagonal jika ada musuh!";
                if (!strcmp(err,"ROOK_BLOCKED"))   em="Benteng: terhalang bidak lain!";
                if (!strcmp(err,"BISHOP_BLOCKED")) em="Gajah: terhalang bidak lain!";
                if (!strcmp(err,"INVALID_KNIGHT")) em="Kuda: gerakan L (2+1 kotak)!";
                if (!strcmp(err,"QUEEN_BLOCKED"))  em="Ratu: terhalang bidak lain!";
                if (!strcmp(err,"INVALID_KING"))   em="Raja: maksimal 1 kotak!";
                if (!strcmp(err,"INVALID_ROOK"))   em="Benteng: hanya lurus!";
                if (!strcmp(err,"INVALID_BISHOP")) em="Gajah: hanya diagonal!";
                if (!strcmp(err,"INVALID_QUEEN"))  em="Ratu: lurus atau diagonal saja!";

                /* Kirim pesan error ke diri sendiri via MQ */
                mq_send(my_mt, EVT_MOVE_ERROR, "", em, 0);
                snprintf(last_msg, 255, "\033[31m✗ %s\033[0m", em);
            }

        } while (strcmp(err,"OK")!=0
                 && strcmp(err,"draw_pending")!=0
                 && strcmp(err,"chat")!=0
                 && g_running && shm->game_active);

        if (!g_running || !shm->game_active) break;

        /* SEMAPHORE POST — serahkan giliran ke lawan */
        sleep(1);
        sem_post(op_sem);
    }

    /* Papan akhir */
    if (shm) {
        Snap s; take_snap(&s);
        render(my_color, last_msg);
        if      (s.winner == my_color)
            printf("\033[1;32m╔══════════════════════╗\n"
                   "║   SELAMAT MENANG!    ║\n"
                   "╚══════════════════════╝\033[0m\n");
        else if (s.winner == -my_color)
            printf("\033[1;31m╔══════════════════════╗\n"
                   "║   ANDA KALAH.        ║\n"
                   "╚══════════════════════╝\033[0m\n");
        else if (!s.active)
            printf("\033[1;33m╔══════════════════════╗\n"
                   "║   SERI (DRAW).       ║\n"
                   "╚══════════════════════╝\033[0m\n");
    }

    g_running = 0;
    pthread_cancel(recv_tid);
    pthread_join(recv_tid, NULL);
    pthread_mutex_destroy(&targ.lock);
    printf("\n\033[33mTerima kasih sudah bermain! [%s]\033[0m\n", cn);
}

/* ═══════════════════════════════════════════════════════════
   SERVER — inisialisasi semua resource IPC + monitor
   ═══════════════════════════════════════════════════════════ */

/* Thread server: monitor semua pesan dari client */
static void *thread_server_mq(void *arg) {
    (void)arg;
    printf("  [MQ-Thread] Mendengarkan pesan dari client...\n\n");
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
                default: break;
            }
            printf("  [MQ %s] %-10s | \"%s\"\n", ts, evname, m.text);
            fflush(stdout);
        }
        usleep(300000); /* poll tiap 300ms */
    }
    return NULL;
}

static void run_server(void) {
    printf("\033[2J\033[H");
    printf("\033[1;36m");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║   CHESS SERVER — IPC LENGKAP                     ║\n");
    printf("  ║   Shared Memory Array + Semaphore + Msg Queue    ║\n");
    printf("  ╚══════════════════════════════════════════════════╝\033[0m\n\n");

    /* ── 1. POSIX Shared Memory ── */
    if (shm_setup_server() < 0) exit(1);
    board_init();
    printf("  \033[32m✓ Shared Memory\033[0m  : %s\n", SHM_NAME);
    printf("    struct ChessBoard { int board[8][8]; ... } = %lu bytes\n",
           (unsigned long)sizeof(ChessBoard));
    printf("    Putih(+): Pion=1 Benteng=2 Kuda=3 Gajah=4 Ratu=5 Raja=6\n");
    printf("    Hitam(-): nilai negatif\n\n");

    /* ── 2. POSIX Semaphore ── */
    sem_unlink(SEM_WHITE); sem_unlink(SEM_BLACK); sem_unlink(SEM_MUTEX);
    sem_white = sem_open(SEM_WHITE, O_CREAT, 0666, 1); /* WHITE mulai */
    sem_black = sem_open(SEM_BLACK, O_CREAT, 0666, 0); /* BLACK tunggu */
    sem_mutex = sem_open(SEM_MUTEX, O_CREAT, 0666, 1); /* mutex */
    if (sem_white==SEM_FAILED||sem_black==SEM_FAILED||sem_mutex==SEM_FAILED) {
        perror("sem_open"); exit(1);
    }
    printf("  \033[32m✓ Semaphore POSIX\033[0m:\n");
    printf("    %-20s (WHITE giliran, nilai awal=1)\n", SEM_WHITE);
    printf("    %-20s (BLACK tunggu,  nilai awal=0)\n", SEM_BLACK);
    printf("    %-20s (mutex board,   nilai awal=1)\n\n", SEM_MUTEX);

    /* ── 3. System V Message Queue ── */
    if (mq_setup_server() < 0) exit(1);
    shm->mq_id_stored = mq_id;
    printf("  \033[32m✓ Message Queue\033[0m  : ID=%d\n", mq_id);
    printf("    MTYPE_WHITE=%ld → pesan ke WHITE\n", MTYPE_WHITE);
    printf("    MTYPE_BLACK=%ld → pesan ke BLACK\n", MTYPE_BLACK);
    printf("    MTYPE_SERVER=%ld → log dari client\n", MTYPE_SERVER);
    printf("    Event: YOUR_TURN OPP_MOVED CHECK GAMEOVER CHAT DRAW ERROR\n\n");

    printf("  \033[1mJalankan client:\033[0m\n");
    printf("  \033[32m./chess_shm white\033[0m   ← Terminal 2 (Layer 1)\n");
    printf("  \033[32m./chess_shm black\033[0m   ← Terminal 3 (Layer 2)\n\n");
    printf("  \033[2mMonitor aktif... (Ctrl+C untuk stop)\033[0m\n");
    printf("  ─────────────────────────────────────────────────────\n");

    /* Thread monitor MQ */
    pthread_t mq_tid;
    pthread_create(&mq_tid, NULL, thread_server_mq, NULL);

    /* Main monitor loop */
    int last_mc = -1;
    while (g_running) {
        sleep(2);
        if (!shm->game_active) break;

        int mc = shm->move_count;
        if (mc != last_mc) {
            mq_print_info();
            printf("  [SHM] Move:%-3d | Giliran:%-6s | "
                   "WCap:%d BCap:%d | SHM_board[8][8]\n",
                   mc,
                   shm->current_player > 0 ? "WHITE" : "BLACK",
                   shm->white_cap_count,
                   shm->black_cap_count);
            fflush(stdout);
            last_mc = mc;
        }
    }

    printf("\n  [SERVER] Game selesai.");
    if (shm->winner > 0)       printf(" Pemenang: WHITE\n");
    else if (shm->winner < 0)  printf(" Pemenang: BLACK\n");
    else                       printf(" Hasil: SERI\n");

    g_running = 0;
    pthread_cancel(mq_tid);
    pthread_join(mq_tid, NULL);

    printf("  [SERVER] Membersihkan semua resource IPC...\n");
    shm_destroy();
    sem_close(sem_white); sem_unlink(SEM_WHITE);
    sem_close(sem_black); sem_unlink(SEM_BLACK);
    sem_close(sem_mutex); sem_unlink(SEM_MUTEX);
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
        printf("\n  Game Catur Linux — IPC Complete\n\n");
        printf("  Penggunaan:\n");
        printf("    %s server  ← Terminal 1: init IPC + monitor\n", argv[0]);
        printf("    %s white   ← Terminal 2: Pemain WHITE (Layer 1)\n", argv[0]);
        printf("    %s black   ← Terminal 3: Pemain BLACK (Layer 2)\n\n", argv[0]);
        printf("  Fitur IPC:\n");
        printf("    Shared Memory  : shm_open/mmap → board[8][8]\n");
        printf("    Semaphore POSIX: sem_open/sem_wait/sem_post\n");
        printf("    Message Queue  : msgget/msgsnd/msgrcv (System V)\n");
        printf("    POSIX Thread   : pthread (receiver MQ background)\n\n");
        return 1;
    }

    if (!strcmp(argv[1], "server")) {
        run_server();
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
    else { printf("Argumen tidak dikenal: %s\n", argv[1]); return 1; }

    sem_close(sem_white);
    sem_close(sem_black);
    sem_close(sem_mutex);
    if (shm && shm != MAP_FAILED) munmap(shm, sizeof(ChessBoard));
    if (shm_fd >= 0) close(shm_fd);
    return 0;
}
