/* This file is evident by the split TUs in symbol map. Unsure of proper filename. */

#include "ac_animal_logo.h"

#include "m_common_data.h"
#include "m_npc.h"
#include "m_name_table.h"
#include "m_play.h"
#include "m_time.h"
#include "m_titledemo.h"
#include "m_event.h"
#include "m_scene_table.h"
#include "libultra/libultra.h"
#include "m_cpak.h"
#include "m_card.h"
#include "m_home.h"
#include "m_house.h"
#include "m_quest.h"
#include "m_bgm.h"
#ifdef TARGET_PC
#include "client/client_network.h"
#endif

extern void flash_rom_and_player_info_clear() {
    Save* save = Common_GetPointer(save);
    Private_c* priv_p;
    int i;

    bzero(save, sizeof(Save));
    mFRm_ClearSaveCheckData(Save_GetPointer(save_check));

    priv_p = Save_GetPointer(private_data[0]);
    for (i = 0; i < PLAYER_NUM; i++) {
        mPr_ClearPrivateInfo(priv_p);
        priv_p++;
    }

    Save_Set(land_info.exists, TRUE);
}

extern int decide_next_scene_no() {
    int is_first_game;
    int bad_save;
    int next_scene_no;

    is_first_game = mFRm_CheckSaveData() == FALSE;

    // this is weird, can't get it as a ternary or a switch
    switch ((int)Common_Get(save_error_type)) {
        case mFRm_ERROR_MEMCARD_TRAVELDATA_CONFLICT:
        case mFRm_ERROR_DATA_CORRUPTED:
        case mFRm_ERROR_MEMCARD_NO_FILES:
        case mFRm_ERROR_MEMCARD_DAMAGED:
        case mFRm_ERROR_MEMCARD_FORMAT:
        case mFRm_ERROR_MEMCARD_INVALID:
        case mFRm_ERROR_MEMCARD_NO_SPACE:
        case mFRm_ERROR_NOT_MEMCARD:
        case mFRm_ERROR_NO_MEMCARD: {
            bad_save = TRUE;
            break;
        }
        default: {
            bad_save = FALSE;
            break;
        }
    }

    if (lbRTC_IsAbnormal()) {
        const lbRTC_time_c* src;

        Common_Set(time.rtc_crashed, TRUE);
        Common_Set(time.add_sec, 1);

        if (is_first_game == TRUE || bad_save == TRUE) {
            src = &mTM_rtcTime_default_code;
        } else {
            src = Save_GetPointer(save_check.time);
        }

        lbRTC_TimeCopy(Common_GetPointer(newly_set_time), src);
    }

    if (bad_save == TRUE || Common_Get(time.rtc_crashed) == TRUE) {
        if (bad_save == TRUE) {
            flash_rom_and_player_info_clear();
        }

        Common_Set(house_owner_name, RSV_NO);
        Common_Set(last_field_id, -1);
        next_scene_no = SCENE_PLAYERSELECT_3;
    } else if (is_first_game == TRUE) {
        flash_rom_and_player_info_clear();
        Common_Set(house_owner_name, RSV_NO);
        Common_Set(last_field_id, -1);
        next_scene_no = SCENE_PLAYERSELECT;
    } else {
#if defined(TARGET_PC)
        int player_sum = 0;
        for (int p = 0; p < PLAYER_NUM; p++) {
            if (!mPr_NullCheckPersonalID(&Save_Get(private_data[p]).player_ID) &&
                Save_Get(private_data[p]).exists == TRUE) {
                player_sum++;
            }
        }
        if (player_sum == 0) {
            next_scene_no = SCENE_START_DEMO2;
        } else {
            next_scene_no = SCENE_PLAYERSELECT_2;
        }
#else
        next_scene_no = SCENE_PLAYERSELECT_2;
#endif
    }

    return next_scene_no;
}

