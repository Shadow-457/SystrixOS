#pragma once
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 0x100

static inline void *dlopen(const char *path, int flags)  { (void)path; (void)flags; return (void*)1; }
static inline void *dlsym(void *handle, const char *sym)  { (void)handle; (void)sym; return 0; }
static inline int   dlclose(void *handle)                 { (void)handle; return 0; }
static inline char *dlerror(void)                         { return "dlopen not supported"; }
