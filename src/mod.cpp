#include <cstring>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"
#include "mods/svc/hook.hpp"

#include "d/d_com_inf_game.h"
#include "d/actor/d_a_player.h"
#include "d/d_item_data.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&dSv_save_c::init, SaveInit);

static constexpr int kHiddenSkillCount = 7;
static const u16 kHiddenSkillFlags[kHiddenSkillCount] = {
    dSv_event_flag_c::F_0339, dSv_event_flag_c::F_0338,
    dSv_event_flag_c::F_0340, dSv_event_flag_c::F_0341,
    dSv_event_flag_c::F_0342, dSv_event_flag_c::F_0343,
    dSv_event_flag_c::F_0344,
};

static constexpr int kBombBagLevelCount = 2;
static const u8 kBombBagLevels[kBombBagLevelCount] = {
    (u8)dItemNo_BOMB_BAG_LV1_e, (u8)dItemNo_BOMB_BAG_LV2_e,
};

static constexpr int kQuiverLevelCount = 3;
static const u8 kQuiverLevels[kQuiverLevelCount] = {
    (u8)dItemNo_ARROW_LV1_e, (u8)dItemNo_ARROW_LV2_e, (u8)dItemNo_ARROW_LV3_e,
};

static const int kMaxSelectItem = 4;
static const int kMaxItemSlots = 24;
static const int kDefaultMaxLife = 15;
static const int kStableFrames = 60;

ConfigVarHandle cfg_enabled;
ConfigVarHandle cfg_carry_hearts;
ConfigVarHandle cfg_carry_wallet;
ConfigVarHandle cfg_carry_bottles;
ConfigVarHandle cfg_carry_magic;
ConfigVarHandle cfg_carry_rupees;
ConfigVarHandle cfg_carry_skills;
ConfigVarHandle cfg_carry_bagquiver;
ConfigVarHandle cfg_carry_weapons;

static void register_bool(const char* name, bool def, ConfigVarHandle* out) {
    ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
    d.name = name;
    d.type = CONFIG_VAR_BOOL;
    d.default_bool = def;
    if (svc_config->register_var(mod_ctx, &d, out) != MOD_OK) {
        svc_log->error(mod_ctx, "NG+ failed: could not register an option");
    }
}

static void register_configs() {
    register_bool("carryoverEnabled", true, &cfg_enabled);
    register_bool("carryHearts", true, &cfg_carry_hearts);
    register_bool("carryWallet", true, &cfg_carry_wallet);
    register_bool("carryBottles", true, &cfg_carry_bottles);
    register_bool("carryMagic", true, &cfg_carry_magic);
    register_bool("carryRupees", false, &cfg_carry_rupees);
    register_bool("carrySkills", true, &cfg_carry_skills);
    register_bool("carryBagQuiver", true, &cfg_carry_bagquiver);
    register_bool("carryWeapons", true, &cfg_carry_weapons);
}

static void int_to_str(int value, char* out, size_t outSize) {
    char tmp[16];
    int len = 0;
    bool neg = value < 0;
    unsigned int uv = neg ? (unsigned int)(-value) : (unsigned int)value;
    if (uv == 0) tmp[len++] = '0';
    while (uv > 0 && len < (int)sizeof(tmp) - 1) {
        tmp[len++] = (char)('0' + (uv % 10));
        uv /= 10;
    }
    if (neg && len < (int)sizeof(tmp) - 1) tmp[len++] = '-';
    int j = 0;
    for (int i = len - 1; i >= 0 && j < (int)outSize - 1; i--, j++) {
        out[j] = tmp[i];
    }
    out[j] = '\0';
}

static void append_text(char* buf, size_t cap, const char* text) {
    size_t used = strlen(buf);
    size_t add = strlen(text);
    if (used + add + 1 < cap) {
        strcat(buf, text);
    }
}

static void append_int(char* buf, size_t cap, int value) {
    char num[16];
    int_to_str(value, num, sizeof(num));
    append_text(buf, cap, num);
}

static void append_name(char* buf, size_t cap, const char* name, int& count) {
    if (count > 0) {
        append_text(buf, cap, ", ");
    }
    append_text(buf, cap, name);
    count++;
}

static void show_toast(const char* title, const char* body, uint32_t durationMs) {
    UiToastDesc t = UI_TOAST_DESC_INIT;
    t.title_rml = title;
    t.body_rml = body;
    t.duration_ms = durationMs;
    svc_ui->push_toast(mod_ctx, &t);
}

