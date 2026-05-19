#!/bin/bash
# ============================================================
# chess_client.sh — Client Pemain (satu terminal = satu pemain)
# 
# Penggunaan:
#   bash chess_client.sh w    ← Pemain WHITE (Terminal 1)
#   bash chess_client.sh b    ← Pemain BLACK (Terminal 2)
#
# Jalankan SETELAH chess_server.sh sudah berjalan
# ============================================================

PLAYER="${1,,}"   # w atau b
if [ "$PLAYER" != "w" ] && [ "$PLAYER" != "b" ]; then
    printf "Penggunaan: bash chess_client.sh w|b\n"
    printf "  w = Pemain WHITE\n"
    printf "  b = Pemain BLACK\n"
    exit 1
fi

GAME_ID="chessgame1"
STATE_FILE="/tmp/${GAME_ID}_state"
LOG_FILE="/tmp/${GAME_ID}_log"
PIPE_W="/tmp/${GAME_ID}_move_w"
PIPE_B="/tmp/${GAME_ID}_move_b"
NOTIFY_W="/tmp/${GAME_ID}_notify_w"
NOTIFY_B="/tmp/${GAME_ID}_notify_b"

# Tentukan pipe berdasarkan warna
if [ "$PLAYER" = "w" ]; then
    MY_PIPE="$PIPE_W"; MY_NOTIFY="$NOTIFY_W"
    MY_NAME="WHITE";   OPP_NAME="BLACK"
else
    MY_PIPE="$PIPE_B"; MY_NOTIFY="$NOTIFY_B"
    MY_NAME="BLACK";   OPP_NAME="WHITE"
fi

RED=$'\033[0;31m';  GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; WHITE=$'\033[1;37m'; BOLD=$'\033[1m'
DIM=$'\033[2m';     RESET=$'\033[0m'

MOVE_COUNT=0; IN_CHECK=0
WHITE_CAPTURED=""; BLACK_CAPTURED=""
LAST_MSG=""; GAME_ACTIVE=0
NOTIFY_PID=0

# ── Signal Handling ──────────────────────────────────────────
cleanup() {
    GAME_ACTIVE=0
    [ $NOTIFY_PID -gt 0 ] && kill $NOTIFY_PID 2>/dev/null
    printf "\n${YELLOW}Disconnected dari game.${RESET}\n"
    exit 0
}
trap 'cleanup' SIGTERM EXIT SIGINT

log_msg() { printf "[%s][CLIENT-%s] %s\n" "$(date '+%H:%M:%S')" "$MY_NAME" "$1" >> "$LOG_FILE"; }

# ── Baca state dari shared memory ────────────────────────────
read_state() {
    local line
    while IFS= read -r line; do
        case "$line" in
            MOVE_COUNT:*)     MOVE_COUNT="${line#MOVE_COUNT:}" ;;
            WHITE_CAPTURED:*) WHITE_CAPTURED="${line#WHITE_CAPTURED:}" ;;
            BLACK_CAPTURED:*) BLACK_CAPTURED="${line#BLACK_CAPTURED:}" ;;
            CURRENT_PLAYER:*) CURRENT_PLAYER="${line#CURRENT_PLAYER:}" ;;
        esac
    done < "$STATE_FILE" 2>/dev/null
}

# ── Simbol bidak ──────────────────────────────────────────────
piece_sym() {
    case "$1" in
        K|k) printf "♚";; Q|q) printf "♛";; R|r) printf "♜";;
        B|b) printf "♝";; N|n) printf "♞";; P|p) printf "♟";; *) printf " ";;
    esac
}

