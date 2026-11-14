// The port's own texts in the game's language.
//
// The game is translated into eleven languages and has a Language page in Options; the few texts the port adds
// (option rows, two questions, the name box) were English only. rt_tr_str(c, english) gives the managed string for
// the language the game is in (LocalizationManager.language); a text without an entry, or a language without a
// translation, stays English. The translations were written with the port, not by native speakers. This file is
// UTF-8.
#include <windows.h>
#include <string.h>
#include "rt.h"

#define FN(off) (GUEST_BASE + (off))
#define F_STRING_NEW_UTF16 0x45af94ull      // il2cpp::vm::String::NewUtf16(const Il2CppChar *, int32_t)
#define F_GET_LANGUAGE 0x43d0100ull         // Wonder.LocalizationManager.get_language (static): 0 while not ready

// Wonder.DB.Def.Language.eID: 1 Japanese, 2 English, 3 French, 4 Italian, 5 German, 6 Spanish, 7 Chinese
// (Simplified), 8 Chinese (Traditional), 9 Korean, 10 Portuguese, 11 Spanish (Latin America), 12 Portuguese (Brazil)
typedef struct { const char *en, *ja, *fr, *it, *de, *es, *zhs, *zht, *ko, *pt; } Text;
static const Text g_text[] = {
    {"Resolution", "解像度", "Résolution", "Risoluzione", "Auflösung", "Resolución", "分辨率", "解析度", "해상도", "Resolução"},
    {"Frame Rate Limit", "フレームレート上限", "Limite d'images/s", "Limite fotogrammi", "Bildratenlimit", "Límite de fotogramas", "帧率上限", "幀率上限",
     "프레임 제한", "Limite de quadros"},
    {"VSync", "垂直同期", "Synchro verticale", "Sincronia verticale", "VSync", "Sincronización vertical", "垂直同步", "垂直同步", "수직 동기화",
     "Sincronização vertical"},
    {"Smooth Motion", "なめらかな動き", "Mouvement fluide", "Movimento fluido", "Flüssige Bewegung", "Movimiento fluido", "流畅动作", "流暢動作",
     "부드러운 움직임", "Movimento suave"},
    {"On", "オン", "Activé", "Sì", "An", "Sí", "开", "開", "켜기", "Ativado"},
    {"Off", "オフ", "Désactivé", "No", "Aus", "No", "关", "關", "끄기", "Desativado"},
    {"Borderless", "ボーダーレス", "Sans bordure", "Senza bordi", "Randlos", "Sin bordes", "无边框", "無邊框", "테두리 없음", "Sem bordas"},
    {"Unlimited", "無制限", "Illimitée", "Illimitato", "Unbegrenzt", "Ilimitado", "无限制", "無限制", "무제한", "Ilimitado"},
    {"Quit the game?", "ゲームを終了しますか？", "Quitter le jeu ?", "Uscire dal gioco?", "Spiel beenden?", "¿Salir del juego?", "要退出游戏吗？",
     "要結束遊戲嗎？", "게임을 종료하시겠습니까?", "Sair do jogo?"},
    {"Would you like to skip the intro tutorial?", "はじめのチュートリアルをスキップしますか？", "Voulez-vous passer le tutoriel d'introduction ?",
     "Vuoi saltare il tutorial introduttivo?", "Möchtest du das Einführungstutorial überspringen?", "¿Quieres saltarte el tutorial inicial?",
     "要跳过入门教程吗？", "要跳過入門教學嗎？", "튜토리얼을 건너뛰시겠습니까?", "Quer pular o tutorial inicial?"},
    {"Set your online display name!", "オンラインでの表示名を決めよう！", "Choisis ton nom en ligne !", "Scegli il tuo nome online!",
     "Lege deinen Online-Namen fest!", "¡Elige tu nombre en línea!", "设置你的在线显示名称！", "設定你的線上顯示名稱！", "온라인 표시 이름을 정하세요!",
     "Escolha seu nome online!"},
    {"Enter: OK      Esc: Cancel", "Enter：決定      Esc：キャンセル", "Entrée : OK      Échap : Annuler", "Invio: OK      Esc: Annulla",
     "Enter: OK      Esc: Abbrechen", "Intro: Aceptar      Esc: Cancelar", "Enter：确定      Esc：取消", "Enter：確定      Esc：取消",
     "Enter: 확인      Esc: 취소", "Enter: OK      Esc: Cancelar"},
    {"Imported Stage Music", "インポートステージの音楽", "Musique des niveaux importés", "Musica dei livelli importati", "Musik importierter Level",
     "Música de niveles importados", "导入关卡音乐", "匯入關卡音樂", "가져온 스테이지 음악", "Música das fases importadas"},
};

// The text for the game's current language (UTF-8), or `english` itself.
const char *rt_tr(Cpu *c, const char *english) {
    int lang = (int32_t)rt_call(c, FN(F_GET_LANGUAGE), 1, 0ull);
    if (lang <= 0 || lang == 2) return english;
    for (size_t i = 0; i < sizeof g_text / sizeof *g_text; i++) {
        const Text *t = &g_text[i];
        if (strcmp(t->en, english)) continue;
        const char *s = lang == 1 ? t->ja : lang == 3 ? t->fr : lang == 4 ? t->it : lang == 5 ? t->de : lang == 6 || lang == 11 ? t->es :
                        lang == 7 ? t->zhs : lang == 8 ? t->zht : lang == 9 ? t->ko : lang == 10 || lang == 12 ? t->pt : NULL;
        return s ? s : english;
    }
    return english;
}
// The same as UTF-16 code units; returns their number.
int rt_tr_utf16(Cpu *c, const char *english, uint16_t *out, int max) {
    int n = MultiByteToWideChar(CP_UTF8, 0, rt_tr(c, english), -1, (LPWSTR)out, max);
    return n > 0 ? n - 1 : 0;
}
// A managed string of `english` in the game's language.
uint64_t rt_tr_str(Cpu *c, const char *english) {
    uint16_t w[256];
    int n = rt_tr_utf16(c, english, w, 256);
    return rt_call(c, FN(F_STRING_NEW_UTF16), 2, (uint64_t)(uintptr_t)w, (uint64_t)n);
}
