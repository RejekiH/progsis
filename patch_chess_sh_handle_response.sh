#!/bin/bash
# PATCH chess.sh — handle_response() dengan stalemate (Fix #7)
# Ganti fungsi handle_response() yang lama dengan versi ini.
# Perubahan: tambah deteksi STALEMATE:1 dan pesan yang sesuai.

handle_response() {
    local resp="$1"
    local status="" cap="" chk=0 mate=0 stale=0 next="" wcap="" bcap="" mc=""

    while IFS= read -r line; do
        case "$line" in
            OK)                 status="OK" ;;
            ERROR:*)            status="${line#ERROR:}" ;;
            CAPTURED:*)         cap="${line#CAPTURED:}" ;;
            CHECK:1)            chk=1 ;;
            CHECKMATE:1)        mate=1 ;;
            STALEMATE:1)        stale=1 ;;          # FIX #7
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
        WHITE_CAPTURED="$wcap"
        BLACK_CAPTURED="$bcap"

        if [ -n "$cap" ] && [ "$cap" != "." ]; then
            local csym; csym=$(piece_sym "$cap")
            LAST_MSG="${GREEN}✓ Berhasil — menangkap ${csym}!${RESET}"
        else
            LAST_MSG="${GREEN}✓ Gerakan berhasil${RESET}"
        fi

        # FIX #7: Deteksi stalemate sebelum checkmate
        if [ $stale -eq 1 ]; then
            draw_board
            printf "\n${YELLOW}${BOLD}  ╔═══════════════════════════════════╗\n"
            printf "  ║  STALEMATE!  HASIL SERI (DRAW)  ║\n"
            printf "  ╚═══════════════════════════════════╝${RESET}\n\n"
            printf "  Tekan Enter..."; read -r
            GAME_ACTIVE=0; return
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
            CASTLE_RIGHTS_LOST)    LAST_MSG="${RED}✗ Castling tidak bisa: raja atau benteng sudah pernah bergerak!${RESET}" ;;
            CASTLE_IN_CHECK)       LAST_MSG="${RED}✗ Castling tidak bisa: raja sedang dalam skak!${RESET}" ;;
            CASTLE_BLOCKED)        LAST_MSG="${RED}✗ Castling tidak bisa: ada bidak di antara raja dan benteng!${RESET}" ;;
            CASTLE_THROUGH_CHECK)  LAST_MSG="${RED}✗ Castling tidak bisa: raja melewati petak yang diserang!${RESET}" ;;
            LEAVES_KING_IN_CHECK)  LAST_MSG="${RED}✗ Tidak bisa! Raja Anda tetap dalam bahaya!${RESET}" ;;
            OUT_OF_BOUNDS)         LAST_MSG="${RED}✗ Posisi di luar papan!${RESET}" ;;
            *)                     LAST_MSG="${RED}✗ Tidak valid: ${status}${RESET}" ;;
        esac
        log_msg "INVALID" "$status"
    fi
}
