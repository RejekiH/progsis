# Game Catur Linux — Mode Dua Terminal

## Cara Main

### Langkah 1 — Buka 3 terminal di Rocky Linux VMware

Di VMware, Anda bisa buka terminal baru dengan:
- Klik kanan desktop → Open Terminal
- Atau tekan Ctrl+Alt+T
- Atau buka aplikasi Terminal dari menu

### Langkah 2 — Terminal pertama: jalankan server

```bash
cd ~/chess_game
bash chess_server.sh
```

Server akan menampilkan:
```
  ╔════════════════════════════════════════════╗
  ║      ♟  GAME CATUR LINUX — SERVER  ♟      ║
  ╚════════════════════════════════════════════╝

  PID Server: 1234
  Cara memulai di terminal lain:
  bash chess_client.sh w   ← Terminal Pemain WHITE
  bash chess_client.sh b   ← Terminal Pemain BLACK

  Menunggu kedua pemain terhubung...
```

### Langkah 3 — Terminal kedua: Pemain WHITE

```bash
cd ~/chess_game
bash chess_client.sh w
```

### Langkah 4 — Terminal ketiga: Pemain BLACK

```bash
cd ~/chess_game
bash chess_client.sh b
```

Setelah kedua pemain terhubung, game otomatis dimulai!

---

## Tampilan Per Terminal

**Terminal Server:**
- Menampilkan log koneksi dan move yang masuk
- Bisa dipantau untuk debug

**Terminal WHITE:**
- Papan catur dari perspektif WHITE (putih di bawah)
- Bisa input saat giliran putih
- Saat menunggu: tampil "Menunggu BLACK bermain..."

**Terminal BLACK:**
- Papan catur dari perspektif BLACK (hitam di bawah — papan terbalik)
- Bisa input saat giliran hitam
- Saat menunggu: tampil "Menunggu WHITE bermain..."

---

## Mekanisme Komunikasi

```
Terminal WHITE          Server              Terminal BLACK
     │                    │                      │
     │── CONNECT:w ──────►│                      │
     │                    │◄──── CONNECT:b ───────│
     │◄── GAME_START ─────┤──── GAME_START ──────►│
     │                    │                      │
     │── e2e4 ───────────►│                      │
     │                    │─── UPDATE:e2e4 ──────►│
     │◄── UPDATE:e2e4 ────│                      │
     │                    │                      │
     │                    │◄──── e7e5 ────────────│
     │◄── UPDATE:e7e5 ────│─── UPDATE:e7e5 ──────►│
```

**Komponen yang digunakan:**
- Named Pipes (FIFO): komunikasi client → server dan server → client
- Shared Memory (file): state papan dibaca semua terminal
- Signal: SIGTERM untuk shutdown bersih
- POSIX Thread (subshell): monitor heartbeat
- AWK Engine: validasi semua gerakan

---

## Perintah Saat Bermain

| Perintah | Fungsi |
|----------|--------|
| `e2e4`   | Gerakan standar |
| `resign` | Menyerah |
| `quit`   | Keluar dari client |
| `help`   | Bantuan format |

---

## File yang Digunakan (/tmp/)

| File | Fungsi |
|------|--------|
| `chessgame1_state`    | Shared memory — state papan |
| `chessgame1_move_w`   | Pipe WHITE → Server |
| `chessgame1_move_b`   | Pipe BLACK → Server |
| `chessgame1_notify_w` | Pipe Server → WHITE |
| `chessgame1_notify_b` | Pipe Server → BLACK |
| `chessgame1_log`      | Log sistem |

Semua file dibersihkan otomatis saat server berhenti.

---

## Bermain di Satu Komputer (Split Screen)

Di Rocky Linux dengan GNOME:
1. Buka Terminal
2. Klik menu terminal → "Buka Terminal Baru" (atau Ctrl+Shift+N)
3. Susun jendela berdampingan
4. Terminal 1: `bash chess_server.sh`
5. Terminal 2: `bash chess_client.sh w`
6. Terminal 3: `bash chess_client.sh b`

Atau gunakan **tmux** jika tersedia:
```bash
tmux new-session -s chess \; \
  split-window -h \; \
  split-window -v \; \
  select-pane -t 0 \; send-keys "bash chess_server.sh" Enter \; \
  select-pane -t 1 \; send-keys "bash chess_client.sh w" Enter \; \
  select-pane -t 2 \; send-keys "bash chess_client.sh b" Enter
```