extern void title_action_data_init_start_select(GAME_PLAY* play) {
    mCD_LoadLand();
    mCPk_InitPak(0);

    Common_Set(now_private, Save_GetPointer(private_data[0]));
    Common_Set(player_no, 0);

    mTD_rtc_reserve();
    mTM_clear_renew_is();
    mNpc_ClearAnimalInfo(mNpc_GetInAnimalP());
#if defined(TARGET_PC) && !defined(TARGET_SERVER)
    {
        extern bool client_network_is_connected(void);
        extern bool client_network_is_new_player(void);
        extern ClientNetState client_network_get_state(void);
        extern void client_network_poll(void);
        if (client_network_get_state() == CLIENT_NET_CONNECTING ||
            client_network_get_state() == CLIENT_NET_HANDSHAKING) {
            uint32_t start_ms = SDL_GetTicks();
            while ((client_network_get_state() == CLIENT_NET_CONNECTING ||
                    client_network_get_state() == CLIENT_NET_HANDSHAKING) &&
                   (SDL_GetTicks() - start_ms) < 2000) {
                client_network_poll();
                SDL_Delay(5);
            }
        }

        int player_sum = 0;
        for (int p = 0; p < PLAYER_NUM; p++) {
            if (!mPr_NullCheckPersonalID(&Save_Get(private_data[p]).player_ID) &&
                Save_Get(private_data[p]).exists == TRUE) {
                player_sum++;
            }
        }

        if ((client_network_is_connected() && client_network_is_new_player()) || player_sum == 0) {
            s32 result;
            Door_data_c door_data;

            g_pc_quick_start = 0;
            mTD_rtc_set();
            extern void pc_sync_server_town_if_needed(void);
            pc_sync_server_town_if_needed();

            /* Clear any foreign resident data in the local save so this new player starts fresh */
            for (int p = 0; p < PLAYER_NUM; p++) {
                mPr_ClearPrivateInfo(Save_GetPointer(private_data[p]));
            }
            extern int pc_save_force_write(void);
            pc_save_force_write();

            mCD_InitGameStart_bg(0, -1, mCD_START_COND_2, &result);

            bzero(&door_data, sizeof(Door_data_c));
            door_data.next_scene_id = SCENE_START_DEMO2;
            door_data.exit_orientation = mSc_DIRECT_NORTH;
            door_data.exit_type = 0;
            door_data.extra_data = 0;
            door_data.exit_position.x = 120;
            door_data.exit_position.y = 0;
            door_data.exit_position.z = 340;
            door_data.door_actor_name = EMPTY_NO;
            door_data.wipe_type = WIPE_TYPE_FADE_BLACK;

            Common_Set(door_data, door_data);
            Common_Get(door_data).next_scene_id = SCENE_START_DEMO2;
            play->next_scene_no = SCENE_START_DEMO2;
            Common_Set(submenu_disabled, TRUE);
            Common_Get(transition).wipe_type = WIPE_TYPE_FADE_BLACK;
            mBGMPsComp_make_ps_wipe(0x11C2);
            return;
        }

        int p_idx = -1;
        for (int p = 0; p < PLAYER_NUM; p++) {
            if (!mPr_NullCheckPersonalID(&Save_Get(private_data[p]).player_ID) &&
                Save_Get(private_data[p]).exists == TRUE) {
                p_idx = p;
                break;
            }
        }
        if (p_idx < 0) {
            p_idx = 0;
        }
        Common_Set(player_no, p_idx);
        Common_Set(now_private, Save_GetPointer(private_data[p_idx]));

        int player_exists = (mPr_NullCheckPlayerName(Save_Get(private_data[p_idx]).player_ID.player_name) == FALSE) &&
                            (Save_Get(private_data[p_idx]).exists == TRUE);
        if ((g_pc_quick_start || client_network_is_connected()) && player_exists && decide_next_scene_no() == SCENE_PLAYERSELECT_2) {
            s32 result;
            int arrange_idx;
            static s16 homeX[] = { 2128, 2352, 2128, 2352 };
            static s16 homeZ[] = { 1488, 1488, 1768, 1768 };
            static u8 drt[] = { mSc_DIRECT_SOUTH_EAST, mSc_DIRECT_SOUTH_WEST, mSc_DIRECT_SOUTH_EAST, mSc_DIRECT_SOUTH_WEST };
            Door_data_c door_data;
            int has_map;

            g_pc_quick_start = 0;
            mTD_rtc_set();
            mCD_InitGameStart_bg(p_idx, -1, mCD_START_COND_1, &result);

            if (lbRTC_IsOverTime(&Save_Get(save_check).time, Common_GetPointer(time.rtc_time)) == lbRTC_LESS) {
                Save_Set(cheated_flag, TRUE);
                Save_Set(npc_force_go_home, TRUE);
            }

#ifdef TARGET_PC
            int has_home = FALSE;
            int owned_house_idx = -1;
            if (Now_Private != NULL && !mPr_NullCheckPersonalID(&Now_Private->player_ID)) {
                for (int h = 0; h < mHS_HOUSE_NUM; h++) {
                    if (!mPr_NullCheckPersonalID(&Save_Get(homes[h]).ownerID) &&
                        mPr_CheckCmpPersonalID(&Save_Get(homes[h]).ownerID, &Now_Private->player_ID) == TRUE) {
                        has_home = TRUE;
                        owned_house_idx = h;
                        break;
                    }
                }
            }
            if (client_network_is_connected() && client_network_is_resident()) {
                int net_h = client_network_get_house_idx();
                if (net_h >= 0 && net_h < mHS_HOUSE_NUM) {
                    has_home = TRUE;
                    owned_house_idx = net_h;
                }
            }

            bzero(&door_data, sizeof(Door_data_c));
            door_data.next_scene_id = SCENE_FG;
            if (!has_home) {
                door_data.exit_orientation = mSc_DIRECT_SOUTH;
                door_data.exit_type = 0;
                door_data.extra_data = 0;
                door_data.exit_position.x = 2200;
                door_data.exit_position.y = 0;
                door_data.exit_position.z = 860;
                door_data.door_actor_name = EMPTY_NO;
                door_data.wipe_type = WIPE_TYPE_CIRCLE_RIGHT;
            } else {
                arrange_idx = owned_house_idx;
                door_data.exit_orientation = drt[arrange_idx];
                door_data.exit_type = 1;
                door_data.extra_data = 1;
                door_data.exit_position.x = homeX[arrange_idx];
                door_data.exit_position.y = 0;
                door_data.exit_position.z = homeZ[arrange_idx];
                door_data.door_actor_name = HOUSE0 + arrange_idx;
                door_data.wipe_type = WIPE_TYPE_TRIFORCE;
            }
#else
            arrange_idx = mHS_get_arrange_idx(0);
            bzero(&door_data, sizeof(Door_data_c));
            door_data.next_scene_id = SCENE_FG;
            door_data.exit_orientation = drt[arrange_idx];
            door_data.exit_type = 1;
            door_data.extra_data = 1;
            door_data.exit_position.x = homeX[arrange_idx];
            door_data.exit_position.y = 0;
            door_data.exit_position.z = homeZ[arrange_idx];
            door_data.door_actor_name = HOUSE0 + arrange_idx;
            door_data.wipe_type = WIPE_TYPE_TRIFORCE;
#endif

            Common_Set(door_data, door_data);
            Common_Get(door_data).next_scene_id = SCENE_FG + 1;
            play->next_scene_no = SCENE_FG;

            if (Now_Private != NULL) {
                Now_Private->birthday_present_npc = EMPTY_NO;
            }

            has_map = TRUE;
            if (mEv_CheckFirstJob() == TRUE) {
                mQst_errand_c* fj_p = mQst_GetFirstJobData();
                if (fj_p != NULL) {
                    switch (fj_p->base.quest_kind) {
                        case mQst_ERRAND_FIRSTJOB_CHANGE_CLOTH:
                        case mQst_ERRAND_FIRSTJOB_PLANT_FLOWER:
                        case mQst_ERRAND_FIRSTJOB_DELIVER_FTR:
                        case mQst_ERRAND_FIRSTJOB_INTRODUCTIONS:
                        case mQst_ERRAND_FIRSTJOB_START:
                            has_map = FALSE;
                            break;
                    }
                }
            }
            Common_Set(map_flag, has_map);
            Common_Set(submenu_disabled, FALSE);
            Common_Get(transition).wipe_type = WIPE_TYPE_TRIFORCE;
            mBGMPsComp_make_ps_wipe(0x11C2);
            return;
        } else {
            g_pc_quick_start = 0;
            play->next_scene_no = decide_next_scene_no();
            Common_Set(door_data.next_scene_id, 0);
            Common_Set(submenu_disabled, TRUE);
        }
    }
#else
    play->next_scene_no = decide_next_scene_no();
    Common_Set(door_data.next_scene_id, 0);
    Common_Set(submenu_disabled, TRUE);
#endif

    {
        int rtc_enabled = Common_Get(time.rtc_enabled);
        Common_Set(time.rtc_enabled, TRUE);
        mTM_rtcTime_limit_check();
        Common_Set(time.rtc_enabled, rtc_enabled);
    }

    mEv_ClearEventInfo();
    Common_Set(scene_from_title_demo, -1);
}
