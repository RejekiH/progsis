#!/bin/bash
# chess.sh — FIXED: captured pieces display + unicode width
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE="$SCRIPT_DIR/chess_engine.awk"
STATE_FILE="/tmp/chess_state_$$"
LOG_FILE="/tmp/chess_log_$$"

RED=$'\033[0;31m';  GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; WHITE=$'\033[1;37m'; BOLD=$'\033[1m'
DIM=$'\033[2m';     RESET=$'\033[0m'

GAME_ACTIVE=0; THREAD_PID=0
CURRENT_PLAYER="w"; MOVE_COUNT=0; PAUSE_FLAG=0
IN_CHECK=0; WHITE_CAPTURED=""; BLACK_CAPTURED=""; LAST_MSG=""

# ── Signal ───────────────────────────────────────────────────
cleanup() {
    GAME_ACTIVE=0
    [ $THREAD_PID -gt 0 ] && kill $THREAD_PID 2>/dev/null
    rm -f "$STATE_FILE" "$STATE_FILE.tmp"
    printf "\n${YELLOW}Game dihentikan. Terima kasih!${RESET}\n"; exit 0
}
handle_sigint() {
    printf "\n${YELLOW}[SIGINT] Ingin keluar? (y/n): ${RESET}"
    read -r -t 5 ans; [[ "$ans" =~ ^[Yy]$ ]] && cleanup
    printf "${GREEN}Melanjutkan...${RESET}\n"
}
handle_sigusr1() {
    [ $PAUSE_FLAG -eq 0 ] \
        && { PAUSE_FLAG=1; printf "\n${YELLOW}[USR1] PAUSED${RESET}\n"; } \
        || { PAUSE_FLAG=0; printf "\n${GREEN}[USR1] RESUMED${RESET}\n"; }
}
handle_sigusr2() {
    printf "\n${CYAN}=== STATISTIK ===\nMove:%d Player:%s PID:%d Thread:%d${RESET}\n" \
           "$MOVE_COUNT" "$(player_name "$CURRENT_PLAYER")" "$$" "$THREAD_PID"
}
trap 'handle_sigint'  SIGINT
trap 'cleanup'        SIGTERM EXIT
trap 'handle_sigusr1' SIGUSR1
trap 'handle_sigusr2' SIGUSR2

log_msg()     { printf "[%s][%s] %s\n" "$(date '+%H:%M:%S')" "$1" "$2" >> "$LOG_FILE"; }
player_name() { [ "$1" = "w" ] && printf "WHITE" || printf "BLACK"; }

# ── State file (shared memory) ────────────────────────────────
init_state() {
    cat > "$STATE_FILE" << 'EOF'
MOVE_COUNT:0
CURRENT_PLAYER:w
STATUS:ACTIVE
WHITE_CAPTURED:
BLACK_CAPTURED:
BOARD:r,b,0,0|n,b,0,1|b,b,0,2|q,b,0,3|k,b,0,4|b,b,0,5|n,b,0,6|r,b,0,7|p,b,1,0|p,b,1,1|p,b,1,2|p,b,1,3|p,b,1,4|p,b,1,5|p,b,1,6|p,b,1,7|P,w,6,0|P,w,6,1|P,w,6,2|P,w,6,3|P,w,6,4|P,w,6,5|P,w,6,6|P,w,6,7|R,w,7,0|N,w,7,1|B,w,7,2|Q,w,7,3|K,w,7,4|B,w,7,5|N,w,7,6|R,w,7,7
EOF
}

# ── Thread monitor ────────────────────────────────────────────
start_thread_monitor() {
    (
        log_msg "THREAD" "Monitor dimulai PID:$$"
        while true; do
            sleep 3
            [ -f "$STATE_FILE" ] || break
            local mc st
            mc=$(grep "^MOVE_COUNT:" "$STATE_FILE" 2>/dev/null | cut -d: -f2)
            # FIX #6: Cek STATUS dari state file — bukan variabel GAME_ACTIVE.
            # Subshell tidak mewarisi perubahan variabel dari parent shell,
            # jadi membaca GAME_ACTIVE di sini selalu bernilai awal (0).
            st=$(grep "^STATUS:" "$STATE_FILE" 2>/dev/null | cut -d: -f2)
            [ "$st" = "GAMEOVER" ] || [ "$st" = "SHUTDOWN" ] && break
            log_msg "THREAD" "Heartbeat move=$mc status=$st"
        done
        log_msg "THREAD" "Monitor selesai"
    ) &
    THREAD_PID=$!
}

