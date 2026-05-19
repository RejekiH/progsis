#!/bin/bash
# ============================================================
# chess_server.sh — Server Game Catur Dua Terminal
# Jalankan PERTAMA di terminal manapun:
#   bash chess_server.sh
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE="$SCRIPT_DIR/chess_engine.awk"

# Semua file di /tmp dengan nama tetap (bukan PID) agar client bisa temukan
GAME_ID="chessgame1"
STATE_FILE="/tmp/${GAME_ID}_state"
LOG_FILE="/tmp/${GAME_ID}_log"
PIPE_W="/tmp/${GAME_ID}_move_w"    # client WHITE kirim move ke sini
PIPE_B="/tmp/${GAME_ID}_move_b"    # client BLACK kirim move ke sini
NOTIFY_W="/tmp/${GAME_ID}_notify_w" # server kirim notifikasi ke WHITE
NOTIFY_B="/tmp/${GAME_ID}_notify_b" # server kirim notifikasi ke BLACK
LOCK_FILE="/tmp/${GAME_ID}_lock"    # mutex lock untuk state file

RED=$'\033[0;31m';  GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; WHITE=$'\033[1;37m'; BOLD=$'\033[1m'
DIM=$'\033[2m';     RESET=$'\033[0m'

CURRENT_PLAYER="w"
MOVE_COUNT=0
GAME_ACTIVE=0
WHITE_CONNECTED=0
BLACK_CONNECTED=0
THREAD_PID=0

# ── Signal Handling ──────────────────────────────────────────
cleanup() {
    echo ""
    log_msg "INFO" "Server shutdown..."
    GAME_ACTIVE=0
    [ $THREAD_PID -gt 0 ] && kill $THREAD_PID 2>/dev/null

    # Beritahu semua client bahwa server mati
    echo "SERVER_SHUTDOWN" > "$NOTIFY_W" 2>/dev/null &
    echo "SERVER_SHUTDOWN" > "$NOTIFY_B" 2>/dev/null &
    sleep 0.5

    rm -f "$PIPE_W" "$PIPE_B" "$NOTIFY_W" "$NOTIFY_B" \
          "$STATE_FILE" "$LOCK_FILE"
    printf "${YELLOW}Server dihentikan.${RESET}\n"
    exit 0
}
handle_sigusr2() {
    printf "\n${CYAN}=== SERVER STATUS ===\n"
    printf "Move: %d | Player: %s\n" "$MOVE_COUNT" "$CURRENT_PLAYER"
    printf "WHITE: %s | BLACK: %s${RESET}\n" \
           "$([ $WHITE_CONNECTED -eq 1 ] && echo CONNECTED || echo WAITING)" \
           "$([ $BLACK_CONNECTED -eq 1 ] && echo CONNECTED || echo WAITING)"
}
trap 'cleanup'        SIGTERM EXIT
trap 'handle_sigusr2' SIGUSR2
trap 'cleanup'        SIGINT

log_msg() { printf "[%s][%s] %s\n" "$(date '+%H:%M:%S')" "$1" "$2" >> "$LOG_FILE"; }

# ── Init state file ───────────────────────────────────────────
init_state() {
    cat > "$STATE_FILE" << 'STEOF'
MOVE_COUNT:0
WHITE_CAPTURED:
BLACK_CAPTURED:
CURRENT_PLAYER:w
STATUS:WAITING
BOARD:r,b,0,0|n,b,0,1|b,b,0,2|q,b,0,3|k,b,0,4|b,b,0,5|n,b,0,6|r,b,0,7|p,b,1,0|p,b,1,1|p,b,1,2|p,b,1,3|p,b,1,4|p,b,1,5|p,b,1,6|p,b,1,7|P,w,6,0|P,w,6,1|P,w,6,2|P,w,6,3|P,w,6,4|P,w,6,5|P,w,6,6|P,w,6,7|R,w,7,0|N,w,7,1|B,w,7,2|Q,w,7,3|K,w,7,4|B,w,7,5|N,w,7,6|R,w,7,7
STEOF
}