static bool isBottleItemId(u8 itemId) {
    return itemId >= (u8)dItemNo_EMPTY_BOTTLE_e && itemId <= (u8)dItemNo_OIL_e;
}

static int g_snap_hearts = -1;
static int g_snap_wallet = 0;
static int g_snap_bottles = 0;
static int g_snap_magic = 0;
static int g_snap_rupees = 0;
static int g_snap_skills = 0;
static int g_snap_bombbag_level = 0;
static int g_snap_quiver_level = 0;
static int g_snap_sword = (int)dItemNo_NONE_e;
static int g_snap_shield = (int)dItemNo_NONE_e;
static bool g_snap_has_zora = false;
static bool g_snap_has_magic_armor = false;
static bool g_snap_has_hero_clothes = false;

static bool g_pending_apply = false;
static int g_player_frames = 0;

static char g_status_text[192];
static UiElementHandle g_status_elem = 0;
static bool g_status_dirty = false;

static void rebuild_status_text() {
    const size_t cap = sizeof(g_status_text);
    if (g_snap_hearts < 0) {
        strcpy(g_status_text, "No progress saved yet. Press the button below when you finish a run.");
        return;
    }

    int skillCount = 0;
    for (int i = 0; i < kHiddenSkillCount; i++) {
        if (g_snap_skills & (1 << i)) skillCount++;
    }

    g_status_text[0] = '\0';
    append_text(g_status_text, cap, "Saved: ");
    append_int(g_status_text, cap, g_snap_hearts / 5);
    append_text(g_status_text, cap, " hearts, ");
    append_int(g_status_text, cap, g_snap_bottles);
    append_text(g_status_text, cap, " bottles, ");
    append_int(g_status_text, cap, skillCount);
    append_text(g_status_text, cap, " Hidden Skills. Ready for your next new game.");
}

static void on_save_snapshot_pressed(ModContext*, void*) {
    dSv_player_status_a_c& st = g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();
    dSv_player_item_c& item = g_dComIfG_gameInfo.info.getPlayer().getItem();

    int bottleCount = 0;
    for (int i = 0; i < kMaxItemSlots; i++) {
        u8 itemId = item.getItem(i, false);
        if (isBottleItemId(itemId)) {
            bottleCount++;
        }
    }

    int skillMask = 0;
    for (int i = 0; i < kHiddenSkillCount; i++) {
        if (dComIfGs_isEventBit(kHiddenSkillFlags[i]) != 0) {
            skillMask |= (1 << i);
        }
    }

    int bombBagLevel = 0;
    for (int i = 0; i < kBombBagLevelCount; i++) {
        if (dComIfGs_isItemFirstBit(kBombBagLevels[i]) != 0) {
            bombBagLevel = i + 1;
        }
    }

    int quiverLevel = 0;
    for (int i = 0; i < kQuiverLevelCount; i++) {
        if (dComIfGs_isItemFirstBit(kQuiverLevels[i]) != 0) {
            quiverLevel = i + 1;
        }
    }

    g_snap_hearts = (int)dComIfGs_getMaxLife();
    g_snap_wallet = (int)st.getWalletSize();
    g_snap_bottles = bottleCount;
    g_snap_magic = (int)st.getMaxMagic();
    g_snap_rupees = (int)st.getRupee();
    g_snap_skills = skillMask;
    g_snap_bombbag_level = bombBagLevel;
    g_snap_quiver_level = quiverLevel;
    g_snap_sword = (int)st.mSelectEquip[1];
    g_snap_shield = (int)st.mSelectEquip[2];
    g_snap_has_zora = dComIfGs_isItemFirstBit((u8)dItemNo_WEAR_ZORA_e) != 0;
    g_snap_has_magic_armor = dComIfGs_isItemFirstBit((u8)dItemNo_ARMOR_e) != 0;
    g_snap_has_hero_clothes = dComIfGs_isCollectClothes(KOKIRI_CLOTHES_FLAG) != 0;

    g_status_dirty = true;

    svc_log->info(mod_ctx, "NG+ progress saved");
    show_toast("New Game+", "Progress saved. Start a new game to use it.", 4000);
}