# ── Panggil engine AWK (sekali per move, tanpa pipe) ──────────
run_engine() {
    awk -f "$ENGINE" \
        -v cmd="$1" -v player="${2:-w}" \
        -v fr="${3:-0}" -v fc="${4:-0}" \
        -v tr="${5:-0}" -v tc="${6:-0}" \
        -v state_file="$STATE_FILE" /dev/null
}

# ── Baca state dari file ke variabel global ───────────────────
read_state() {
    local line
    MOVE_COUNT=0; WHITE_CAPTURED=""; BLACK_CAPTURED=""
    while IFS= read -r line; do
        case "$line" in
            MOVE_COUNT:*)    MOVE_COUNT="${line#MOVE_COUNT:}" ;;
            WHITE_CAPTURED:*) WHITE_CAPTURED="${line#WHITE_CAPTURED:}" ;;
            BLACK_CAPTURED:*) BLACK_CAPTURED="${line#BLACK_CAPTURED:}" ;;
        esac
    done < "$STATE_FILE"
}

# ── Simbol bidak ──────────────────────────────────────────────
piece_sym() {
    case "$1" in
        K|k) printf "♚";; Q|q) printf "♛";; R|r) printf "♜";;
        B|b) printf "♝";; N|n) printf "♞";; P|p) printf "♟";; *) printf " ";;
    esac
}

