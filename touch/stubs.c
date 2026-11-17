/*
  Заглушки вместо bufferoverflowfastfailk.lib: она тянет _load_config_used (CFG-метаданные MSVC),
  который lld-link собирает иначе, чем link.exe. Объекты WDF-стаба собраны с /guard:cf и /GS,
  поэтому им нужны __guard_check_icall_fptr и __security_init_cookie.
*/

/* проверка CFG не нужна: цель непрямого вызова в x15, просто возврат */
__attribute__((naked)) static void GuardCheckNop(void)
{
    __asm__ volatile("ret");
}

void *__guard_check_icall_fptr = (void *)GuardCheckNop;

unsigned long long __security_cookie = 0x2B992DDFA232ULL;

void __security_init_cookie(void)
{
}
