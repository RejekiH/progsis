#!/usr/bin/awk -f
# chess_engine.awk — dipanggil sekali per move
# -v cmd=MOVE/BOARD/STATUS  -v player=w/b
# -v fr fc tr tc (koordinat 0-7)  -v state_file=PATH

BEGIN {
    for (r=0;r<8;r++) for (c=0;c<8;c++) board[r,c]="."
    move_count=0; white_cap=""; black_cap=""

    # Baca state dari file
    while ((getline line < state_file) > 0) {
        if (line ~ /^MOVE_COUNT:/)    { move_count = substr(line,12)+0 }
        if (line ~ /^WHITE_CAPTURED:/){ white_cap  = substr(line,16) }
        if (line ~ /^BLACK_CAPTURED:/){ black_cap  = substr(line,16) }
        if (line ~ /^BOARD:/) {
            n = split(substr(line,7), pcs, "|")
            for (i=1;i<=n;i++) {
                split(pcs[i], f, ",")
                board[f[3]+0, f[4]+0] = f[1]
            }
        }
    }
    close(state_file)

    if      (cmd == "MOVE")   { print do_move(player, fr+0, fc+0, tr+0, tc+0) }
    else if (cmd == "BOARD")  { print_board() }
    else if (cmd == "STATUS") { printf "MOVE_COUNT:%d\n", move_count }
}

# ── Utilitas ──────────────────────────────────────────────────
function abs(x)  { return x<0?-x:x }
function sign(x) { return x>0?1:x<0?-1:0 }
function inb(r,c){ return r>=0&&r<8&&c>=0&&c<8 }
function col(p)  { if(p==".") return ""; return (p~/[a-z]/)?"b":"w" }
function opp(c)  { return c=="w"?"b":"w" }

# ── Cek apakah petak (r,c) diserang oleh warna acol ──────────
function attacked(r,c,acol,  i,ar,ac,p,dr,dc,kn,kd) {
    # Kuda
    split("2,1|2,-1|-2,1|-2,-1|1,2|1,-2|-1,2|-1,-2",kn,"|")
    for(i=1;i<=8;i++) {
        split(kn[i],kd,","); ar=r+kd[1]; ac=c+kd[2]
        if(inb(ar,ac) && col(board[ar,ac])==acol && (board[ar,ac]=="N"||board[ar,ac]=="n")) return 1
    }
    # Diagonal — gajah & ratu
    for(dr=-1;dr<=1;dr+=2) for(dc=-1;dc<=1;dc+=2) {
        ar=r+dr; ac=c+dc
        while(inb(ar,ac)) {
            p=board[ar,ac]
            if(p!="."){if(col(p)==acol && (p~/[bBqQ]/))return 1; break}
            ar+=dr; ac+=dc
        }
    }
    # Lurus — benteng & ratu
    split("1,0|-1,0|0,1|0,-1",kn,"|")
    for(i=1;i<=4;i++) {
        split(kn[i],kd,","); ar=r+kd[1]; ac=c+kd[2]
        while(inb(ar,ac)) {
            p=board[ar,ac]
            if(p!="."){if(col(p)==acol && (p~/[rRqQ]/))return 1; break}
            ar+=kd[1]; ac+=kd[2]
        }
    }
    # Pion
    if(acol=="w"){for(dc=-1;dc<=1;dc+=2){ar=r+1;ac=c+dc;if(inb(ar,ac)&&board[ar,ac]=="P")return 1}}
    else         {for(dc=-1;dc<=1;dc+=2){ar=r-1;ac=c+dc;if(inb(ar,ac)&&board[ar,ac]=="p")return 1}}
    # Raja
    for(dr=-1;dr<=1;dr++) for(dc=-1;dc<=1;dc++) {
        if(!dr&&!dc) continue
        ar=r+dr; ac=c+dc
        if(inb(ar,ac)&&col(board[ar,ac])==acol&&(board[ar,ac]=="K"||board[ar,ac]=="k")) return 1
    }
    return 0
}