# ── Tampilkan baris captured ──────────────────────────────────
show_captured_row() {
    local label="$1" col_code="$2" caps="$3"
    printf "  %b%s%b " "$col_code" "$label" "$RESET"
    if [ -n "$caps" ]; then
        local IFS='|'
        local -a arr=($caps)
        local count=${#arr[@]}
        local i p sym
        for i in "${!arr[@]}"; do
            p="${arr[$i]}"
            sym=$(piece_sym "$p")
            printf "%b%b%b" "$col_code" "$sym" "$RESET"
            # Spasi antar bidak
            printf " "
        done
        printf "(total: %d)\n" "$count"
    else
        printf "%b(belum ada)%b\n" "$DIM" "$RESET"
    fi
}

# ── Gambar papan ──────────────────────────────────────────────
draw_board() {
    read_state

    # Baca posisi bidak dari engine
    declare -A bd
    local r c
    for r in 0 1 2 3 4 5 6 7; do
        for c in 0 1 2 3 4 5 6 7; do bd[$r,$c]=".:."; done
    done
    local sp sc sr sc2
    while IFS=' ' read -r sp sc sr sc2; do
        [[ "$sr" =~ ^[0-7]$ ]] && [[ "$sc2" =~ ^[0-7]$ ]] || continue
        bd[$sr,$sc2]="${sp}:${sc}"
    done < <(run_engine "BOARD")

    clear

    # Header
    printf "${BOLD}${CYAN}  ╔═══════════════════════════════════════╗\n"
    printf "  ║        ♟  GAME CATUR LINUX  ♟        ║\n"
    printf "  ╚═══════════════════════════════════════╝${RESET}\n\n"

    local pname chk_str=""
    pname=$(player_name "$CURRENT_PLAYER")
    [ $IN_CHECK -eq 1 ] && chk_str="${RED}${BOLD}  ← SKAK! Selamatkan Raja!${RESET}"
    printf "  ${DIM}Giliran:${RESET} ${BOLD}%s${RESET}%b\n" "$pname" "$chk_str"
    printf "  ${DIM}Move #${BOLD}%d${RESET}   ${DIM}PID:${BOLD}%d${RESET}   ${DIM}Thread:${BOLD}%d${RESET}\n\n" \
           "$MOVE_COUNT" "$$" "$THREAD_PID"

    # Panel captured (dibaca langsung dari variabel global)
    printf "  ┌─────────────────────────────────────────┐\n"
    printf "  │ ${BOLD}Bidak yang Ditangkap${RESET}                     │\n"
    printf "  │ "
    show_captured_row "Putih tangkap:" "$WHITE" "$WHITE_CAPTURED"
    printf "  │ "
    show_captured_row "Hitam tangkap:" "$RED"   "$BLACK_CAPTURED"
    printf "  └─────────────────────────────────────────┘\n\n"

    # Papan 8x8 — sel 3 karakter: " simbol "
    printf "       a   b   c   d   e   f   g   h\n"
    printf "     ┌───┬───┬───┬───┬───┬───┬───┬───┐\n"

    local dr dc rank cell p cl is_dark sym
    for dr in 0 1 2 3 4 5 6 7; do
        rank=$(( 8 - dr ))
        printf "   %d │" "$rank"
        for dc in 0 1 2 3 4 5 6 7; do
            cell="${bd[$dr,$dc]}"
            p="${cell%%:*}"; cl="${cell##*:}"
            is_dark=$(( (dr + dc) % 2 ))
            if [ "$p" = "." ] || [ -z "$p" ]; then
                [ $is_dark -eq 1 ] && printf "${DIM}   ${RESET}│" || printf "   │"
            else
                sym=$(piece_sym "$p")
                if [ "$cl" = "w" ]; then
                    [ $is_dark -eq 1 ] \
                        && printf "${DIM}${WHITE} %b ${RESET}│" "$sym" \
                        || printf "${WHITE} %b ${RESET}│" "$sym"
                else
                    [ $is_dark -eq 1 ] \
                        && printf "${DIM}${RED} %b ${RESET}│" "$sym" \
                        || printf "${RED} %b ${RESET}│" "$sym"
                fi
            fi
        done
        printf " %d\n" "$rank"
        [ $dr -lt 7 ] && printf "     ├───┼───┼───┼───┼───┼───┼───┼───┤\n"
    done
    printf "     └───┴───┴───┴───┴───┴───┴───┴───┘\n"
    printf "       a   b   c   d   e   f   g   h\n\n"

    # Pesan terakhir
    if [ -n "$LAST_MSG" ]; then
        printf "  %b\n\n" "$LAST_MSG"
        LAST_MSG=""
    fi
}

# ── Parse notasi catur ────────────────────────────────────────
parse_move() {
    local raw="${1//-/}"; raw="${raw// /}"
    [[ "$raw" =~ ^[a-h][1-8][a-h][1-8]$ ]] || return 1
    local fc fr tc tr
    fc=$(( $(printf '%d' "'${raw:0:1}") - 97 ))
    fr=$(( 8 - ${raw:1:1} ))
    tc=$(( $(printf '%d' "'${raw:2:1}") - 97 ))
    tr=$(( 8 - ${raw:3:1} ))
    printf "%d %d %d %d" "$fr" "$fc" "$tr" "$tc"
}

# ── Proses respons engine ─────────────────────────────────────
handle_response() {
    local resp="$1"
    local status="" cap="" chk=0 mate=0 next="" wcap="" bcap="" mc=""

    while IFS= read -r line; do
        case "$line" in
            OK)                 status="OK" ;;
            ERROR:*)            status="${line#ERROR:}" ;;
            CAPTURED:*)         cap="${line#CAPTURED:}" ;;
            CHECK:1)            chk=1 ;;
            CHECKMATE:1)        mate=1 ;;
            NEXT_PLAYER:*)      next="${line#NEXT_PLAYER:}" ;;
            WHITE_CAPTURED:*)   wcap="${line#WHITE_CAPTURED:}" ;;
            BLACK_CAPTURED:*)   bcap="${line#BLACK_CAPTURED:}" ;;
            MOVE_COUNT:*)       mc="${line#MOVE_COUNT:}" ;;
        esac
    done <<< "$resp"

    if [ "$status" = "OK" ]; then
        CURRENT_PLAYER="$next"
        IN_CHECK=$chk
        MOVE_COUNT="${mc:-$MOVE_COUNT}"
        # Update captured dari respons engine (sudah terakumulasi di engine)
        WHITE_CAPTURED="$wcap"
        BLACK_CAPTURED="$bcap"

        if [ -n "$cap" ] && [ "$cap" != "." ]; then
            local csym; csym=$(piece_sym "$cap")
            LAST_MSG="${GREEN}✓ Berhasil — menangkap ${csym}!${RESET}"
        else
            LAST_MSG="${GREEN}✓ Gerakan berhasil${RESET}"
        fi

        if [ $mate -eq 1 ]; then
            draw_board
            local winner; [ "$CURRENT_PLAYER" = "w" ] && winner="BLACK" || winner="WHITE"
            printf "\n${RED}${BOLD}  ╔═══════════════════════════════════╗\n"
            printf "  ║  SKAKMAT!  %-6s MENANG!          ║\n" "$winner"
            printf "  ╚═══════════════════════════════════╝${RESET}\n\n"
            printf "  Tekan Enter..."; read -r
            GAME_ACTIVE=0; return
        fi

        [ $chk -eq 1 ] && \
            LAST_MSG="${YELLOW}${BOLD}⚠ SKAK! Raja $(player_name "$CURRENT_PLAYER") dalam bahaya!${RESET}"
    else
        case "$status" in
            EMPTY_SOURCE)          LAST_MSG="${RED}✗ Tidak ada bidak di petak itu!${RESET}" ;;
            WRONG_COLOR)           LAST_MSG="${RED}✗ Itu bidak lawan! Giliran: $(player_name "$CURRENT_PLAYER")${RESET}" ;;
            DEST_OCCUPIED)         LAST_MSG="${RED}✗ Petak tujuan sudah ada bidak Anda!${RESET}" ;;
            INVALID_PAWN)          LAST_MSG="${RED}✗ Pion: maju lurus 1/2 kotak, makan diagonal hanya jika ada musuh!${RESET}" ;;
            ROOK_BLOCKED|INVALID_ROOK)      LAST_MSG="${RED}✗ Benteng: hanya lurus, tidak bisa melewati bidak lain!${RESET}" ;;
            BISHOP_BLOCKED|INVALID_BISHOP)  LAST_MSG="${RED}✗ Gajah: hanya diagonal, tidak bisa melewati bidak lain!${RESET}" ;;
            INVALID_KNIGHT)        LAST_MSG="${RED}✗ Kuda: bergerak L (2+1 kotak)!${RESET}" ;;
            QUEEN_BLOCKED|INVALID_QUEEN)    LAST_MSG="${RED}✗ Ratu: lurus atau diagonal, tidak bisa melewati bidak!${RESET}" ;;
            INVALID_KING)          LAST_MSG="${RED}✗ Raja: maksimal 1 kotak ke segala arah!${RESET}" ;;
            LEAVES_KING_IN_CHECK)  LAST_MSG="${RED}✗ Tidak bisa! Raja Anda tetap dalam bahaya!${RESET}" ;;
            OUT_OF_BOUNDS)         LAST_MSG="${RED}✗ Posisi di luar papan!${RESET}" ;;
            *)                     LAST_MSG="${RED}✗ Tidak valid: ${status}${RESET}" ;;
        esac
        log_msg "INVALID" "$status"
    fi
}

