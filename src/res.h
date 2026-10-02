#ifndef M95_RES_H
#define M95_RES_H

/* program icon (lowest icon id becomes the exe's default icon) */
#define IDI_APP            1

/* menus */
#define IDM_FILE_OPEN        101
#define IDM_FILE_OPENFOLDER  102
#define IDM_FILE_RESCAN      103
#define IDM_FILE_IMPORTM3U   104
#define IDM_FILE_EXPORTM3U   105
#define IDM_FILE_EXIT        106
#define IDM_PB_PLAYPAUSE     111
#define IDM_PB_STOP          112
#define IDM_PB_PREV          113
#define IDM_PB_NEXT          114
#define IDM_PB_LOOP          115
#define IDM_PB_REPEAT        116
#define IDM_PB_MUTE          117
#define IDM_HELP_SETTINGS    121
#define IDM_HELP_ABOUT       122

/* playlists / favorites / ignore */
#define IDM_PL_NEW           130
#define IDM_PL_RENAME        131
#define IDM_PL_DELETE        132
#define IDM_PL_FAV           133
#define IDM_PL_UNFAV         134
#define IDM_PL_IGNORE        135
#define IDM_PL_MANAGEIGN     136

/* keyboard behavior parity with the original */
#define IDM_SEEK_BACK        140
#define IDM_SEEK_FWD         141
#define IDM_SEEK_BACK30      142
#define IDM_SEEK_FWD30       143
#define IDM_VOL_UP           144
#define IDM_VOL_DOWN         145
#define IDM_VOL_ZERO         146
#define IDM_TOGGLE_LOOP      147
#define IDM_TOGGLE_REPEAT    148
#define IDM_PL_SHUFFLENOW    149
#define IDM_FOCUS_SEARCH     150
#define IDM_REVEAL_PLAYING   151
#define IDM_CLEAR_SEARCH     152
#define IDM_PLAY_SELECTED    153
#define IDM_VIEW_TRACKER     154
#define IDM_TRK_CHL          155
#define IDM_TRK_CHR          156

/* top rows */
#define IDC_BTN_OPENFOLDER   201
#define IDC_BTN_RESCAN       202
#define IDC_EDIT_SEARCH      203
#define IDC_STATIC_COUNT     204
#define IDC_COMBO_SOURCE     205
#define IDC_COMBO_ORDER      206
#define IDC_BTN_SHUFFLENOW   207
#define IDC_BTN_FILTER       208
#define IDC_BTN_ADDPL        209
#define IDC_BTN_CLEARPL      210

/* main list / right panel */
#define IDC_LIST             220
#define IDC_STATIC_TITLE     221
#define IDC_STATIC_FORMAT    222
#define IDC_STATIC_TRACKER   223
#define IDC_STATIC_LENGTH    224
#define IDC_STATIC_POSITION  225
#define IDC_STATIC_SEQUENCER 226
#define IDC_STATIC_OUTPUT    227
#define IDC_COMBO_SUBSONG    228
#define IDC_STATIC_OF        229
#define IDC_CHK_PLAYALL      230
#define IDC_EDIT_LOG         231

/* transport */
#define IDC_BTN_PREV         240
#define IDC_BTN_PLAYPAUSE    241
#define IDC_BTN_NEXT         242
#define IDC_BTN_STOP         243
#define IDC_CHK_LOOP         244
#define IDC_CHK_REPEAT       245
#define IDC_BTN_MUTE         246
#define IDC_TRK_VOLUME       247
#define IDC_STATIC_TIME      248
#define IDC_TRK_SEEK         249
#define IDC_STATIC_DUR       250
#define IDC_STATUSBAR        251
#define IDC_STATIC_VOLPCT    252
#define IDC_TRACKER          253
#define IDC_TAB              254

/* dialogs */
#define IDD_SETTINGS         300
#define IDC_SET_RATE         301
#define IDC_SET_INTERP       302
#define IDC_SET_BUFFER       303
#define IDC_SET_TRKDELAY     304
#define IDC_SET_TITLE        305
#define IDC_SET_REFRESH      306
#define IDD_FILTER           310
#define IDC_FLT_MINLEN       311
#define IDC_FLT_PLAYABLE     312
#define IDD_ABOUT            320
#define IDD_INPUT            330
#define IDC_INPUT_TEXT       331
#define IDD_IGNORED          335
#define IDC_IGN_LIST         336
#define IDC_IGN_REMOVE       337
#define IDC_IGN_CLEARALL     338
#define IDC_FLT_MAXLEN       340
#define IDC_FLT_CLEAR        341
#define IDC_FLT_GROUP        342
#define IDC_ABOUT_EDITION    343
#define IDC_ABOUT_FORMATS    344

/* custom messages */
#define WM_APP_ENDED         (WM_APP + 1)
#define WM_APP_ANALYZED      (WM_APP + 2)
#define WM_APP_SEARCHTICK    (WM_APP + 3)
#define WM_APP_LOG           (WM_APP + 4)
#define WM_APP_ROWS          (WM_APP + 5)

#endif