static void apply_snapshot_to_current() {
    if (g_snap_hearts < 0) {
        return;
    }

    bool enabled = true;
    bool carryHearts = true;
    bool carryWallet = true;
    bool carryBottles = true;
    bool carryMagic = true;
    bool carryRupees = false;
    bool carrySkills = true;
    bool carryBagQuiver = true;
    bool carryWeapons = true;
    svc_config->get_bool(mod_ctx, cfg_enabled, &enabled);
    svc_config->get_bool(mod_ctx, cfg_carry_hearts, &carryHearts);
    svc_config->get_bool(mod_ctx, cfg_carry_wallet, &carryWallet);
    svc_config->get_bool(mod_ctx, cfg_carry_bottles, &carryBottles);
    svc_config->get_bool(mod_ctx, cfg_carry_magic, &carryMagic);
    svc_config->get_bool(mod_ctx, cfg_carry_rupees, &carryRupees);
    svc_config->get_bool(mod_ctx, cfg_carry_skills, &carrySkills);
    svc_config->get_bool(mod_ctx, cfg_carry_bagquiver, &carryBagQuiver);
    svc_config->get_bool(mod_ctx, cfg_carry_weapons, &carryWeapons);

    if (!enabled) {
        return;
    }

    dSv_player_status_a_c& st = g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA();

    char summary[160];
    summary[0] = '\0';
    int applied = 0;

    if (carryHearts) {
        st.setMaxLife((u8)g_snap_hearts);
        st.setLife((u16)g_snap_hearts);
        append_name(summary, sizeof(summary), "hearts", applied);
    }
    if (carryWallet) {
        st.setWalletSize((u8)g_snap_wallet);
        append_name(summary, sizeof(summary), "wallet", applied);
    }
    if (carryMagic && g_snap_magic > 0) {
        st.setMaxMagic((u8)g_snap_magic);
        st.setMagic((u8)g_snap_magic);
        append_name(summary, sizeof(summary), "magic", applied);
    }
    if (carryRupees) {
        st.setRupee((u16)g_snap_rupees);
        append_name(summary, sizeof(summary), "rupees", applied);
    }
    if (carryBottles && g_snap_bottles > 0) {
        int toGive = g_snap_bottles;
        if (toGive > kMaxSelectItem) toGive = kMaxSelectItem;
        for (int i = 0; i < toGive; i++) {
            dComIfGs_setEmptyBottle();
        }
        append_name(summary, sizeof(summary), "bottles", applied);
    }
    if (carrySkills && g_snap_skills > 0) {
        for (int i = 0; i < kHiddenSkillCount; i++) {
            if (g_snap_skills & (1 << i)) {
                dComIfGs_onEventBit(kHiddenSkillFlags[i]);
            }
        }
        append_name(summary, sizeof(summary), "Hidden Skills", applied);
    }
    if (carryBagQuiver) {
        bool any = false;
        for (int i = 0; i < g_snap_bombbag_level && i < kBombBagLevelCount; i++) {
            dComIfGs_onItemFirstBit(kBombBagLevels[i]);
            any = true;
        }
        for (int i = 0; i < g_snap_quiver_level && i < kQuiverLevelCount; i++) {
            dComIfGs_onItemFirstBit(kQuiverLevels[i]);
            any = true;
        }
        if (any) {
            append_name(summary, sizeof(summary), "bag & quiver", applied);
        }
    }
    if (carryWeapons) {
        bool any = false;
        if (g_snap_sword != (int)dItemNo_NONE_e) {
            dComIfGs_onItemFirstBit((u8)g_snap_sword);
            st.mSelectEquip[1] = (u8)g_snap_sword;
            any = true;
        }
        if (g_snap_shield != (int)dItemNo_NONE_e) {
            dComIfGs_onItemFirstBit((u8)g_snap_shield);
            st.mSelectEquip[2] = (u8)g_snap_shield;
            any = true;
        }
        if (g_snap_has_zora) {
            dComIfGs_onItemFirstBit((u8)dItemNo_WEAR_ZORA_e);
            any = true;
        }
        if (g_snap_has_magic_armor) {
            dComIfGs_onItemFirstBit((u8)dItemNo_ARMOR_e);
            any = true;
        }
        if (g_snap_has_hero_clothes) {
            dComIfGs_setCollectClothes(KOKIRI_CLOTHES_FLAG);
            any = true;
        }
        if (any) {
            append_name(summary, sizeof(summary), "equipment", applied);
        }
    }

    if (applied > 0) {
        char logLine[200];
        strcpy(logLine, "NG+ applied: ");
        append_text(logLine, sizeof(logLine), summary);
        svc_log->info(mod_ctx, logLine);

        char toastBody[200];
        strcpy(toastBody, "Carried over: ");
        append_text(toastBody, sizeof(toastBody), summary);
        show_toast("New Game+ loaded", toastBody, 6000);
    }
}