show_menu() {
    clear
    printf "${BOLD}${CYAN}  ╔═══════════════════════════════════════╗\n"
    printf "  ║        ♟  GAME CATUR LINUX  ♟        ║\n"
    printf "  ║  Shell+AWK+Signal+Pipe+Thread+SHM    ║\n"
    printf "  ╚═══════════════════════════════════════╝${RESET}\n\n"
    printf "  ${GREEN}[1]${RESET} Mulai Game Baru (2 Pemain)\n"
    printf "  ${GREEN}[2]${RESET} Aturan & Bantuan\n"
    printf "  ${GREEN}[3]${RESET} Lihat Log Sistem\n"
    printf "  ${RED}[4]${RESET} Keluar\n\n"
    printf "  ${DIM}PID: $$\n"
    printf "  kill -USR1 $$ → pause/resume\n"
    printf "  kill -USR2 $$ → statistik${RESET}\n\n"
    printf "  Pilihan: "
}

show_help() {
    clear
    printf "${BOLD}${CYAN}═══════════════════════════════════════${RESET}\n"
    printf "${BOLD}   ATURAN CATUR & PANDUAN BERMAIN${RESET}\n"
    printf "${BOLD}${CYAN}═══════════════════════════════════════${RESET}\n\n"
    printf "${BOLD}Format:${RESET}  e2e4  atau  e2-e4\n\n"
    printf "${BOLD}Aturan bidak:${RESET}\n"
    printf "  ♟ Pion    : Maju 1 kotak (2 dari posisi awal)\n"
    printf "              Makan HANYA diagonal jika ada musuh\n"
    printf "  ♜ Benteng : Lurus (baris/kolom), tidak bisa melompat\n"
    printf "  ♝ Gajah   : Diagonal, tidak bisa melompat\n"
    printf "  ♞ Kuda    : Huruf L (2+1), BISA melompati bidak lain\n"
    printf "  ♛ Ratu    : Lurus + diagonal, tidak bisa melompat\n"
    printf "  ♚ Raja    : 1 kotak, tidak bisa ke petak yang diserang\n\n"
    printf "  ${WHITE}Putih${RESET} = WHITE   ${RED}Merah${RESET} = BLACK\n\n"
    printf "${BOLD}Skak & Skakmat:${RESET}\n"
    printf "  Raja tidak bisa dimakan langsung\n"
    printf "  Saat SKAK → wajib selamatkan Raja\n"
    printf "  Tidak ada gerakan penyelamat → SKAKMAT\n\n"
    printf "${BOLD}Perintah:${RESET}  help  resign  log  quit\n\n"
    printf "Tekan Enter untuk kembali... "; read -r
}