function find_king(c,  r,rc,k) {
    k=(c=="w")?"K":"k"
    for(r=0;r<8;r++) for(rc=0;rc<8;rc++) if(board[r,rc]==k){kg_r=r;kg_c=rc;return 1}
    return 0
}
function in_check(c) { find_king(c); return attacked(kg_r,kg_c,opp(c)) }

function move_ok(fr,fc,tr,tc,c,  sv,r,rc,res) {
    for(r=0;r<8;r++) for(rc=0;rc<8;rc++) sv[r,rc]=board[r,rc]
    board[tr,tc]=board[fr,fc]; board[fr,fc]="."
    res=in_check(c)
    for(r=0;r<8;r++) for(rc=0;rc<8;rc++) board[r,rc]=sv[r,rc]
    return !res
}

# ── Validasi gerakan per jenis bidak ─────────────────────────
function valid(c,fr,fc,tr,tc,  p,lp,dr,dc,dr2,dc2,ar,ac) {
    if(!inb(fr,fc)||!inb(tr,tc)) return "OUT_OF_BOUNDS"
    p=board[fr,fc]
    if(p==".") return "EMPTY_SOURCE"
    if(col(p)!=c) return "WRONG_COLOR"
    if(col(board[tr,tc])==c) return "DEST_OCCUPIED"
    lp=tolower(p); dr=tr-fr; dc=tc-fc

    if(lp=="p") {
        if(c=="w") {
            if     (dc==0&&dr==-1&&board[tr,tc]=="."){}
            else if(dc==0&&dr==-2&&fr==6&&board[tr,tc]=="."&&board[fr-1,fc]=="."){}
            else if(abs(dc)==1&&dr==-1&&board[tr,tc]!="."&&col(board[tr,tc])=="b"){}
            else return "INVALID_PAWN"
        } else {
            if     (dc==0&&dr==1&&board[tr,tc]=="."){}
            else if(dc==0&&dr==2&&fr==1&&board[tr,tc]=="."&&board[fr+1,fc]=="."){}
            else if(abs(dc)==1&&dr==1&&board[tr,tc]!="."&&col(board[tr,tc])=="w"){}
            else return "INVALID_PAWN"
        }
    } else if(lp=="r") {
        if(fr!=tr&&fc!=tc) return "INVALID_ROOK"
        dr2=sign(tr-fr); dc2=sign(tc-fc); ar=fr+dr2; ac=fc+dc2
        while(ar!=tr||ac!=tc){if(board[ar,ac]!=".")return "ROOK_BLOCKED"; ar+=dr2; ac+=dc2}
    } else if(lp=="b") {
        if(abs(dr)!=abs(dc)) return "INVALID_BISHOP"
        dr2=sign(dr); dc2=sign(dc); ar=fr+dr2; ac=fc+dc2
        while(ar!=tr||ac!=tc){if(board[ar,ac]!=".")return "BISHOP_BLOCKED"; ar+=dr2; ac+=dc2}
    } else if(lp=="n") {
        if(!((abs(dr)==2&&abs(dc)==1)||(abs(dr)==1&&abs(dc)==2))) return "INVALID_KNIGHT"
    } else if(lp=="q") {
        if(fr==tr||fc==tc) {
            dr2=sign(tr-fr); dc2=sign(tc-fc); ar=fr+dr2; ac=fc+dc2
            while(ar!=tr||ac!=tc){if(board[ar,ac]!=".")return "QUEEN_BLOCKED"; ar+=dr2; ac+=dc2}
        } else if(abs(dr)==abs(dc)) {
            dr2=sign(dr); dc2=sign(dc); ar=fr+dr2; ac=fc+dc2
            while(ar!=tr||ac!=tc){if(board[ar,ac]!=".")return "QUEEN_BLOCKED"; ar+=dr2; ac+=dc2}
        } else return "INVALID_QUEEN"
    } else if(lp=="k") {
        if(abs(dr)>1||abs(dc)>1) return "INVALID_KING"
    }

    if(!move_ok(fr,fc,tr,tc,c)) return "LEAVES_KING_IN_CHECK"
    return "OK"
}

