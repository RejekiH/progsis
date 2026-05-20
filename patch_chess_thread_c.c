/*
 * PATCH chess_thread.c — Fix #13: pthread_join sebelum cleanup_shared_memory()
 *
 * Masalah: cleanup_shared_memory() memanggil pthread_mutex_destroy(&shm_ptr->board_mutex)
 * padahal thread lain bisa masih memakai mutex tersebut → undefined behavior.
 *
 * Solusi: Pastikan semua thread sudah di-join SEBELUM cleanup dipanggil.
 * Ganti blok setelah loop "while (running)" di main() dengan ini:
 */

    /* Blok ini menggantikan kode lama di main() setelah loop running */

    printf("\n[Main] Menunggu semua thread selesai...\n");

    /* Sinyal semua thread untuk berhenti */
    running = 0;
    if (shm_ptr) shm_ptr->game_active = 0;

    /* FIX #13: Join SEMUA thread terlebih dahulu sebelum cleanup apapun.
     * Setelah semua thread selesai, dijamin tidak ada yang memegang mutex,
     * sehingga pthread_mutex_destroy() di cleanup_shared_memory() aman. */
    for (int i = 0; i < MAX_THREADS; i++) {
        int ret = pthread_join(threads[i], NULL);
        if (ret != 0) {
            fprintf(stderr, "[Main] pthread_join thread %d gagal: %s\n",
                    i, strerror(ret));
        } else {
            printf("[Main] Thread %d (%s) berhasil di-join\n",
                   i, thread_args[i].role);
        }
    }

    /* FIX #13: Baru cleanup setelah SEMUA thread selesai */
    cleanup_shared_memory();

    printf("\n[Main] Semua thread selesai. Program berakhir.\n");
    return 0;

/*
 * Catatan tambahan untuk cleanup_shared_memory():
 * Tidak perlu diubah — sudah benar memanggil pthread_mutex_destroy lalu munmap.
 * Yang penting urutan panggilannya: join dulu, cleanup kemudian.
 *
 * Kode lama yang SALAH (di main):
 *   cleanup_shared_memory();   ← dipanggil dulu
 *   for (i...) pthread_join()  ← join belakangan — UB!
 *
 * Kode baru yang BENAR:
 *   for (i...) pthread_join()  ← join semua thread
 *   cleanup_shared_memory();   ← baru cleanup
 */