shm_write() {
    # Lock sederhana menggunakan file
    local key="$1" val="$2"
    while ! mkdir "$LOCK_FILE" 2>/dev/null; do sleep 0.05; done
    if grep -q "^${key}:" "$STATE_FILE" 2>/dev/null; then
        sed -i "s|^${key}:.*|${key}:${val}|" "$STATE_FILE"
    else
        echo "${key}:${val}" >> "$STATE_FILE"
    fi
    rmdir "$LOCK_FILE" 2>/dev/null
}

shm_read() {
    grep "^${1}:" "$STATE_FILE" 2>/dev/null | cut -d: -f2-
}

# ── Thread monitor status koneksi ─────────────────────────────
start_thread_monitor() {
    (
        log_msg "THREAD" "Monitor PID:$$"
        while true; do
            sleep 5
            [ -f "$STATE_FILE" ] || break
            local st; st=$(shm_read "STATUS")
            [ "$st" = "GAMEOVER" ] || [ "$st" = "SHUTDOWN" ] && break
            log_msg "THREAD" "Heartbeat | Move:$(shm_read MOVE_COUNT) | Player:$(shm_read CURRENT_PLAYER)"
        done
        log_msg "THREAD" "Monitor selesai"
    ) &
    THREAD_PID=$!
}

# ── Setup pipes ───────────────────────────────────────────────
setup_pipes() {
    rm -f "$PIPE_W" "$PIPE_B" "$NOTIFY_W" "$NOTIFY_B"
    mkfifo "$PIPE_W" "$PIPE_B" "$NOTIFY_W" "$NOTIFY_B"
    chmod 666 "$PIPE_W" "$PIPE_B" "$NOTIFY_W" "$NOTIFY_B"
    log_msg "INFO" "Pipes dibuat"
}

# ── Kirim notifikasi ke semua client ──────────────────────────
notify_clients() {
    local msg="$1"
    # Non-blocking write ke notify pipe
    ( echo "$msg" > "$NOTIFY_W" ) &
    ( echo "$msg" > "$NOTIFY_B" ) &
}

notify_one() {
    local pipe="$1" msg="$2"
    ( echo "$msg" > "$pipe" ) &
}

# ── Proses satu gerakan ───────────────────────────────────────
process_move() {
    local player="$1" notation="$2"
    
    # Parse notasi
    local raw="${notation//-/}"; raw="${raw// /}"
    if ! [[ "$raw" =~ ^[a-h][1-8][a-h][1-8]$ ]]; then
        log_msg "ERROR" "Format notasi salah: $notation"
        return 1
    fi
    
    local fc fr tc tr
    fc=$(( $(printf '%d' "'${raw:0:1}") - 97 ))
    fr=$(( 8 - ${raw:1:1} ))
    tc=$(( $(printf '%d' "'${raw:2:1}") - 97 ))
    tr=$(( 8 - ${raw:3:1} ))
    
    # Panggil AWK engine
    local resp
    resp=$(awk -f "$ENGINE" -v cmd=MOVE -v player="$player" \
               -v fr="$fr" -v fc="$fc" -v tr="$tr" -v tc="$tc" \
               -v state_file="$STATE_FILE" /dev/null)
    
    echo "$resp"
}