function has_legal_move(c,  r,rc,t1,t2) {
    for(r=0;r<8;r++) for(rc=0;rc<8;rc++) {
        if(board[r,rc]=="."||col(board[r,rc])!=c) continue
        for(t1=0;t1<8;t1++) for(t2=0;t2<8;t2++)
            if(valid(c,r,rc,t1,t2)=="OK") return 1
    }
    return 0
}

# ── Eksekusi move ─────────────────────────────────────────────
function do_move(c,fr,fc,tr,tc,  res,cap,p,oc,chk,mate,sep) {
    res=valid(c,fr,fc,tr,tc)
    if(res!="OK") return "ERROR:" res

    cap=board[tr,tc]; p=board[fr,fc]
    board[tr,tc]=p; board[fr,fc]="."

    # Promosi pion
    if(p=="P"&&tr==0) board[tr,tc]="Q"
    if(p=="p"&&tr==7) board[tr,tc]="q"

    # Akumulasi captured — PERBAIKAN: concat dengan separator "|"
    if(cap!=".") {
        if(c=="w") {
            sep = (white_cap==""?"":"|")
            white_cap = white_cap sep cap
        } else {
            sep = (black_cap==""?"":"|")
            black_cap = black_cap sep cap
        }
    }

    move_count++
    oc=opp(c); chk=in_check(oc)
    mate=(chk && !has_legal_move(oc)) ? 1 : 0

    # Tentukan STATUS baru: GAMEOVER jika checkmate, selain itu ACTIVE
    new_status = (mate ? "GAMEOVER" : "ACTIVE")

    save_state(oc, new_status)

    return "OK\nMOVE_COUNT:" move_count \
           "\nCAPTURED:" cap \
           "\nCHECK:" chk \
           "\nCHECKMATE:" mate \
           "\nNEXT_PLAYER:" oc \
           "\nWHITE_CAPTURED:" white_cap \
           "\nBLACK_CAPTURED:" black_cap
}

# ── Simpan state ke file ──────────────────────────────────────
# FIX #1: Tambah next_player sebagai parameter agar CURRENT_PLAYER tersimpan
# FIX #2: Tambah status sebagai parameter agar STATUS tidak hilang
function save_state(next_player, status,   r,c,p,first,bs) {
    # Tulis ulang seluruh file sekaligus (atomic lewat file tmp lalu rename)
    # agar tidak ada pembaca yang membaca file setengah-selesai
    tmp_file = state_file ".tmp"
    print "MOVE_COUNT:"     move_count   > tmp_file
    print "CURRENT_PLAYER:" next_player >> tmp_file
    print "STATUS:"         status      >> tmp_file
    print "WHITE_CAPTURED:" white_cap   >> tmp_file
    print "BLACK_CAPTURED:" black_cap   >> tmp_file
    # Board
    first=1; bs=""
    for(r=0;r<8;r++) for(c=0;c<8;c++) {
        p=board[r,c]; if(p==".") continue
        bs = bs (first?"":"|") p "," col(p) "," r "," c
        first=0
    }
    print "BOARD:" bs >> tmp_file
    close(tmp_file)
    # Rename atomik — pembaca tidak akan mendapat file setengah-tulis
    system("mv " tmp_file " " state_file)
}

# ── Cetak papan untuk draw_board ─────────────────────────────
function print_board(  r,c,p) {
    for(r=0;r<8;r++) for(c=0;c<8;c++) {
        p=board[r,c]; if(p!=".") printf "%s %s %d %d\n",p,col(p),r,c
    }
}
