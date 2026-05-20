#!/bin/bash
# PATCH chess_server.sh — Fix #7 stalemate + Fix #9 castling error

# Di game_loop(), setelah "if [ "$status" = "OK" ]; then"
# Tambahkan deteksi stalemate di kedua blok (WHITE dan BLACK).
# Ganti blok "if [ "$mate" = "1" ]; then" dengan ini:

                    # Baca juga stalemate dari respons engine
                    stale=$(echo  "$resp" | grep "^STALEMATE:" | cut -d: -f2)

                    if [ "$mate" = "1" ]; then
                        shm_write "STATUS" "GAMEOVER"
                        notify_clients "GAMEOVER:WINNER:w:CHECKMATE"    # ganti w/b sesuai konteks
                        GAME_ACTIVE=0; break
                    # FIX #7: Stalemate — seri, tidak ada pemenang
                    elif [ "$stale" = "1" ]; then
                        shm_write "STATUS" "GAMEOVER"
                        notify_clients "STALEMATE:DRAW"
                        GAME_ACTIVE=0; break
                    elif [ "$chk" = "1" ]; then
                        notify_clients "UPDATE:MOVE:$move_w:CHECK"
                    else
                        notify_clients "UPDATE:MOVE:$move_w"
                    fi

# ── Tambahkan juga di error handler agar castling punya pesan ─
# Di blok "else" (move gagal), tambahkan case castling:
# case "$err" in
#     CASTLE_RIGHTS_LOST|CASTLE_IN_CHECK|CASTLE_BLOCKED|CASTLE_THROUGH_CHECK)
#         ;;   # error castling sudah informatif, kirim apa adanya
# esac
