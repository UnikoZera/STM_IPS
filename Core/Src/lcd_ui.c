/*
 * lcd_ui.c
 *
 *  Created on: 2026年4月2日
 *      Author: UnikoZera
 *
 *  组织方式：按 region 划分「主题 / 组件 / 页面 / 状态机」。
 *
 *  架构约定：
 *   - 页面(screen)：一个页面 = 一组图层 + 内容更新逻辑，通过 enter/update/exit 三
 *     个回调接入状态机。新增页面时在 s_screens 表注册即可。
 *   - 组件(component)：跨页面复用的 UI 单元（如状态栏），提供 init/update/mount
 *     三个动作，由状态机在切换页面时挂载(mount)。
 *   - 主题(theme)：颜色 / 字号 / 间距集中定义，页面与组件一律引用这些 token，
 *     不写死具体色值，保证整体视觉一致。
 */

#include "lcd_ui.h"
#include "lcd_mjpeg.h"
#include "storage_manager.h"
#include <stdio.h>
#include <string.h>

#pragma region 主题（UI 设计系统）

/* ────── 背景 ────── */
#define UI_BG          0x0841U  /* 深蓝黑主背景（避免纯黑的死板） */
#define UI_BG_BAR      0x1082U  /* 状态栏底色（略亮于主背景，建立层次） */

/* ────── 文字 ────── */
#define UI_FG          0xFFFFU  /* 主文字：白 */
#define UI_FG_MUTED    0x632CU  /* 弱化文字：中灰（诊断信息） */

/* ────── 强调 / 分隔 ────── */
#define UI_ACCENT      0x06BFU  /* 品牌强调色：青绿 */
#define UI_DIVIDER     0x3186U  /* 分隔线：暗灰 */

/* ────── 布局 ────── */
#define UI_PAD            4      /* 基础留白 */
#define UI_STATUSBAR_H    10     /* 状态栏高度（6x8 字体 8px + 分隔线 1px + 上下 1px） */
#define UI_BAR_BG_Y       0      /* 状态栏背景条基准 y */
#define UI_BAR_TXT_Y      1      /* 状态栏文字基准 y */
#define UI_BAR_DIV_Y      (UI_STATUSBAR_H - 1)  /* 状态栏分隔线基准 y */
#define UI_STATUS_INFO_W  66     /* 状态栏右侧信息区预留宽度（"999 FPS 99%" = 11 字符） */

/* ────── 资源文件名绑定（须与上位机写入的 FAT 文件名一致） ────── */
#define UI_VIDEO_NAME    ""   /* 主界面播放的视频 */
#define UI_PICTURE_NAME  "photo_t"  /* 无视频时的兜底图片 */

#pragma endregion

/* 前向声明：由下方「页面状态机」region 提供（状态栏组件在挂载时依赖） */
static lcd_screen_id_t s_current_screen;
static const char *ui_screen_title(lcd_screen_id_t id);

#pragma region 组件：状态栏

typedef struct
{
    lcd_rect_t  bg;        /* 背景条 */
    lcd_rect_t  divider;   /* 底部分隔线 */
    lcd_label_t title;     /* 左侧页面标题 */
    lcd_label_t info;      /* 右侧诊断信息（FPS/CPU，弱化显示） */
    char title_buf[16];
    char info_buf[24];
} ui_statusbar_t;

static ui_statusbar_t s_statusbar;

/* 启动一个单次、非往返的 i16 属性动画（用于页面切换过渡） */
static void ui_anim_in_i16(void *target, int32_t from, int32_t to)
{
    lcd_anim_config_t cfg = {
        .target = target,
        .start_value = from,
        .end_value = to,
        .duration_ms = 240,
        .delay_ms = 0,
        .repeat = false,
        .yoyo = false,
        .exec_cb = lcd_anim_exec_set_i16,
        .done_cb = NULL,
        .path_cb = lcd_anim_get_path(LCD_ANIM_EASE_OUT_QUAD),
    };
    lcd_anim_start(&cfg);
}