static void on_save_init_post(ModContext*, void*, void*, void*) {
    g_pending_apply = true;
    g_player_frames = 0;
}

static void add_toggle(UiElementHandle panel, const char* label, ConfigVarHandle var) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_TOGGLE;
    c.label = label;
    c.binding = UI_BINDING_CONFIG_VAR;
    c.config_var = var;
    svc_ui->pane_add_control(mod_ctx, panel, &c, nullptr);
}

static void add_button(UiElementHandle panel, const char* label, UiPressedFn fn) {
    UiControlDesc c = UI_CONTROL_DESC_INIT;
    c.kind = UI_CONTROL_BUTTON;
    c.label = label;
    c.on_pressed = fn;
    svc_ui->pane_add_control(mod_ctx, panel, &c, nullptr);
}

static ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "New Game+");

    const char* description = R"(<p><b>Carry your last run into a fresh save.</b></p>
<p>1. Finish (or pause) your playthrough.<br/>
2. Press <b>Save Progress for NG+</b> below.<br/>
3. Start a new game on an empty slot. Your progress is applied a moment after the game begins.</p>)";

    if (svc_ui->pane_add_rml(mod_ctx, panel, description, nullptr) != MOD_OK) {
        svc_ui->pane_add_text(mod_ctx, panel,
            "Carry your last run into a fresh save. Press Save Progress for NG+, then start a new game.",
            nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, panel, "Your saved progress");
    g_status_elem = 0;
    rebuild_status_text();
    svc_ui->pane_add_text(mod_ctx, panel, g_status_text, &g_status_elem);
    add_button(panel, "Save Progress for NG+", on_save_snapshot_pressed);

    svc_ui->pane_add_section(mod_ctx, panel, "Options");
    add_toggle(panel, "Enable New Game+", cfg_enabled);

    svc_ui->pane_add_section(mod_ctx, panel, "Character");
    add_toggle(panel, "Hearts", cfg_carry_hearts);
    add_toggle(panel, "Magic meter", cfg_carry_magic);
    add_toggle(panel, "Wallet size", cfg_carry_wallet);
    add_toggle(panel, "Rupees (exact amount)", cfg_carry_rupees);

    svc_ui->pane_add_section(mod_ctx, panel, "Items and gear");
    add_toggle(panel, "Bottles", cfg_carry_bottles);
    add_toggle(panel, "Bomb Bag and Quiver size", cfg_carry_bagquiver);
    add_toggle(panel, "Equipped weapons and armor", cfg_carry_weapons);

    svc_ui->pane_add_section(mod_ctx, panel, "Progress");
    add_toggle(panel, "Hidden Skills", cfg_carry_skills);

    return MOD_OK;
}

static ModResult on_panel_update(ModContext*, void*, ModError*) {
    if (g_status_dirty && g_status_elem != 0) {
        rebuild_status_text();
        svc_ui->elem_set_text(mod_ctx, g_status_elem, g_status_text);
        g_status_dirty = false;
    }
    return MOD_OK;
}

extern "C" {

    MOD_EXPORT ModResult mod_initialize(ModError* error) {
        register_configs();

        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = build_panel;
        panel.update = on_panel_update;
        svc_ui->register_mods_panel(mod_ctx, &panel);

        ModResult hookResult = mods::hook::add_post<SaveInit>(on_save_init_post);
        if (hookResult != MOD_OK) {
            svc_log->error(mod_ctx, "NG+ failed: could not install the new-game hook");
        }
        else {
            svc_log->info(mod_ctx, "New Game+ loaded");
        }

        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_update(ModError* error) {
        if (!g_pending_apply) {
            return MOD_OK;
        }

        if (daPy_getPlayerActorClass() == nullptr) {
            g_player_frames = 0;
            return MOD_OK;
        }

        g_player_frames++;
        if (g_player_frames < kStableFrames) {
            return MOD_OK;
        }

        g_pending_apply = false;
        g_player_frames = 0;

        if ((int)dComIfGs_getMaxLife() != kDefaultMaxLife) {
            return MOD_OK;
        }

        apply_snapshot_to_current();
        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_shutdown(ModError* error) {
        return MOD_OK;
    }

}