game_loop() {
    GAME_ACTIVE=1; CURRENT_PLAYER="w"; MOVE_COUNT=0
    IN_CHECK=0; WHITE_CAPTURED=""; BLACK_CAPTURED=""; LAST_MSG=""
    init_state
    log_msg "INFO" "Game dimulai PID:$$ Thread:$THREAD_PID"
    printf "${GREEN}Game dimulai! PID: $$${RESET}\n"; sleep 1

    while [ $GAME_ACTIVE -eq 1 ]; do
        while [ $PAUSE_FLAG -eq 1 ]; do sleep 0.3; done
        draw_board

        local pname; pname=$(player_name "$CURRENT_PLAYER")
        printf "  ${BOLD}Giliran %s${RESET}\n" "$pname"
        printf "  ${DIM}Gerakan (cth: e2e4) / help / resign / quit:${RESET}\n  > "

        local input=""
        if ! read -r -t 300 input; then
            printf "\n${YELLOW}Timeout!${RESET}\n"; break
        fi
        input="${input,,}"

        case "$input" in
            quit|q|exit) GAME_ACTIVE=0; break ;;
            resign)
                local w; [ "$CURRENT_PLAYER" = "w" ] && w="BLACK" || w="WHITE"
                draw_board
                printf "\n  ${RED}${BOLD}$(player_name "$CURRENT_PLAYER") menyerah!${RESET}\n"
                printf "  ${GREEN}${BOLD}%s MENANG!${RESET}\n\n" "$w"
                sleep 3; GAME_ACTIVE=0; break ;;
            help|h) show_help; continue ;;
            log)
                clear; printf "${CYAN}=== LOG ===${RESET}\n"
                tail -30 "$LOG_FILE" 2>/dev/null || printf "(kosong)\n"
                printf "\nEnter..."; read -r; continue ;;
            "") continue ;;
        esac

        local coords
        if ! coords=$(parse_move "$input"); then
            LAST_MSG="${RED}✗ Format salah! Gunakan: e2e4 atau e2-e4${RESET}"
            continue
        fi

        local fr fc tr tc
        read -r fr fc tr tc <<< "$coords"
        log_msg "MOVE" "$(player_name "$CURRENT_PLAYER"): $input ($fr,$fc)→($tr,$tc)"

        local resp
        resp=$(run_engine "MOVE" "$CURRENT_PLAYER" "$fr" "$fc" "$tr" "$tc")
        handle_response "$resp"
    done

    GAME_ACTIVE=0
    # FIX #6: Tulis STATUS=GAMEOVER ke state file agar thread monitor tahu harus berhenti
    if [ -f "$STATE_FILE" ]; then
        sed -i "s|^STATUS:.*|STATUS:GAMEOVER|" "$STATE_FILE" 2>/dev/null
    fi
    log_msg "INFO" "Game selesai. Total move: $MOVE_COUNT"
}

main() {
    touch "$LOG_FILE"
    log_msg "INFO" "Start PID:$$  Bash:$BASH_VERSION"
    if [ ! -f "$ENGINE" ]; then
        printf "${RED}ERROR: chess_engine.awk tidak ditemukan di:\n  %s${RESET}\n" "$ENGINE"
        exit 1
    fi
    start_thread_monitor

    while true; do
        show_menu; local ch; read -r ch
        case "$ch" in
            1) game_loop ;;
            2) show_help ;;
            3) clear; printf "${CYAN}=== LOG ===${RESET}\n"
               cat "$LOG_FILE" 2>/dev/null || printf "(kosong)\n"
               printf "\nEnter..."; read -r ;;
            4|q) printf "${YELLOW}Sampai jumpa!${RESET}\n"; break ;;
            *) printf "${RED}Tidak valid${RESET}\n"; sleep 1 ;;
        esac
    done
}

main