static void ui_statusbar_init(ui_statusbar_t *sb)
{
    memset(sb, 0, sizeof(*sb));

    sb->bg.x = 0;
    sb->bg.y = UI_BAR_BG_Y;
    sb->bg.w = LCD_W;
    sb->bg.h = UI_STATUSBAR_H;
    sb->bg.color = UI_BG_BAR;

    sb->divider.x = 0;
    sb->divider.y = UI_BAR_DIV_Y;
    sb->divider.w = LCD_W;
    sb->divider.h = 1;
    sb->divider.color = UI_DIVIDER;

    sb->title.x = UI_PAD;
    sb->title.y = UI_BAR_TXT_Y;
    sb->title.fg_color = UI_FG;
    sb->title.bg_color = UI_BG_BAR;
    sb->title.size = 8;
    sb->title.text = sb->title_buf;

    sb->info.x = (int16_t)(LCD_W - UI_STATUS_INFO_W - UI_PAD);
    sb->info.y = UI_BAR_TXT_Y;
    sb->info.fg_color = UI_FG_MUTED;
    sb->info.bg_color = UI_BG_BAR;
    sb->info.size = 8;
    sb->info.text = sb->info_buf;
}

/* 状态栏整体从屏幕上方滑入（页面切换过渡动画，绝对基准坐标、幂等） */
static void ui_statusbar_slide_in(ui_statusbar_t *sb)
{
    sb->bg.y      = (int16_t)(UI_BAR_BG_Y  - UI_STATUSBAR_H);
    sb->divider.y = (int16_t)(UI_BAR_DIV_Y - UI_STATUSBAR_H);
    sb->title.y   = (int16_t)(UI_BAR_TXT_Y - UI_STATUSBAR_H);
    sb->info.y    = (int16_t)(UI_BAR_TXT_Y - UI_STATUSBAR_H);

    ui_anim_in_i16(&sb->bg.y,      (int32_t)sb->bg.y,      UI_BAR_BG_Y);
    ui_anim_in_i16(&sb->divider.y, (int32_t)sb->divider.y, UI_BAR_DIV_Y);
    ui_anim_in_i16(&sb->title.y,   (int32_t)sb->title.y,   UI_BAR_TXT_Y);
    ui_anim_in_i16(&sb->info.y,    (int32_t)sb->info.y,    UI_BAR_TXT_Y);
}

/* 挂载状态栏：先设置标题，再按「先底层后顶层」的顺序加入图层，最后做入场动画 */
static void ui_statusbar_mount(ui_statusbar_t *sb)
{
    snprintf(sb->title_buf, sizeof(sb->title_buf), "%s", ui_screen_title(s_current_screen));

    lcd_anim_manager_add_layer(&sb->bg, lcd_draw_rect_layer);
    lcd_anim_manager_add_layer(&sb->divider, lcd_draw_rect_layer);
    lcd_anim_manager_add_layer(&sb->title, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&sb->info, lcd_draw_label_layer);

    ui_statusbar_slide_in(sb);
}

/* 状态栏内容更新（跨页面，主循环每帧调用；FPS/CPU 为低频信息，1s 节流） */
static void ui_statusbar_update(ui_statusbar_t *sb)
{
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_tick) < 1000U)
    {
        return;
    }
    last_tick = now;

    snprintf(sb->info_buf, sizeof(sb->info_buf), "%u FPS %u%%",
             (unsigned)lcd_fps, (unsigned)cpu_usage_percent);
}

#pragma endregion

#pragma region 页面：HOME（视频 / 图片内容展示）

static lcd_video_t g_home_video = {0};
static lcd_picture_t g_home_picture = {0};

static lcd_label_t g_home_empty = {
    56, LCD_H / 2 - 4, UI_FG_MUTED, UI_BG, 8, "NO MEDIA"
};

/* 从 FAT 绑定视频 / 图片资源地址（文件名与上位机一致） */
static void ui_home_bind_media(void)
{
    g_home_video = (lcd_video_t){0};
    g_home_picture = (lcd_picture_t){0};

    int16_t idx = find_large_file_by_name(UI_VIDEO_NAME);
    if (idx >= 0)
    {
        large_file_info_t info;
        if (get_large_file_info((uint8_t)idx, &info))
        {
            g_home_video.x = 0;
            g_home_video.y = 0;
            g_home_video.start_addr = info.start_sector * 4096;
            g_home_video.end_addr = g_home_video.start_addr + info.size;
        }
    }

    idx = find_large_file_by_name(UI_PICTURE_NAME);
    if (idx >= 0)
    {
        large_file_info_t info;
        if (get_large_file_info((uint8_t)idx, &info))
        {
            g_home_picture.x = 0;
            g_home_picture.y = 0;
            g_home_picture.addr = info.start_sector * 4096;
        }
    }
}

