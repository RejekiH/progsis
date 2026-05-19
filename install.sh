#!/bin/bash
# ============================================================
# install.sh - Script Instalasi Game Catur di Rocky Linux
# ============================================================

set -e  # Hentikan jika ada error

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

INSTALL_DIR="$HOME/chess_game"

print_header() {
    echo -e "${CYAN}${BOLD}"
    echo "╔══════════════════════════════════════════╗"
    echo "║    Instalasi Game Catur Linux            ║"
    echo "║    Rocky Linux / VMware Setup            ║"
    echo "╚══════════════════════════════════════════╝"
    echo -e "${RESET}"
}

check_command() {
    local cmd="$1"
    if command -v "$cmd" &>/dev/null; then
        echo -e "  ${GREEN}✓${RESET} $cmd tersedia"
        return 0
    else
        echo -e "  ${RED}✗${RESET} $cmd tidak ditemukan"
        return 1
    fi
}

check_dependencies() {
    echo -e "\n${BOLD}=== Memeriksa Dependensi ===${RESET}"
    
    local missing=0
    check_command bash    || missing=$((missing+1))
    check_command awk     || missing=$((missing+1))
    check_command mkfifo  || missing=$((missing+1))
    check_command gcc     || { echo -e "  ${YELLOW}⚠${RESET}  gcc opsional (untuk fitur thread C)"; }
    check_command make    || { echo -e "  ${YELLOW}⚠${RESET}  make opsional"; }
    
    if [ $missing -gt 0 ]; then
        echo -e "\n${RED}Ada $missing dependensi yang hilang!${RESET}"
        return 1
    fi
    echo -e "\n${GREEN}Semua dependensi utama tersedia!${RESET}"
    return 0
}

install_gcc() {
    echo -e "\n${BOLD}=== Install GCC (untuk chess_thread.c) ===${RESET}"
    
    if command -v gcc &>/dev/null; then
        echo -e "${GREEN}GCC sudah terinstall: $(gcc --version | head -1)${RESET}"
        return 0
    fi
    
    echo "Mencoba install gcc..."
    
    if command -v dnf &>/dev/null; then
        # Rocky Linux / RHEL 8+
        echo "Menggunakan dnf (Rocky Linux/RHEL)..."
        sudo dnf groupinstall -y "Development Tools" 2>/dev/null || \
        sudo dnf install -y gcc glibc-devel 2>/dev/null
        
    elif command -v yum &>/dev/null; then
        # RHEL 7 / CentOS 7
        echo "Menggunakan yum..."
        sudo yum groupinstall -y "Development Tools" 2>/dev/null || \
        sudo yum install -y gcc glibc-devel 2>/dev/null
        
    elif command -v apt-get &>/dev/null; then
        # Debian/Ubuntu
        echo "Menggunakan apt-get..."
        sudo apt-get update -y
        sudo apt-get install -y gcc make libc6-dev
        
    else
        echo -e "${YELLOW}Package manager tidak dikenali. Install gcc secara manual.${RESET}"
        return 1
    fi
    
    if command -v gcc &>/dev/null; then
        echo -e "${GREEN}GCC berhasil diinstall!${RESET}"
    else
        echo -e "${RED}Gagal install GCC. Game shell tetap bisa dijalankan.${RESET}"
        return 1
    fi
}

compile_thread() {
    echo -e "\n${BOLD}=== Kompilasi chess_thread.c ===${RESET}"
    
    if [ ! -f "chess_thread.c" ]; then
        echo -e "${RED}chess_thread.c tidak ditemukan!${RESET}"
        return 1
    fi
    
    if command -v gcc &>/dev/null; then
        echo "Mengkompilasi..."
        gcc -Wall -Wextra -std=c99 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            -o chess_thread chess_thread.c -lpthread -lrt 2>&1
        
        if [ -f "chess_thread" ]; then
            echo -e "${GREEN}✓ Kompilasi berhasil: chess_thread${RESET}"
        else
            echo -e "${RED}✗ Kompilasi gagal${RESET}"
            return 1
        fi
    else
        echo -e "${YELLOW}GCC tidak tersedia. Lewati kompilasi C.${RESET}"
        echo -e "${YELLOW}Fitur POSIX thread akan disimulasikan via shell.${RESET}"
        return 0
    fi
}

set_permissions() {
    echo -e "\n${BOLD}=== Setting Permissions ===${RESET}"
    
    chmod +x chess.sh       && echo -e "  ${GREEN}✓${RESET} chess.sh"
    chmod +x chess_engine.awk && echo -e "  ${GREEN}✓${RESET} chess_engine.awk"
    [ -f chess_thread ] && chmod +x chess_thread && echo -e "  ${GREEN}✓${RESET} chess_thread"
    [ -f install.sh ] && chmod +x install.sh && echo -e "  ${GREEN}✓${RESET} install.sh"
}

show_summary() {
    echo -e "\n${CYAN}${BOLD}"
    echo "╔══════════════════════════════════════════╗"
    echo "║           INSTALASI SELESAI!             ║"
    echo "╚══════════════════════════════════════════╝"
    echo -e "${RESET}"
    
    echo -e "${BOLD}Cara Menjalankan:${RESET}"
    echo -e "  ${GREEN}./chess.sh${RESET}              → Jalankan game catur"
    echo -e "  ${GREEN}./chess_thread${RESET}           → Demo POSIX thread"
    echo -e "  ${GREEN}./chess_thread --interactive${RESET} → Thread + input manual"
    echo ""
    
    echo -e "${BOLD}Komponen yang digunakan:${RESET}"
    echo "  ✓ Shell (bash)    - Logika utama & UI"
    echo "  ✓ AWK             - Engine validasi move"
    echo "  ✓ Named Pipes     - Komunikasi antar proses"
    echo "  ✓ Shared Memory   - State papan catur"
    echo "  ✓ Signal Handling - SIGINT, SIGTERM, SIGUSR1, SIGUSR2"
    [ -f chess_thread ] && echo "  ✓ POSIX Thread    - Monitor, Timer, Logger, AI"
    echo ""
    
    echo -e "${BOLD}Signal yang tersedia saat game berjalan:${RESET}"
    echo "  kill -USR1 <PID>  → Pause/Resume game"
    echo "  kill -USR2 <PID>  → Tampilkan statistik"
    echo "  Ctrl+C            → Konfirmasi keluar"
    echo ""
    
    echo -e "${DIM}File game tersimpan di: $(pwd)${RESET}"
}

# ── Main ──────────────────────────────────────────────────────
main() {
    print_header
    
    # Pindah ke direktori script
    cd "$(dirname "${BASH_SOURCE[0]}")"
    
    check_dependencies
    
    echo -e "\n${BOLD}Apakah ingin install GCC? (y/n) [n]:${RESET} "
    read -r -t 10 install_gcc_choice || install_gcc_choice="n"
    
    if [[ "$install_gcc_choice" =~ ^[Yy]$ ]]; then
        install_gcc
    fi
    
    compile_thread
    set_permissions
    show_summary
    
    echo -e "\n${BOLD}Apakah ingin langsung menjalankan game? (y/n) [y]:${RESET} "
    read -r -t 10 run_choice || run_choice="y"
    
    if [[ "$run_choice" =~ ^[Nn]$ ]]; then
        echo "Jalankan dengan: ./chess.sh"
    else
        ./chess.sh
    fi
}

main "$@"