show_captured_row() {
    local label="$1" col_code="$2" caps="$3"
    printf "  %b%s%b " "$col_code" "$label" "$RESET"
    if [ -n "$caps" ]; then
        local IFS='|'; local -a arr=($caps)
        local count=${#arr[@]}
        for p in "${arr[@]}"; do
            printf "%b%b%b " "$col_code" "$(piece_sym "$p")" "$RESET"
        done
        printf "(total: %d)\n" "$count"
    else
        printf "%b(belum ada)%b\n" "$DIM" "$RESET"
    fi
}

# ── Gambar papan ──────────────────────────────────────────────
CURRENT_PLAYER="w"
draw_board() {
    read_state

    # Baca posisi bidak dari state file (BOARD field)
    declare -A bd
    local r c
    for r in 0 1 2 3 4 5 6 7; do
        for c in 0 1 2 3 4 5 6 7; do bd[$r,$c]=".:NONE"; done
    done

    local board_str
    board_str=$(grep "^BOARD:" "$STATE_FILE" 2>/dev/null | cut -d: -f2-)
    local IFS='|'
    local -a pieces=($board_str)
    IFS=' '
    local pf
    for pf in "${pieces[@]}"; do
        IFS=',' read -r sp sc sr scl <<< "$pf"
        [[ "$sr" =~ ^[0-7]$ ]] && [[ "$scl" =~ ^[0-7]$ ]] || continue
        bd[$sr,$scl]="${sp}:${sc}"
    done

    clear

    # Header — tampilkan perspektif pemain
    printf "${BOLD}${CYAN}  ╔════════════════════════════════════════════╗\n"
    printf "  ║        ♟  GAME CATUR LINUX — %s ♟       ║\n" "$MY_NAME"
    printf "  ╚════════════════════════════════════════════╝${RESET}\n\n"

    local cur_name
    [ "$CURRENT_PLAYER" = "w" ] && cur_name="WHITE" || cur_name="BLACK"
    local chk_str=""
    [ $IN_CHECK -eq 1 ] && chk_str="${RED}${BOLD} ← SKAK!${RESET}"
    printf "  ${DIM}Giliran:${RESET} ${BOLD}%s${RESET}%b\n" "$cur_name" "$chk_str"
    printf "  ${DIM}Move #${RESET}${BOLD}%d${RESET}    ${DIM}Anda:${RESET}${BOLD}%s${RESET}\n\n" \
           "$MOVE_COUNT" "$MY_NAME"

    # Panel captured
    printf "  ┌───────────────────────────────────────────┐\n"
    printf "  │ ${BOLD}Bidak yang Ditangkap${RESET}                       │\n"
    printf "  │ "; show_captured_row "Putih tangkap:" "$WHITE" "$WHITE_CAPTURED"
    printf "  │ "; show_captured_row "Hitam tangkap:" "$RED"   "$BLACK_CAPTURED"
    printf "  └───────────────────────────────────────────┘\n\n"

    # Papan — WHITE melihat dari bawah (normal), BLACK melihat dari atas (terbalik)
    if [ "$PLAYER" = "w" ]; then
        printf "       a   b   c   d   e   f   g   h\n"
        printf "     ┌───┬───┬───┬───┬───┬───┬───┬───┐\n"
        local dr dc rank
        for dr in 0 1 2 3 4 5 6 7; do
            rank=$(( 8 - dr ))
            printf "   %d │" "$rank"
            _draw_row $dr; printf " %d\n" "$rank"
            [ $dr -lt 7 ] && printf "     ├───┼───┼───┼───┼───┼───┼───┼───┤\n"
        done
        printf "     └───┴───┴───┴───┴───┴───┴───┴───┘\n"
        printf "       a   b   c   d   e   f   g   h\n\n"
    else
        # BLACK melihat papan terbalik (hitam di bawah)
        printf "       h   g   f   e   d   c   b   a\n"
        printf "     ┌───┬───┬───┬───┬───┬───┬───┬───┐\n"
        local dr rank
        for dr in 7 6 5 4 3 2 1 0; do
            rank=$(( 8 - dr ))
            printf "   %d │" "$rank"
            _draw_row_rev $dr; printf " %d\n" "$rank"
            [ $dr -gt 0 ] && printf "     ├───┼───┼───┼───┼───┼───┼───┼───┤\n"
        done
        printf "     └───┴───┴───┴───┴───┴───┴───┴───┘\n"
        printf "       h   g   f   e   d   c   b   a\n\n"
    fi

    # Pesan
    if [ -n "$LAST_MSG" ]; then
        printf "  %b\n\n" "$LAST_MSG"; LAST_MSG=""
    fi

    # Status giliran
    if [ "$CURRENT_PLAYER" = "$PLAYER" ]; then
        printf "  ${GREEN}${BOLD}► Giliran Anda!${RESET}\n"
    else
        printf "  ${DIM}► Menunggu ${OPP_NAME} bermain...${RESET}\n"
    fi
}

_draw_row() {
    local dr=$1 dc cell p cl is_dark sym
    for dc in 0 1 2 3 4 5 6 7; do
        cell="${bd[$dr,$dc]}"; p="${cell%%:*}"; cl="${cell##*:}"
        is_dark=$(( (dr + dc) % 2 ))
        if [ "$p" = "." ] || [ -z "$p" ]; then
            [ $is_dark -eq 1 ] && printf "${DIM}   ${RESET}│" || printf "   │"
        else
            sym=$(piece_sym "$p")
            if [ "$cl" = "w" ]; then
                [ $is_dark -eq 1 ] && printf "${DIM}${WHITE} %b ${RESET}│" "$sym" \
                                   || printf "${WHITE} %b ${RESET}│" "$sym"
            else
                [ $is_dark -eq 1 ] && printf "${DIM}${RED} %b ${RESET}│" "$sym" \
                                   || printf "${RED} %b ${RESET}│" "$sym"
            fi
        fi
    done
}

_draw_row_rev() {
    # Untuk BLACK: tampilkan kolom terbalik (7..0)
    local dr=$1 dc cell p cl is_dark sym
    for dc in 7 6 5 4 3 2 1 0; do
        cell="${bd[$dr,$dc]}"; p="${cell%%:*}"; cl="${cell##*:}"
        is_dark=$(( (dr + dc) % 2 ))
        if [ "$p" = "." ] || [ -z "$p" ]; then
            [ $is_dark -eq 1 ] && printf "${DIM}   ${RESET}│" || printf "   │"
        else
            sym=$(piece_sym "$p")
            if [ "$cl" = "w" ]; then
                [ $is_dark -eq 1 ] && printf "${DIM}${WHITE} %b ${RESET}│" "$sym" \
                                   || printf "${WHITE} %b ${RESET}│" "$sym"
            else
                [ $is_dark -eq 1 ] && printf "${DIM}${RED} %b ${RESET}│" "$sym" \
                                   || printf "${RED} %b ${RESET}│" "$sym"
            fi
        fi
    done
}

# ── Parse notasi catur ────────────────────────────────────────
# FIX #5: Client hanya memvalidasi format dan mengembalikan notasi bersih.
# Server yang melakukan konversi ke koordinat di process_move().
parse_move() {
    local raw="${1//-/}"; raw="${raw// /}"
    [[ "$raw" =~ ^[a-h][1-8][a-h][1-8]$ ]] || return 1
    printf "%s" "$raw"
}

# ── Terima notifikasi dari server (background) ────────────────
# FIX #5: Fungsi ini sekarang dipanggil di main() setelah GAME_START diterima.
start_notify_listener() {
    (
        while true; do
            local msg
            # Timeout 60 detik per read — jika server mati, loop ini berhenti
            if read -r -t 60 msg < "$MY_NOTIFY" 2>/dev/null; then
                printf "%s\n" "$msg" > "/tmp/${GAME_ID}_notify_buf_${PLAYER}"
            fi
            # Berhenti jika state file sudah tidak ada (server mati)
            [ -f "/tmp/${GAME_ID}_state" ] || break
        done
    ) &
    NOTIFY_PID=$!
}

# ── Cek notifikasi terbaru ────────────────────────────────────
check_notify() {
    local buf="/tmp/${GAME_ID}_notify_buf_${PLAYER}"
    [ -f "$buf" ] || return
    local msg; msg=$(cat "$buf"); rm -f "$buf"
    printf "%s" "$msg"
}

# ── Main client ───────────────────────────────────────────────
main() {
    clear
    printf "${BOLD}${CYAN}"
    printf "  ╔════════════════════════════════════════════╗\n"
    printf "  ║     ♟  GAME CATUR LINUX — CLIENT  ♟       ║\n"
    printf "  ╚════════════════════════════════════════════╝${RESET}\n\n"
    printf "  Anda bermain sebagai: ${BOLD}%s${RESET}\n\n" "$MY_NAME"

    # Cek server aktif
    if [ ! -p "$MY_PIPE" ]; then
        printf "${RED}ERROR: Server belum berjalan!\n"
        printf "Jalankan dulu: bash chess_server.sh${RESET}\n"
        exit 1
    fi

    printf "  ${DIM}Menghubungkan ke server...${RESET}\n"
    echo "CONNECT:${PLAYER}" > "$MY_PIPE"

    # Tunggu konfirmasi dari server
    local srv_msg
    read -r -t 30 srv_msg < "$MY_NOTIFY"
    case "$srv_msg" in
        CONNECTED:*:WAIT_BLACK)
            printf "  ${GREEN}Terhubung! Menunggu BLACK bergabung...${RESET}\n"
            read -r -t 120 srv_msg < "$MY_NOTIFY" ;;
        CONNECTED:*) ;;
    esac

    if [ "$srv_msg" != "GAME_START" ]; then
        printf "  ${YELLOW}Menunggu game dimulai...${RESET}\n"
        read -r -t 120 srv_msg < "$MY_NOTIFY"
    fi

    if [ "$srv_msg" = "GAME_START" ]; then
        printf "  ${GREEN}${BOLD}Game dimulai!${RESET}\n"; sleep 1
    else
        printf "  ${RED}Timeout atau server error: %s${RESET}\n" "$srv_msg"; exit 1
    fi

    GAME_ACTIVE=1
    log_msg "Game dimulai"
    # FIX #5: Jalankan listener notifikasi background SEKARANG agar
    # tidak berkompetisi dengan read langsung di game loop giliran lawan
    start_notify_listener

    # ── Game loop client ──────────────────────────────────────
    while [ $GAME_ACTIVE -eq 1 ]; do
        read_state
        draw_board

        local cur; cur=$(grep "^CURRENT_PLAYER:" "$STATE_FILE" 2>/dev/null | cut -d: -f2)

        if [ "$cur" = "$PLAYER" ]; then
            # ── Giliran saya — minta input ────────────────────
            printf "  ${DIM}Gerakan (cth: e2e4) / resign / quit:${RESET}\n  > "

            local input=""
            if ! read -r -t 120 input; then
                printf "\n${YELLOW}Timeout!${RESET}\n"; break
            fi
            input="${input,,}"

            case "$input" in
                quit|q|exit)
                    GAME_ACTIVE=0; break ;;
                resign)
                    echo "RESIGN" > "$MY_PIPE"
                    printf "${RED}Anda menyerah. ${OPP_NAME} menang.${RESET}\n"
                    sleep 2; GAME_ACTIVE=0; break ;;
                help)
                    printf "\n  ${BOLD}Format:${RESET} e2e4 (kolom+baris ke kolom+baris)\n"
                    printf "  ${BOLD}Contoh:${RESET} e2e4, g1f3, d7d5\n"
                    printf "  Tekan Enter..."; read -r; continue ;;
                "") continue ;;
            esac

            # Validasi format
            local notation
            if ! notation=$(parse_move "$input"); then
                LAST_MSG="${RED}✗ Format salah! Contoh: e2e4${RESET}"
                continue
            fi

            # Kirim move ke server
            log_msg "Send move: $notation"
            echo "$notation" > "$MY_PIPE"

            # FIX #5: Tunggu respons dari buffer file (ditulis listener background).
            # Listener sudah memegang pipe, jadi kita tidak bisa read langsung.
            local resp="" buf_file="/tmp/${GAME_ID}_notify_buf_${PLAYER}"
            local waited=0
            while [ $waited -lt 10 ] && [ -z "$resp" ]; do
                sleep 0.2; waited=$(( waited + 1 ))
                if [ -f "$buf_file" ]; then
                    resp=$(cat "$buf_file" 2>/dev/null)
                    rm -f "$buf_file"
                fi
            done
            [ -z "$resp" ] && resp="TIMEOUT"

            case "$resp" in
                UPDATE:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    if [ "${parts[3]}" = "CHECK" ]; then
                        IN_CHECK=1
                        LAST_MSG="${YELLOW}${BOLD}⚠ SKAK! Raja lawan dalam bahaya!${RESET}"
                    else
                        IN_CHECK=0
                        LAST_MSG="${GREEN}✓ Gerakan ${parts[2]} berhasil${RESET}"
                    fi ;;
                ERROR:*)
                    local err="${resp#ERROR:}"
                    case "$err" in
                        INVALID_PAWN)         LAST_MSG="${RED}✗ Pion tidak valid! (maju lurus, makan diagonal)${RESET}" ;;
                        WRONG_COLOR)          LAST_MSG="${RED}✗ Bukan bidak Anda!${RESET}" ;;
                        DEST_OCCUPIED)        LAST_MSG="${RED}✗ Petak sudah ditempati bidak Anda!${RESET}" ;;
                        LEAVES_KING_IN_CHECK) LAST_MSG="${RED}✗ Raja Anda akan tetap dalam bahaya!${RESET}" ;;
                        INVALID_ROOK|ROOK_BLOCKED)     LAST_MSG="${RED}✗ Benteng: hanya lurus!${RESET}" ;;
                        INVALID_BISHOP|BISHOP_BLOCKED) LAST_MSG="${RED}✗ Gajah: hanya diagonal!${RESET}" ;;
                        INVALID_KNIGHT)       LAST_MSG="${RED}✗ Kuda: gerakan L (2+1)!${RESET}" ;;
                        INVALID_QUEEN|QUEEN_BLOCKED)   LAST_MSG="${RED}✗ Ratu: lurus atau diagonal!${RESET}" ;;
                        INVALID_KING)         LAST_MSG="${RED}✗ Raja: maksimal 1 kotak!${RESET}" ;;
                        *)                    LAST_MSG="${RED}✗ Tidak valid: $err${RESET}" ;;
                    esac ;;
                GAMEOVER:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    local winner="${parts[2]}" reason="${parts[3]}"
                    draw_board
                    if [ "$winner" = "$PLAYER" ]; then
                        printf "\n  ${GREEN}${BOLD}╔════════════════════════════╗\n"
                        printf "  ║   SELAMAT! ANDA MENANG!   ║\n"
                        printf "  ╚════════════════════════════╝${RESET}\n"
                    else
                        printf "\n  ${RED}${BOLD}╔════════════════════════════╗\n"
                        printf "  ║   ANDA KALAH.             ║\n"
                        printf "  ╚════════════════════════════╝${RESET}\n"
                    fi
                    printf "  ${DIM}Alasan: %s${RESET}\n\n" "$reason"
                    printf "  Tekan Enter..."; read -r
                    GAME_ACTIVE=0; break ;;
                SERVER_SHUTDOWN)
                    printf "\n${RED}Server dimatikan.${RESET}\n"
                    GAME_ACTIVE=0; break ;;
                TIMEOUT|"")
                    LAST_MSG="${YELLOW}⚠ Tidak ada respons dari server. Coba lagi.${RESET}" ;;
            esac

        else
            # ── Bukan giliran saya — tunggu update ───────────
            printf "\n  ${DIM}Menunggu giliran %s...${RESET}\n" "$OPP_NAME"
            printf "  ${DIM}(ketik 'quit' lalu Enter untuk keluar)${RESET}\n  > "

            # FIX #5: Baca dari buffer file yang ditulis start_notify_listener(),
            # bukan langsung dari pipe (listener sudah memegang pipe tersebut).
            local resp="" buf_file="/tmp/${GAME_ID}_notify_buf_${PLAYER}"
            local input=""
            while [ -z "$resp" ] && [ $GAME_ACTIVE -eq 1 ]; do
                # Cek buffer notifikasi yang ditulis listener
                if [ -f "$buf_file" ]; then
                    resp=$(cat "$buf_file" 2>/dev/null)
                    rm -f "$buf_file"
                    [ -n "$resp" ] && break
                fi
                # Cek apakah user mengetik sesuatu (quit)
                if read -r -t 0.3 input 2>/dev/null; then
                    input="${input,,}"
                    if [ "$input" = "quit" ] || [ "$input" = "q" ]; then
                        GAME_ACTIVE=0; break
                    fi
                fi
            done

            [ $GAME_ACTIVE -eq 0 ] && break

            case "$resp" in
                UPDATE:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    IN_CHECK=0
                    if [ "${parts[3]}" = "CHECK" ]; then
                        IN_CHECK=1
                        LAST_MSG="${YELLOW}${BOLD}⚠ SKAK! Raja Anda dalam bahaya!${RESET}"
                    else
                        LAST_MSG="${CYAN}Lawan bermain: ${parts[2]}${RESET}"
                    fi ;;
                GAMEOVER:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    local winner="${parts[2]}" reason="${parts[3]}"
                    draw_board
                    if [ "$winner" = "$PLAYER" ]; then
                        printf "\n  ${GREEN}${BOLD}╔════════════════════════════╗\n"
                        printf "  ║   SELAMAT! ANDA MENANG!   ║\n"
                        printf "  ╚════════════════════════════╝${RESET}\n"
                    else
                        printf "\n  ${RED}${BOLD}╔════════════════════════════╗\n"
                        printf "  ║   ANDA KALAH.             ║\n"
                        printf "  ╚════════════════════════════╝${RESET}\n"
                    fi
                    printf "  ${DIM}Alasan: %s${RESET}\n\n" "$reason"
                    printf "  Tekan Enter..."; read -r
                    GAME_ACTIVE=0; break ;;
                SERVER_SHUTDOWN)
                    printf "\n${RED}Server dimatikan.${RESET}\n"
                    GAME_ACTIVE=0; break ;;
            esac
        fi
    done

    printf "\n${YELLOW}Terima kasih sudah bermain!${RESET}\n"
    log_msg "Game selesai"
}

main