# ── Loop terima move dari client ──────────────────────────────
game_loop() {
    log_msg "INFO" "Game loop dimulai"
    
    # Buka pipe input dengan subshell baca bergantian
    while [ $GAME_ACTIVE -eq 1 ]; do
        local cur; cur=$(shm_read "CURRENT_PLAYER")
        
        if [ "$cur" = "w" ]; then
            # Tunggu move dari WHITE
            local move_w
            if read -r -t 120 move_w < "$PIPE_W" 2>/dev/null; then
                log_msg "MOVE" "WHITE: $move_w"
                
                if [ "$move_w" = "RESIGN" ]; then
                    shm_write "STATUS" "GAMEOVER"
                    notify_clients "GAMEOVER:WINNER:b:RESIGN"
                    GAME_ACTIVE=0; break
                fi
                
                local resp; resp=$(process_move "w" "$move_w")
                local status; status=$(echo "$resp" | head -1)
                
                if [ "$status" = "OK" ]; then
                    # Update state dari respons engine
                    local next chk mate mc wcap bcap
                    next=$(echo  "$resp" | grep "^NEXT_PLAYER:" | cut -d: -f2)
                    chk=$(echo   "$resp" | grep "^CHECK:"       | cut -d: -f2)
                    mate=$(echo  "$resp" | grep "^CHECKMATE:"   | cut -d: -f2)
                    mc=$(echo    "$resp" | grep "^MOVE_COUNT:"  | cut -d: -f2)
                    wcap=$(echo  "$resp" | grep "^WHITE_CAPTURED:" | sed 's/^WHITE_CAPTURED://')
                    bcap=$(echo  "$resp" | grep "^BLACK_CAPTURED:" | sed 's/^BLACK_CAPTURED://')
                    
                    MOVE_COUNT="$mc"
                    CURRENT_PLAYER="$next"
                    shm_write "CURRENT_PLAYER" "$next"
                    shm_write "MOVE_COUNT"     "$mc"
                    [ -n "$wcap" ] && shm_write "WHITE_CAPTURED" "$wcap"
                    [ -n "$bcap" ] && shm_write "BLACK_CAPTURED" "$bcap"
                    
                    if [ "$mate" = "1" ]; then
                        shm_write "STATUS" "GAMEOVER"
                        notify_clients "GAMEOVER:WINNER:w:CHECKMATE"
                        GAME_ACTIVE=0; break
                    elif [ "$chk" = "1" ]; then
                        notify_clients "UPDATE:MOVE:$move_w:CHECK"
                    else
                        notify_clients "UPDATE:MOVE:$move_w"
                    fi
                    log_msg "OK" "Move $move_w berhasil. Next: $next"
                else
                    # Kirim error hanya ke WHITE
                    local err; err=$(echo "$resp" | grep "^ERROR:" | cut -d: -f2-)
                    notify_one "$NOTIFY_W" "ERROR:$err"
                    log_msg "ERROR" "Move $move_w gagal: $err"
                fi
            else
                log_msg "WARN" "Timeout menunggu WHITE"
            fi
            
        else
            # Tunggu move dari BLACK
            local move_b
            if read -r -t 120 move_b < "$PIPE_B" 2>/dev/null; then
                log_msg "MOVE" "BLACK: $move_b"
                
                if [ "$move_b" = "RESIGN" ]; then
                    shm_write "STATUS" "GAMEOVER"
                    notify_clients "GAMEOVER:WINNER:w:RESIGN"
                    GAME_ACTIVE=0; break
                fi
                
                local resp; resp=$(process_move "b" "$move_b")
                local status; status=$(echo "$resp" | head -1)
                
                if [ "$status" = "OK" ]; then
                    local next chk mate mc wcap bcap
                    next=$(echo  "$resp" | grep "^NEXT_PLAYER:" | cut -d: -f2)
                    chk=$(echo   "$resp" | grep "^CHECK:"       | cut -d: -f2)
                    mate=$(echo  "$resp" | grep "^CHECKMATE:"   | cut -d: -f2)
                    mc=$(echo    "$resp" | grep "^MOVE_COUNT:"  | cut -d: -f2)
                    wcap=$(echo  "$resp" | grep "^WHITE_CAPTURED:" | sed 's/^WHITE_CAPTURED://')
                    bcap=$(echo  "$resp" | grep "^BLACK_CAPTURED:" | sed 's/^BLACK_CAPTURED://')
                    
                    MOVE_COUNT="$mc"
                    CURRENT_PLAYER="$next"
                    shm_write "CURRENT_PLAYER" "$next"
                    shm_write "MOVE_COUNT"     "$mc"
                    [ -n "$wcap" ] && shm_write "WHITE_CAPTURED" "$wcap"
                    [ -n "$bcap" ] && shm_write "BLACK_CAPTURED" "$bcap"
                    
                    if [ "$mate" = "1" ]; then
                        shm_write "STATUS" "GAMEOVER"
                        notify_clients "GAMEOVER:WINNER:b:CHECKMATE"
                        GAME_ACTIVE=0; break
                    elif [ "$chk" = "1" ]; then
                        notify_clients "UPDATE:MOVE:$move_b:CHECK"
                    else
                        notify_clients "UPDATE:MOVE:$move_b"
                    fi
                    log_msg "OK" "Move $move_b berhasil. Next: $next"
                else
                    local err; err=$(echo "$resp" | grep "^ERROR:" | cut -d: -f2-)
                    notify_one "$NOTIFY_B" "ERROR:$err"
                    log_msg "ERROR" "Move $move_b gagal: $err"
                fi
            else
                log_msg "WARN" "Timeout menunggu BLACK"
            fi
        fi
    done
    
    log_msg "INFO" "Game loop selesai"
}