static void ui_screen_home_enter(void)
{
    ui_home_bind_media();

    /* 内容层：视频优先，无视频则显示图片，都没有则显示占位提示 */
    if (g_home_video.start_addr != 0UL && g_home_video.end_addr > g_home_video.start_addr)
    {
        lcd_anim_manager_add_layer(&g_home_video, lcd_draw_video_frame_layer);
    }
    else if (g_home_picture.addr != 0UL)
    {
        lcd_anim_manager_add_layer(&g_home_picture, lcd_draw_picture_layer);
    }
    else
    {
        lcd_anim_manager_add_layer(&g_home_empty, lcd_draw_label_layer);
    }
}

#pragma endregion

#pragma region 页面：SETTINGS（系统诊断）

typedef struct
{
    lcd_label_t fps;
    lcd_label_t cpu;
    lcd_label_t video;
    lcd_label_t storage;
    lcd_label_t hint;
    char fps_buf[24];
    char cpu_buf[24];
    char video_buf[40];
    char storage_buf[24];
    char hint_buf[40];
} ui_settings_view_t;

static ui_settings_view_t s_settings;

static void ui_settings_init(ui_settings_view_t *v)
{
    memset(v, 0, sizeof(*v));

    const int16_t x = UI_PAD;
    const int16_t first_y = UI_STATUSBAR_H + UI_PAD;
    const int16_t row_h = 10;   /* 6x8 字体 8px + 2px 行距 */

    lcd_label_t labels[] = {
        { x, first_y,                UI_FG,       UI_BG, 8, v->fps_buf },
        { x, first_y + row_h,        UI_FG,       UI_BG, 8, v->cpu_buf },
        { x, first_y + row_h * 2,    UI_ACCENT,   UI_BG, 8, v->video_buf },
        { x, first_y + row_h * 3,    UI_FG,       UI_BG, 8, v->storage_buf },
        { x, first_y + row_h * 4,    UI_FG_MUTED, UI_BG, 8, v->hint_buf },
    };
    v->fps = labels[0];
    v->cpu = labels[1];
    v->video = labels[2];
    v->storage = labels[3];
    v->hint = labels[4];
}

static void ui_screen_settings_enter(void)
{
    lcd_anim_manager_add_layer(&s_settings.fps, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&s_settings.cpu, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&s_settings.video, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&s_settings.storage, lcd_draw_label_layer);
    lcd_anim_manager_add_layer(&s_settings.hint, lcd_draw_label_layer);
}

static void ui_screen_settings_update(void)
{
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_tick) < 500U)
    {
        return;
    }
    last_tick = now;

    snprintf(s_settings.fps_buf, sizeof(s_settings.fps_buf), "FPS: %u",
             (unsigned)lcd_fps);
    snprintf(s_settings.cpu_buf, sizeof(s_settings.cpu_buf), "CPU: %u%%",
             (unsigned)cpu_usage_percent);

    const mjpeg_state_t *st = lcd_mjpeg_get_state();
    if (st != NULL && st->active != 0U)
    {
        snprintf(s_settings.video_buf, sizeof(s_settings.video_buf),
                 "VID %dx%d f%u/%u e%d",
                 (int)st->width, (int)st->height,
                 (unsigned)st->cur_frame_idx, (unsigned)st->frame_count,
                 (int)st->last_error);
    }
    else
    {
        snprintf(s_settings.video_buf, sizeof(s_settings.video_buf), "VID: none");
    }

    snprintf(s_settings.storage_buf, sizeof(s_settings.storage_buf), "STORAGE: %s",
             storage_is_downloading() ? "DL" : "idle");

    snprintf(s_settings.hint_buf, sizeof(s_settings.hint_buf),
             "screen: %d", (int)s_current_screen);
}

#pragma endregion

#pragma region 页面状态机

static lcd_screen_id_t s_current_screen = LCD_SCREEN_COUNT;

typedef struct
{
    lcd_screen_id_t id;
    const char *title;
    void (*enter)(void);
    void (*update)(void);
    void (*exit)(void);
} ui_screen_def_t;

static void ui_screen_home_enter(void);
static void ui_screen_settings_enter(void);
static void ui_screen_settings_update(void);

static const ui_screen_def_t s_screens[LCD_SCREEN_COUNT] = {
    [LCD_SCREEN_HOME] = {
        .id = LCD_SCREEN_HOME,
        .title = "HOME",
        .enter = ui_screen_home_enter,
        .update = NULL,
        .exit = NULL,
    },
    [LCD_SCREEN_SETTINGS] = {
        .id = LCD_SCREEN_SETTINGS,
        .title = "SETTINGS",
        .enter = ui_screen_settings_enter,
        .update = ui_screen_settings_update,
        .exit = NULL,
    },
};

