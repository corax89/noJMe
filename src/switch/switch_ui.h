/*
 * switch_ui.h — Nintendo Switch in-app menu: game browser + settings.
 */
#ifndef SWITCH_UI_H
#define SWITCH_UI_H

#include <stddef.h>

/* Result of the menu session. */
typedef enum {
    SWITCH_UI_EXIT = 0,   /* пользователь выбрал «Выход» */
    SWITCH_UI_GAME = 1    /* выбран jar — путь в out_path */
} SwitchUiResult;

/* Show the main menu (→ browser / settings) until a game is picked or the
 * user exits. Returns 1 and fills out_path on game selection, 0 on exit.
 * Uses the sdl_switch_* glue (switch_glue.h) — no direct SDL calls. */
int switch_ui_pick_game(char* out_path, size_t path_cap);

/* v36.58 [JAR-ICONS]: полный сброс кэша иконок браузера (память игровой
 * сессии освобождается — иконки извлекаются лениво при следующем просмотре
 * списка). Вызывается фронтендом при завершении игровой сессии. */
void switch_ui_jaricons_reset(void);

/* v34.94: in-game per-game settings overlay (PLUS key): resolution /
 * scaling / filter for THIS jar; persists to <base>/pergame/<jar>.ini.
 * Runs its own pump loop on the frame thread (same contract as the pause
 * menu); returns on B. */
void switch_ui_pergame_screen(const char* jar_path);

/* v36.26 [TOUCH-UI]: hit box of the language badge (top-left corner).
 * y_off — 8 in the menu headers (draw_lang_badge), 24 in the pause
 * overlay. A tap on the badge acts as MINUS (language toggle). */
int switch_ui_badge_hit(int x, int y, int y_off);

#endif /* SWITCH_UI_H */
