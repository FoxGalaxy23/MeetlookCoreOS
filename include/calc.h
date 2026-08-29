#ifndef CALC_H
#define CALC_H

/* Точка входа приложения "калькулятор".
   Работает в том же кольце (ring 0), что и ядро — как и все приложения ОС. */
void app_calc_run(const char *expr_or_null);

#endif
