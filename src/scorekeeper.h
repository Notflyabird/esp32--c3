#pragma once

#include <stdbool.h>

bool scorekeeper_register_commands(void);
void scorekeeper_apply_command(int command);
void scorekeeper_print_scores(const char *title);

