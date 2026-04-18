// potato_main_patch.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Ce fichier montre QUOI modifier dans le main.cpp de moonlight-N3DS
// pour activer le mode Potato.
//
// Dans le vrai fork : copie le main.cpp original et applique ces changements.
// ─────────────────────────────────────────────────────────────────────────────

#include "potato/potato_profile.h"

// ════════════════════════════════════════════════════════════════════════════
// ÉTAPE 1 — Au tout début de main(), avant TOUT le reste
// ════════════════════════════════════════════════════════════════════════════

/*
int main(int argc, char* argv[]) {
    // Init services 3DS obligatoires
    cfguInit();   // REQUIS avant potato_init
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    // ─── POTATO INIT ───────────────────────────────────────────────
    bool is_potato = potato_init();

    if (is_potato) {
        printf("🥔 Potato Stream v1.0 — Mode Old 3DS/2DS\n");
        printf("   Résolution : %dx%d @ %dfps\n",
               g_potato.width, g_potato.height, g_potato.fps);
        printf("   Bitrate    : %d Kbps\n", g_potato.bitrate_kbps);
        printf("   HW Decode  : DÉSACTIVÉ (normal sur Old 3DS)\n\n");
    }
    // ────────────────────────────────────────────────────────────────

    // Suite du main normal...
*/


// ════════════════════════════════════════════════════════════════════════════
// ÉTAPE 2 — Là où la CONFIGURATION Moonlight est construite
//           (cherche "CONFIGURATION config" dans le main original)
// ════════════════════════════════════════════════════════════════════════════

/*
    CONFIGURATION config;
    LiInitializeConfiguration(&config);

    // ─── POTATO : override config si Old 3DS ───────────────────────
    if (g_potato.is_potato) {
        config.width    = g_potato.width;          // 400
        config.height   = g_potato.height;         // 240
        config.fps      = g_potato.fps;            // 24
        config.bitrate  = g_potato.bitrate_kbps;   // 3000
        config.hwdecode = 0;                       // pas de MVD

        // Désactive le bitrate auto de moonlight (il essaierait de monter)
        // et force nos valeurs
    }
    // ────────────────────────────────────────────────────────────────
*/


// ════════════════════════════════════════════════════════════════════════════
// ÉTAPE 3 — Là où les VIDEO_RENDERER_CALLBACKS sont définis
//           (cherche "VideoCallbacks" ou "DR_CALLBACKS" dans le main)
// ════════════════════════════════════════════════════════════════════════════

/*
    VIDEO_RENDERER_CALLBACKS vidCb;

    if (g_potato.is_potato) {
        // ─── Utilise notre pipeline soft optimisé ──────────────────
        vidCb.setup   = [](int w, int h, int fps, int bpp) -> int {
            return potato_video_init(w, h, fps, bpp);
        };
        vidCb.submit  = [](PDECODE_UNIT du) -> int {
            // Moonlight nous donne les NAL units concaténés
            // On extrait data + length et on passe à notre décodeur
            unsigned char* data = NULL;
            int length = 0;

            // Parcourt la liste chainée de buffer descriptors
            PLENTRY entry = du->bufferList;
            // Version simple : on copie tout dans un buffer plat
            // (le moonlight-N3DS original fait pareil)
            static u8 nal_buf[256 * 1024]; // 256 KB max par frame
            int offset = 0;
            while (entry != NULL && offset < (int)sizeof(nal_buf)) {
                int to_copy = entry->length;
                if (offset + to_copy > (int)sizeof(nal_buf))
                    to_copy = sizeof(nal_buf) - offset;
                memcpy(nal_buf + offset, entry->data, to_copy);
                offset += to_copy;
                entry = entry->next;
            }
            return potato_video_submit(nullptr, offset, nal_buf);
        };
        vidCb.cleanup = potato_video_cleanup;
        // ───────────────────────────────────────────────────────────
    } else {
        // New 3DS : garde le code MVD original de moonlight-N3DS
        // (copier ici les callbacks originaux)
    }
*/


// ════════════════════════════════════════════════════════════════════════════
// ÉTAPE 4 — Dans la boucle principale (là où aptMainLoop tourne)
//           Log des stats toutes les 10 secondes
// ════════════════════════════════════════════════════════════════════════════

/*
    uint64_t last_stats_tick = svcGetSystemTick();
    const uint64_t STATS_INTERVAL = 268000000ULL * 10; // 10 secondes

    while (aptMainLoop()) {
        // ... code existant ...

        // ─── POTATO : stats périodiques ────────────────────────────
        if (g_potato.is_potato) {
            uint64_t now = svcGetSystemTick();
            if (now - last_stats_tick > STATS_INTERVAL) {
                potato_print_stats();
                last_stats_tick = now;
            }
        }
        // ────────────────────────────────────────────────────────────
    }
*/


// ════════════════════════════════════════════════════════════════════════════
// ÉTAPE 5 — À la fin, dans le cleanup
// ════════════════════════════════════════════════════════════════════════════

/*
    // Cleanup normal moonlight...
    LiStopConnection();

    // ─── POTATO : cleanup ──────────────────────────────────────────
    if (g_potato.is_potato) {
        potato_video_cleanup();
    }
    // ────────────────────────────────────────────────────────────────

    cfguExit();
    gfxExit();
    return 0;
}
*/
