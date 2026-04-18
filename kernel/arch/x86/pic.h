#pragma once

void pic_init   (void);
void pic_eoi    (unsigned int irq);
void pic_unmask (unsigned int irq);  /* habilita un IRQ en el PIC */
void pic_mask   (unsigned int irq);  /* deshabilita un IRQ */
