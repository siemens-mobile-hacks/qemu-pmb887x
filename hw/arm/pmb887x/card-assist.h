/*
 * PMB887X-WEB FEATURE (card assist): hw/arm/pmb887x/card-assist.c
 */
#ifndef HW_ARM_PMB887X_CARD_ASSIST_H
#define HW_ARM_PMB887X_CARD_ASSIST_H

/* write commands the card answered (CMD24/25/38) */
extern uint32_t pmb887x_card_writes;
/* 0 in, 1 out, 2 going out, 3 reading, 4 not seen; | 0x10 waiting for the firmware's timer */
extern uint32_t pmb887x_card_state;
/* clean unmount: 0 none, 1 pending, 2 sent, 3 not possible */
extern uint32_t pmb887x_card_unmount_state;

/*
 * Under the BQL. Through the card assist when there is one (the firmware is
 * told), else the sd-slot's "inserted" directly.
 */
bool pmb887x_card_set_inserted(bool inserted, Error **errp);
void pmb887x_card_unmount(void);

#endif
