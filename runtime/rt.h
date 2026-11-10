// Host runtime API shared by shims.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// ------------------------------------------------------------- shim helpers (stdcall)
#define ARG(n)      g_rd32(cpu->esp + 4 + 4 * (n))
#define ARGP(n)     ((char *)gptr(ARG(n)))
#define RET(v, nargs) do { cpu->eax = (uint32_t)(v); cpu->esp += 4 + 4 * (nargs); return; } while (0)
#define RET_CDECL(v)  do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return; } while (0)
#define SHIM(dll, name) void imp_##dll##_##name(Cpu *cpu)

extern int g_trace_imports;
#define TRACE(...) do { if (g_trace_imports) { fprintf(stderr, "[api] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define LOG(...)   do { fprintf(stderr, "[rt] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// ------------------------------------------------------------- guest memory
#define GUEST_STACK_SIZE   (2u << 20)
uint32_t galloc(uint32_t size);                 // guest heap (zeroed)
void     gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gvalloc(uint32_t size);                // page-aligned, never freed (VirtualAlloc)
uint32_t gstrdup(const char *s);

// ------------------------------------------------------------- host-callable thunks
// Register a host function so that guest code can call it through a guest address.
uint32_t rt_thunk(GuestFn fn, const char *name);
// Call a guest function from the host (stdcall or cdecl): pushes args right-to-left.
uint32_t call_guest(Cpu *cpu, uint32_t fn, int cdecl_, int nargs, ...);

// ------------------------------------------------------------- handles
typedef enum { H_FREE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_THREAD, H_HEAP, H_MODULE, H_OTHER } HKind;
typedef struct RtHandle { HKind kind; void *p; int fd; uint32_t extra; } RtHandle;
uint32_t h_new(HKind kind, void *p);
RtHandle  *h_get(uint32_t h);
void     h_free(uint32_t h);

// ------------------------------------------------------------- modules
#ifdef COD_MP
#define RT_EXE_NAME "CoDMP.exe"
#else
#define RT_EXE_NAME "CoDSP.exe"
#endif
#define RT_VIRTUAL_INSTALL_DIR "CoD"         // guest sees the game at C:\\CoD
const char *rt_bin_dir(void);
uint32_t rt_load_module(Cpu *cpu, const char *name);
int rt_free_module(Cpu *cpu, uint32_t h);
uint32_t rt_module_export(uint32_t h, const char *name);
const char *rt_module_name_of(uint32_t h);
uint32_t rt_import_thunk(const char *dll, const char *name);

// ------------------------------------------------------------- misc
Cpu *rt_cpu(void);                               // current thread's Cpu
void rt_set_last_error(uint32_t e);
uint32_t rt_last_error(void);
const char *rt_game_dir(void);
char *rt_map_path(const char *winpath, char *out, size_t outsz);   // C:\..\X -> host path (case-insensitive)
void rt_unimplemented(Cpu *cpu, const char *name) __attribute__((noreturn));
uint32_t rt_new_thread_cpu(Cpu *c);              // allocate stack + TIB; returns stack top
extern uint32_t g_hinstance;

// ------------------------------------------------------------- COM objects in guest memory
// layout: [0] vtbl, [4] refcount, [8] host ptr lo, [12] host ptr hi, [16..] free for interface-specific data
typedef struct ComMethod { const char *name; GuestFn fn; } ComMethod;
uint32_t com_new(const char *iface, const ComMethod *methods, int n, void *host);
void    *com_host(uint32_t obj);
extern __thread const char *t_thunk_name;
void com_unimpl(Cpu *cpu);
#define THIS        ARG(0)
#define HOST(T)     ((T *)com_host(ARG(0)))
#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u
