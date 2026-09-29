#ifndef FEB_BOARD_HOOKS_H
#define FEB_BOARD_HOOKS_H

/* C6 boot-button/factory-reset glue (board_hooks.c). Both run on the NimBLE host task,
   from host_synced(), in the order host_synced() always used. */
void feb_board_button_events_init(void);
void feb_board_control_ready_set(void);

#endif