# ── Main server ───────────────────────────────────────────────
main() {
    clear
    touch "$LOG_FILE"
    log_msg "INFO" "Server start PID:$$"
    
    printf "${BOLD}${CYAN}"
    printf "  ╔════════════════════════════════════════════╗\n"
    printf "  ║      ♟  GAME CATUR LINUX — SERVER  ♟      ║\n"
    printf "  ╚════════════════════════════════════════════╝${RESET}\n\n"
    printf "  ${BOLD}PID Server: $$${RESET}\n"
    printf "  ${DIM}Game ID: %s${RESET}\n\n" "$GAME_ID"
    
    printf "  ${BOLD}Cara memulai di terminal lain:${RESET}\n"
    printf "  ${GREEN}bash chess_client.sh w${RESET}   ← Terminal Pemain WHITE\n"
    printf "  ${GREEN}bash chess_client.sh b${RESET}   ← Terminal Pemain BLACK\n\n"
    printf "  ${DIM}Menunggu kedua pemain terhubung...${RESET}\n\n"
    
    setup_pipes
    init_state
    start_thread_monitor
    
    # Tunggu WHITE connect
    printf "  ${DIM}Menunggu WHITE...${RESET} "
    local conn_msg
    read -r conn_msg < "$PIPE_W"
    if [ "$conn_msg" = "CONNECT:w" ]; then
        WHITE_CONNECTED=1
        printf "${GREEN}Terhubung!${RESET}\n"
        shm_write "WHITE_STATUS" "CONNECTED"
        notify_one "$NOTIFY_W" "CONNECTED:w:WAIT_BLACK"
        log_msg "INFO" "WHITE terhubung"
    fi
    
    # Tunggu BLACK connect
    printf "  ${DIM}Menunggu BLACK...${RESET} "
    read -r conn_msg < "$PIPE_B"
    if [ "$conn_msg" = "CONNECT:b" ]; then
        BLACK_CONNECTED=1
        printf "${GREEN}Terhubung!${RESET}\n"
        shm_write "BLACK_STATUS" "CONNECTED"
        log_msg "INFO" "BLACK terhubung"
    fi
    
    printf "\n  ${GREEN}${BOLD}Kedua pemain siap! Game dimulai!${RESET}\n\n"
    shm_write "STATUS" "ACTIVE"
    
    # Beritahu kedua client untuk mulai
    notify_clients "GAME_START"
    sleep 0.2
    
    GAME_ACTIVE=1
    game_loop
    
    printf "\n  ${YELLOW}Game selesai. Total move: %d${RESET}\n" "$MOVE_COUNT"
    printf "  ${DIM}Log tersimpan di: %s${RESET}\n" "$LOG_FILE"
    printf "\n  Tekan Enter untuk keluar... "; read -r
}

main
