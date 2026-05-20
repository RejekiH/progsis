#!/bin/bash
# PATCH chess_client.sh — Fix #10: IN_CHECK reset + Fix #7 stalemate
# Ini adalah potongan blok UPDATE di giliran sendiri (sekitar baris 339-346).
# Ganti case "$resp" in UPDATE:*) ... di blok "Giliran saya" dengan ini:

# ── Di blok "Giliran saya" — setelah kirim move ──────────────
# Ganti blok case UPDATE:*) yang lama dengan ini:

                UPDATE:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    # FIX #10: Selalu reset IN_CHECK dulu, baru cek apakah ada CHECK baru
                    IN_CHECK=0
                    if [ "${parts[3]}" = "CHECK" ]; then
                        IN_CHECK=1
                        LAST_MSG="${YELLOW}${BOLD}⚠ SKAK! Raja lawan dalam bahaya!${RESET}"
                    else
                        LAST_MSG="${GREEN}✓ Gerakan ${parts[2]} berhasil${RESET}"
                    fi ;;

                # FIX #7: Tangani STALEMATE dari server
                STALEMATE:*)
                    draw_board
                    printf "\n  ${YELLOW}${BOLD}╔════════════════════════════╗\n"
                    printf "  ║   STALEMATE! HASIL SERI.  ║\n"
                    printf "  ╚════════════════════════════╝${RESET}\n"
                    printf "  Tekan Enter..."; read -r
                    GAME_ACTIVE=0; break ;;

# ── Di blok "Bukan giliran saya" — setelah terima UPDATE ─────
# Ganti blok case UPDATE:*) di blok waiting dengan ini:

                UPDATE:*)
                    local parts; IFS=':' read -ra parts <<< "$resp"
                    # FIX #10: Reset IN_CHECK sebelum evaluasi ulang
                    IN_CHECK=0
                    if [ "${parts[3]}" = "CHECK" ]; then
                        IN_CHECK=1
                        LAST_MSG="${YELLOW}${BOLD}⚠ SKAK! Raja Anda dalam bahaya!${RESET}"
                    else
                        LAST_MSG="${CYAN}Lawan bermain: ${parts[2]}${RESET}"
                    fi ;;

                # FIX #7: Tangani STALEMATE saat menunggu lawan
                STALEMATE:*)
                    draw_board
                    printf "\n  ${YELLOW}${BOLD}╔════════════════════════════╗\n"
                    printf "  ║   STALEMATE! HASIL SERI.  ║\n"
                    printf "  ╚════════════════════════════╝${RESET}\n"
                    printf "  Tekan Enter..."; read -r
                    GAME_ACTIVE=0; break ;;
