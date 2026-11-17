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
    {"Dole Collaboration", "Doleコラボ", "Collaboration Dole", "Collaborazione Dole", "Dole-Kollaboration", "Colaboración con Dole", "Dole联动",
     "Dole聯動", "Dole 컬래버레이션", "Colaboração Dole"},
    {"Mouse Sensitivity", "マウスカーソル感度", "Sensibilité de la souris", "Sensibilità mouse", "Mausempfindlichkeit",
     "Sensibilidad del ratón", "光标灵敏度", "滑鼠游標靈敏度", "마우스 커서 감도", "Sensibilidade do mouse"},      // the game's own texts
    {"On", "オン", "Activé", "Sì", "An", "Sí", "开", "開", "켜기", "Ativado"},
    {"Off", "オフ", "Désactivé", "No", "Aus", "No", "关", "關", "끄기", "Desativado"},
    {"Bananas", "バナナ", "Bananes", "Banane", "Bananen", "Plátanos", "香蕉", "香蕉", "바나나", "Bananas"},
    {"Auto", "オート", "Auto", "Auto", "Auto", "Automático", "自动", "自動", "자동", "Automático"},
    {"Rings", "リング", "Anneaux", "Anelli", "Ringe", "Anillos", "金环", "金環", "링", "Anéis"},
    {"Spray Cans", "スプレー缶", "Bombes de peinture", "Bombolette", "Spraydosen", "Botes de spray", "喷漆罐", "噴漆罐", "스프레이 캔",
     "Latas de spray"},
    {"Coins", "コイン", "Pièces", "Monete", "Münzen", "Monedas", "硬币", "硬幣", "코인", "Moedas"},
    {"Notes", "音符", "Notes", "Note", "Noten", "Notas", "音符", "音符", "음표", "Notas"},
    {"Trains", "電車", "Trains", "Treni", "Züge", "Trenes", "电车", "電車", "전철", "Trens"},
    {"Pizza", "ピザ", "Pizzas", "Pizza", "Pizza", "Pizza", "披萨", "披薩", "피자", "Pizza"},
    {"Feathers", "羽根", "Plumes", "Piume", "Federn", "Plumas", "羽毛", "羽毛", "깃털", "Penas"},
    {"Pac-Dots", "クッキー", "Pac-gommes", "Pac-Dots", "Pac-Dots", "Pac-Dots", "Pac-Dots", "Pac-Dots", "Pac-Dots", "Pac-Dots"},
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
    // option captions (display_opt.c)
    {"Choose whether the game is shown in a window,\nborderless, or in full screen!",
     "ゲームの表示方法をウィンドウ、ボーダーレス、\nフルスクリーンから選べるよ",
     "Choisis l'affichage du jeu : fenêtré,\nsans bordure ou plein écran !",
     "Scegli se mostrare il gioco in una finestra,\nsenza bordi o a schermo intero!",
     "Wähle, ob das Spiel im Fenster, randlos\noder im Vollbild angezeigt wird!",
     "Elige si el juego se muestra en ventana,\nsin bordes o a pantalla completa.",
     "可以选择游戏以窗口、无边框\n或全屏方式显示。",
     "可選擇遊戲以視窗、無邊框\n或全螢幕方式顯示喔",
     "게임을 창 모드, 테두리 없음, 전체 화면 중\n어떻게 표시할지 선택할 수 있어",
     "Escolha se o jogo aparece em janela,\nsem bordas ou em tela cheia!"},
    {"Set the resolution of the game!\n*Borderless always uses the size of your desktop.",
     "ゲーム画面の解像度を設定できるよ\n※ボーダーレスではデスクトップと同じサイズになるよ",
     "Règle la résolution du jeu !\n*Sans bordure, la taille du bureau est toujours utilisée.",
     "Imposta la risoluzione del gioco!\n*Senza bordi viene sempre usata la dimensione del desktop.",
     "Stelle die Auflösung des Spiels ein!\n*Randlos nutzt immer die Größe des Desktops.",
     "Ajusta la resolución del juego.\n*En el modo sin bordes se usa el tamaño del escritorio.",
     "可以设置游戏画面的分辨率。\n※无边框模式下始终使用桌面的尺寸。",
     "可設定遊戲畫面的解析度喔\n※無邊框模式下一律使用桌面的尺寸喔",
     "게임 화면의 해상도를 설정할 수 있어\n※테두리 없음 모드에서는 항상 바탕 화면 크기로 표시",
     "Define a resolução do jogo!\n*Sem bordas, o tamanho da área de trabalho é sempre usado."},
    {"Set the maximum frame rate!\nThe higher the number, the smoother the motion!",
     "フレームレートの上限を設定できるよ\n値が大きいほど動きがなめらかになるよ",
     "Règle la limite d'images par seconde !\nPlus elle est haute, plus les mouvements sont fluides !",
     "Imposta il limite di fotogrammi al secondo!\nPiù alto è il numero, più fluidi saranno i movimenti!",
     "Stelle die maximale Bildrate ein!\nJe höher die Zahl, desto flüssiger die Bewegungen!",
     "Ajusta el límite de fotogramas por segundo.\nCuanto mayor sea el valor, más fluido será el movimiento.",
     "可以设置帧率的上限。\n数值越大，画面越流畅。",
     "可設定幀率的上限喔\n值愈大畫面就愈流暢",
     "프레임 레이트의 상한을 설정할 수 있어\n수치가 클수록 움직임이 부드러워져",
     "Define o limite de quadros por segundo!\nQuanto maior o número, mais suaves os movimentos!"},
    {"Toggle syncing the picture to your monitor ON or OFF!\nWhen set to ON, screen tearing is prevented!",
     "画面をモニターに同期させる機能のＯＮ／ＯＦＦを切りかえるよ\nＯＮにすると画面のズレ（ティアリング）を防げるよ",
     "Active ou désactive la synchronisation verticale !\nActivée, elle évite les déchirures de l'image !",
     "Attiva o disattiva la sincronia verticale!\nSe attiva, evita lo strappo dell'immagine!",
     "Schalte die vertikale Synchronisation EIN oder AUS!\nIst sie EIN, wird Bildreißen verhindert!",
     "Activa o desactiva la sincronización vertical.\nSi está activada, se evita el desgarro de imagen.",
     "可以切换垂直同步的开／关。\n开启后可防止画面撕裂。",
     "可切換垂直同步的ＯＮ／ＯＦＦ喔\n開啟後可防止畫面撕裂",
     "수직 동기화의 ON/OFF 여부를 설정할 수 있어\nON으로 하면 화면 찢어짐을 막을 수 있어",
     "Liga ou desliga a sincronização vertical!\nQuando ligada, evita o rasgo da imagem!"},
    {"Set picture sharpness and the detail of distant models!\nHigher settings need a faster PC!",
     "画面の精細さと遠くのモデルの細かさを設定できるよ\n高い設定ほどＰＣの性能が必要になるよ",
     "Règle la netteté et le détail des modèles lointains !\nLes réglages élevés demandent un PC plus puissant !",
     "Imposta la nitidezza e il dettaglio dei modelli lontani!\nLe impostazioni più alte richiedono un PC più potente!",
     "Stelle Bildschärfe und Details entfernter Modelle ein!\nHöhere Einstellungen brauchen einen schnelleren PC!",
     "Ajusta la nitidez y el detalle de los modelos lejanos.\nLos ajustes altos requieren un PC más potente.",
     "可以设置画面的清晰度和远处模型的精细度。\n设置越高，对电脑性能的要求越高。",
     "可設定畫面的清晰度和遠處模型的精細度喔\n設定愈高，對電腦效能的要求就愈高",
     "화면의 선명도와 먼 모델의 디테일을 설정할 수 있어\n설정이 높을수록 PC 성능이 더 필요해",
     "Define a nitidez da imagem e o detalhe dos modelos distantes!\nConfigurações mais altas exigem um PC mais potente!"},
    {"Set the quality of shadows!\nThe higher the setting, the sharper the shadows!",
     "影のきめ細かさを設定できるよ\n高い設定ほど影がくっきりするよ",
     "Règle la qualité des ombres !\nPlus le réglage est élevé, plus les ombres sont nettes !",
     "Imposta la qualità delle ombre!\nPiù alta è l'impostazione, più nitide saranno le ombre!",
     "Stelle die Qualität der Schatten ein!\nJe höher die Einstellung, desto schärfer die Schatten!",
     "Ajusta la calidad de las sombras.\nCuanto más alto sea el ajuste, más nítidas serán.",
     "可以设置阴影的品质。\n设置越高，阴影越清晰。",
     "可設定陰影的品質喔\n設定愈高陰影就愈清晰",
     "그림자의 품질을 설정할 수 있어\n설정이 높을수록 그림자가 선명해져",
     "Define a qualidade das sombras!\nQuanto maior a configuração, mais nítidas as sombras!"},
    {"Set screen effects such as glow and color tone!\nThe highest setting also smooths jagged edges!",
     "光のにじみや色味などの画面効果を設定できるよ\nいちばん高い設定ではギザギザもなめらかになるよ",
     "Règle les effets d'image comme la lueur et les couleurs !\nLe réglage le plus élevé adoucit aussi les bords crénelés !",
     "Imposta effetti come il bagliore e la tonalità dei colori!\nIl livello più alto ammorbidisce anche i bordi frastagliati!",
     "Stelle Bildeffekte wie Leuchten und Farbton ein!\nDie höchste Stufe glättet auch gezackte Kanten!",
     "Ajusta efectos como el resplandor y el tono de color.\nEl ajuste más alto también suaviza los bordes dentados.",
     "可以设置光晕、色调等画面效果。\n最高设置还会让锯齿边缘更平滑。",
     "可設定光暈、色調等畫面效果喔\n最高設定還會讓鋸齒邊緣更平滑",
     "빛 번짐이나 색감 등의 화면 효과를 설정할 수 있어\n가장 높은 설정에서는 계단 현상도 부드럽게 해 줘",
     "Define efeitos de imagem como brilho e tom de cor!\nA configuração mais alta também suaviza bordas serrilhadas!"},
    {"Toggle smoothing of jagged edges ON or OFF!",
     "ふちのギザギザをなめらかにする\n機能のＯＮ／ＯＦＦを切りかえるよ",
     "Active ou désactive le lissage des bords crénelés !",
     "Attiva o disattiva l'ammorbidimento dei bordi frastagliati!",
     "Schalte die Kantenglättung EIN oder AUS!",
     "Activa o desactiva el suavizado de bordes dentados.",
     "可以切换锯齿边缘平滑处理的开／关。",
     "可切換鋸齒邊緣平滑處理的ＯＮ／ＯＦＦ喔",
     "계단 현상 완화 기능의 ON/OFF 여부를 설정할 수 있어",
     "Liga ou desliga a suavização de bordas serrilhadas!"},
    {"Toggle keeping textures sharp at steep angles ON or OFF!",
     "ななめから見たテクスチャをくっきり表示する\n機能のＯＮ／ＯＦＦを切りかえるよ",
     "Active ou désactive la netteté des textures vues de biais !",
     "Attiva o disattiva la nitidezza delle texture di sbieco!",
     "Schalte scharfe Texturen bei schrägem Blick EIN oder AUS!",
     "Activa o desactiva la nitidez de texturas en ángulo.",
     "可以切换斜向观看时纹理清晰显示的开／关。",
     "可切換斜向觀看時紋理清晰顯示的ＯＮ／ＯＦＦ喔",
     "비스듬히 보이는 텍스처를 선명하게 표시하는\n기능의 ON/OFF 여부를 설정할 수 있어",
     "Liga ou desliga a nitidez das texturas vistas em ângulo!"},
    {"Set how much the Stage tilts when dragging the mouse!\nThe higher the number, the more the Stage will tilt!",
     "マウスのドラッグでステージがかたむく度合いを調整できるよ\n値が大きいほどステージがかたむくようになるよ",
     "Règle l'inclinaison quand tu fais glisser la souris !\nPlus cette valeur est haute, plus les niveaux s'inclinent !",
     "Imposta quanto si inclinano le fasi trascinando il mouse!\nPiù alto è il numero, più si inclineranno le fasi!",
     "Stelle ein, wie stark sich das Level beim Mausziehen neigt!\nJe höher die Zahl, desto stärker neigt sich das Level!",
     "Ajusta cuánto se inclina el escenario al arrastrar el ratón.\nCuanto mayor sea el valor, más se inclinará.",
     "可以调整拖动鼠标时平台倾斜的程度。\n数值越大，平台越倾斜。",
     "可調整拖曳滑鼠時場地傾斜的程度喔\n值愈大場地就愈傾斜",
     "마우스를 드래그할 때 스테이지가 기우는 정도를 조정할 수 있어\n수치가 클수록 스테이지가 많이 기울어",
     "Define o quanto os estágios inclinam ao arrastar o mouse!\nQuanto maior o número, mais os estágios inclinam!"},
    {"Toggle the Dole collaboration look ON or OFF!\nWhen set to ON, Dole bananas and logos appear!",
     "Doleコラボの演出のＯＮ／ＯＦＦを切りかえるよ\nＯＮにするとDoleのバナナやロゴが登場するよ",
     "Active ou désactive l'apparence de la collaboration Dole !\nActivée, les bananes et logos Dole apparaissent !",
     "Attiva o disattiva l'aspetto della collaborazione Dole!\nSe attiva, compaiono banane e loghi Dole!",
     "Schalte den Look der Dole-Kollaboration EIN oder AUS!\nIst er EIN, erscheinen Dole-Bananen und -Logos!",
     "Activa o desactiva el aspecto de la colaboración con Dole.\nSi está activado, aparecen plátanos y logotipos de Dole.",
     "可以切换Dole联动外观的开／关。\n开启后会出现Dole香蕉和标志。",
     "可切換Dole聯動外觀的ＯＮ／ＯＦＦ喔\n開啟後會出現Dole香蕉和標誌",
     "Dole 컬래버레이션 연출의 ON/OFF 여부를 설정할 수 있어\nON으로 하면 Dole 바나나와 로고가 등장해",
     "Liga ou desliga o visual da colaboração Dole!\nQuando ligado, bananas e logos Dole aparecem!"},
    {"Choose what you collect in Stages!\nAuto uses each character's own collectible!",
     "ステージで集めるアイテムを選べるよ\nオートではキャラクターごとのアイテムになるよ",
     "Choisis ce que tu ramasses dans les niveaux !\nAuto utilise l'objet propre à chaque personnage !",
     "Scegli cosa raccogliere nelle fasi!\nAuto usa l'oggetto di ciascun personaggio!",
     "Wähle, was du in den Levels sammelst!\nAuto nutzt das Sammelobjekt der jeweiligen Figur!",
     "Elige qué recoges en los escenarios.\nAutomático usa el objeto propio de cada personaje.",
     "可以选择在平台中收集的物品。\n自动时使用各角色专属的物品。",
     "可選擇在場地中收集的物品喔\n自動時會使用各角色專屬的物品",
     "스테이지에서 모을 아이템을 선택할 수 있어\n자동은 캐릭터마다 고유한 아이템을 사용해",
     "Escolha o que você coleta nos estágios!\nAutomático usa o item próprio de cada personagem!"},
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