static const char *ui_screen_title(lcd_screen_id_t id)
{
    return (id < LCD_SCREEN_COUNT) ? s_screens[id].title : "?";
}

void lcd_ui_switch_screen(lcd_screen_id_t screen)
{
    if (screen >= LCD_SCREEN_COUNT || screen == s_current_screen)
    {
        return;
    }

    if (s_current_screen < LCD_SCREEN_COUNT && s_screens[s_current_screen].exit != NULL)
    {
        s_screens[s_current_screen].exit();
    }

    lcd_anim_stop_all();
    lcd_anim_manager_clear_layers();

    s_current_screen = screen;

    /* 先挂页面内容（底层），再挂状态栏（顶层悬浮） */
    if (s_screens[s_current_screen].enter != NULL)
    {
        s_screens[s_current_screen].enter();
    }
    ui_statusbar_mount(&s_statusbar);

    lcd_anim_manager_render();
}

lcd_screen_id_t lcd_ui_current_screen(void)
{
    return s_current_screen;
}

#pragma endregion

#pragma region 输入：编码器（TIM2 正交编码，翻页）

#define UI_ENC_STEP      2     /* 每 2 个计数翻一页（一个机械格） */
#define UI_ENC_POLL_MS   10    /* 轮询间隔（降频去抖） */

static uint32_t s_enc_last = 0;
static int32_t s_enc_accum = 0;

/* 方向翻页：dir>0 下一页，dir<0 上一页（首尾循环） */
static void ui_encoder_page(int8_t dir)
{
    lcd_screen_id_t next;
    if (dir > 0)
    {
        next = (lcd_screen_id_t)((s_current_screen + 1) % LCD_SCREEN_COUNT);
    }
    else
    {
        next = (lcd_screen_id_t)((s_current_screen + LCD_SCREEN_COUNT - 1) % LCD_SCREEN_COUNT);
    }
    lcd_ui_switch_screen(next);
}

/*
 * 轮询 TIM2 编码器计数并累积增量，每累计 UI_ENC_STEP 个计数翻一页。
 * 计数器为 32 位满量程（Period=0xFFFFFFFF），用 uint32_t 差值绕回求增量。
 */
static void ui_encoder_poll(void)
{
    static uint32_t last_tick = 0;
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_tick) < UI_ENC_POLL_MS)
    {
        return;
    }
    last_tick = now;

    uint32_t cur = __HAL_TIM_GET_COUNTER(&htim2);
    int32_t delta = (int32_t)(cur - s_enc_last);
    s_enc_last = cur;
    s_enc_accum += delta;

    while (s_enc_accum >= UI_ENC_STEP)
    {
        s_enc_accum -= UI_ENC_STEP;
        ui_encoder_page(1);
    }
    while (s_enc_accum <= -UI_ENC_STEP)
    {
        s_enc_accum += UI_ENC_STEP;
        ui_encoder_page(-1);
    }
}

#pragma endregion

#pragma region 对外 API

void lcd_ui_init(void)
{
    lcd_anim_manager_init();
    lcd_anim_manager_set_bg(UI_BG);

    /* 启动 TIM2 正交编码器（翻页输入）。MX_TIM2_Init 已配置但未启动计数 */
    HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);

    ui_statusbar_init(&s_statusbar);
    ui_settings_init(&s_settings);

    /* s_current_screen 初始为 LCD_SCREEN_COUNT（哨兵），强制首次走完整进入流程 */
    lcd_ui_switch_screen(LCD_SCREEN_HOME);
}

/**
 * @brief 更新当前页面状态（由 lcd_ui_updater 调用，勿在中断中调用）
 */
void lcd_ui_change(void)
{
    if (s_current_screen < LCD_SCREEN_COUNT && s_screens[s_current_screen].update != NULL)
    {
        s_screens[s_current_screen].update();
    }
}

/**
 * @brief 主循环 UI 更新入口：先统计、再更新组件与页面、最后推进动画并渲染。
 * @attention 先 task 再 render，确保动画状态更新后再绘制。
 */
void lcd_ui_updater(void)
{
    lcd_calculate_usage();
    lcd_calculate_fps();

    ui_encoder_poll();                  /* 编码器翻页输入 */
    ui_statusbar_update(&s_statusbar);   /* 跨页面组件 */
    lcd_ui_change();                    /* 当前页面 */

    lcd_anim_manager_task();
    lcd_anim_manager_render();
}

#pragma endregion
