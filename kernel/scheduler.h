#pragma once

void         scheduler_init(void);
int          process_create(void (*entry)(void));
unsigned int scheduler_tick(unsigned int current_esp);
