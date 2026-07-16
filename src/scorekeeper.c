#include "scorekeeper.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mn_speech_commands.h"

static const char *TAG = "DDZ_SCORE";

#define CMD_QUERY_SCORE 1
#define CMD_RESET_SCORE 2
#define CMD_SCORE_BASE 100

typedef struct {
    const char *spoken;
    int points;
} point_phrase_t;

static int s_score[3] = {0, 0, 0};

static const char *const PLAYER_PHRASES[3] = {
    "yi hao",
    "er hao",
    "san hao",
};

static const point_phrase_t POINT_PHRASES[] = {
    {"liang fen", 2},
    {"si fen", 4},
    {"liu fen", 6},
    {"ba fen", 8},
    {"yi shi fen", 10},
    {"yi shi er fen", 12},
    {"yi shi si fen", 14},
    {"yi shi liu fen", 16},
    {"yi shi ba fen", 18},
    {"er shi fen", 20},
};

static int score_total(void)
{
    return s_score[0] + s_score[1] + s_score[2];
}

void scorekeeper_print_scores(const char *title)
{
    ESP_LOGI(TAG, "%s: P1=%d, P2=%d, P3=%d, total=%d",
             title, s_score[0], s_score[1], s_score[2], score_total());
}

static void reset_scores(void)
{
    s_score[0] = 0;
    s_score[1] = 0;
    s_score[2] = 0;
    ESP_LOGI(TAG, "All scores reset");
    scorekeeper_print_scores("After reset");
}

static int make_score_command_id(int player, bool landlord_win, int points)
{
    const int player_index = player - 1;
    const int outcome_index = landlord_win ? 0 : 1;
    const int point_index = points / 2 - 1;
    return CMD_SCORE_BASE + player_index * 20 + outcome_index * 10 + point_index;
}

static bool parse_score_command_id(int command, int *player, bool *landlord_win, int *points)
{
    int value = command - CMD_SCORE_BASE;
    if (value < 0 || value >= 60) {
        return false;
    }

    const int player_index = value / 20;
    value %= 20;
    const int outcome_index = value / 10;
    const int point_index = value % 10;

    *player = player_index + 1;
    *landlord_win = (outcome_index == 0);
    *points = (point_index + 1) * 2;
    return true;
}

static void settle_round(int landlord, bool landlord_win, int points)
{
    const int before[3] = {s_score[0], s_score[1], s_score[2]};
    const int landlord_delta = landlord_win ? points : -points;
    const int farmer_delta = landlord_win ? -(points / 2) : (points / 2);
    const int landlord_index = landlord - 1;

    ESP_LOGI(TAG, "Command: P%d landlord %s %d points",
             landlord, landlord_win ? "wins" : "loses", points);
    ESP_LOGI(TAG, "Before: P1=%d, P2=%d, P3=%d, total=%d",
             before[0], before[1], before[2], before[0] + before[1] + before[2]);

    for (int i = 0; i < 3; ++i) {
        s_score[i] += (i == landlord_index) ? landlord_delta : farmer_delta;
    }

    ESP_LOGI(TAG, "Delta: P1=%+d, P2=%+d, P3=%+d",
             s_score[0] - before[0], s_score[1] - before[1], s_score[2] - before[2]);
    scorekeeper_print_scores("After");

    if (score_total() != 0) {
        ESP_LOGE(TAG, "Total check failed");
    }
}

void scorekeeper_apply_command(int command)
{
    int player = 0;
    int points = 0;
    bool landlord_win = false;

    if (parse_score_command_id(command, &player, &landlord_win, &points)) {
        settle_round(player, landlord_win, points);
        return;
    }

    switch (command) {
    case CMD_QUERY_SCORE:
        scorekeeper_print_scores("Query");
        break;
    case CMD_RESET_SCORE:
        reset_scores();
        break;
    default:
        ESP_LOGW(TAG, "Unknown command id: %d", command);
        break;
    }
}

static bool add_command_checked(esp_err_t err, int command_id, const char *phrase)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Add command %d: %s", command_id, phrase);
        return true;
    }
    ESP_LOGE(TAG, "Failed to add command [%s]: %s", phrase, esp_err_to_name(err));
    return false;
}

static bool add_score_command(int player, bool landlord_win, int points, const char *point_phrase)
{
    char command_phrase[64];
    const int command_id = make_score_command_id(player, landlord_win, points);

    snprintf(command_phrase, sizeof(command_phrase), "%s di zhu %s %s",
             PLAYER_PHRASES[player - 1], landlord_win ? "ying" : "shu", point_phrase);

    return add_command_checked(esp_mn_commands_add(command_id, command_phrase),
                               command_id, command_phrase);
}

bool scorekeeper_register_commands(void)
{
    bool ok = true;

    for (int player = 1; player <= 3 && ok; ++player) {
        for (size_t i = 0; i < sizeof(POINT_PHRASES) / sizeof(POINT_PHRASES[0]) && ok; ++i) {
            ok = add_score_command(player, true, POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            if (ok) {
                ok = add_score_command(player, false, POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            }
        }
    }

    return ok &&
           add_command_checked(esp_mn_commands_add(CMD_QUERY_SCORE, "cha xun fen shu"),
                               CMD_QUERY_SCORE, "cha xun fen shu") &&
           add_command_checked(esp_mn_commands_add(CMD_RESET_SCORE, "chong zhi suo you fen shu"),
                               CMD_RESET_SCORE, "chong zhi suo you fen shu");
}

