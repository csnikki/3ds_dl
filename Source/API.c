#include "Handle.h"
#include "Error.h"
#include "Loader.h"
#include "Symbol.h"

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define RTLD_LAZY 0x0001
#define RTLD_DEEPBIND 0x0008
#define RTLD_NODELETE 0x1000

static bool ctrdl_checkFlags(int flags) {
    // Unsupported flags.
    if (flags & (RTLD_LAZY | RTLD_DEEPBIND | RTLD_NODELETE))
        return false;

    // Required flags.
    if (!(flags & RTLD_NOW))
        return false;

    return true;
}

void* dlopen(const char* path, int flags) { return ctrdlOpen(path, flags, NULL, NULL); }
const char* dlerror(void) { return ctrdl_getErrorAsString(ctrdl_getLastError()); }

int dlclose(void* handle) { return !ctrdl_unlockHandle((CTRDLHandle*)handle); }

void* dlsym(void* handle, const char* name) {
    if (!handle || !name) {
        ctrdl_setLastError(Err_InvalidParam);
        return NULL;
    }

    CTRDLHandle* h = (CTRDLHandle*)handle;
    const Elf32_Sym* sym = ctrdl_extendedFindSymbolFromName(h, name);
    if (sym)
        return (void*)(h->base + sym->st_value);

    ctrdl_setLastError(Err_NotFound);
    return NULL;
}

int dladdr(const void* address, Dl_info* info) {
    if (!info)
        return 0;

    const u32 addr = (u32)address;
    CTRDLHandle* h = ctrdlHandleByAddress(addr);
    if (h) {
        info->dli_fname = h->path;
        info->dli_fbase = (void*)h->base;

        const Elf32_Sym* sym = ctrdl_findSymbolFromValue(h, addr - h->base);
        if (sym) {
            info->dli_sname = &h->stringTable[sym->st_name];
            info->dli_saddr = (void*)(h->base + sym->st_value);
        } else {
            info->dli_sname = NULL;
            info->dli_saddr = NULL;
        }

        ctrdl_unlockHandle(h);
    }

    return info->dli_fbase != NULL;
}

void* ctrdlOpen(const char* path, int flags, CTRDLResolverFn resolver, void* resolverUserData) {
    // We don't support the NULL pseudo handle.
    if (!path || !ctrdl_checkFlags(flags)) {
        ctrdl_setLastError(Err_InvalidParam);
        return NULL;
    }

    // Avoid reading if already open.
    ctrdl_acquireHandleMtx();
    CTRDLHandle* handle = ctrdl_unsafeFindHandleByName(path);
    ctrdl_lockHandle(handle);
    ctrdl_releaseHandleMtx();

    if (handle) {
        // Update flags.
        handle->flags = flags;
        return (void*)handle;
    }

    if (flags & RTLD_NOLOAD) {
        ctrdl_setLastError(Err_NotFound);
        return NULL;
    }

    // Open file for reading.
    FILE* f = fopen(path, "rb");
    if (!f) {
        ctrdl_setLastError(Err_InvalidParam);
        return NULL;
    }

    CTRDLStream stream;
    ctrdl_makeFileStream(&stream, f);
    handle = ctrdl_loadObject(path, flags, &stream, resolver, resolverUserData);

    fclose(f);
    return handle;
}

void* ctrdlHandleByAddress(u32 addr) {
    ctrdl_acquireHandleMtx();
    CTRDLHandle* handle = ctrdl_unsafeFindHandleByAddr(addr);
    if (handle) {
        ctrdl_lockHandle(handle);
    } else {
        ctrdl_setLastError(Err_NotFound);
    }
    ctrdl_releaseHandleMtx();
    return handle;
}

void* ctrdlThisHandle(void) { return ctrdlHandleByAddress((u32)__builtin_extract_return_addr(__builtin_return_address(0))); }

void ctrdlEnumerate(CTRDLEnumerateFn callback) {
    if (!callback) {
        ctrdl_setLastError(Err_InvalidParam);
        return;
    }

    ctrdl_acquireHandleMtx();

    for (size_t i = 0; i < CTRDL_MAX_HANDLES; ++i) {
        CTRDLHandle* h = ctrdl_unsafeGetHandleByIndex(i);
        if (h->refc)
            callback(h);
    }

    ctrdl_releaseHandleMtx();
}

bool ctrdlInfo(void* handle, CTRDLInfo* info) {
    if (!handle || !info) {
        ctrdl_setLastError(Err_InvalidParam);
        return false;
    }

    CTRDLHandle* h = (CTRDLHandle*)handle;
    ctrdl_lockHandle(h);

    bool success = true;
    if (h->path) {
        info->pathSize = strlen(h->path);;
        info->path = malloc(info->pathSize + 1);
        if (info->path) {
            memcpy(info->path, h->path, info->pathSize);
            info->path[info->pathSize] = '\0';
        } else {
            ctrdl_setLastError(Err_NoMemory);
            success = false;
        }
    } else {
        info->path = NULL;
        info->pathSize = 0;
    }

    info->base = h->base;
    info->size = h->size;

    ctrdl_unlockHandle(h);
    return success;
}

void ctrdlFreeInfo(CTRDLInfo* info) {
    if (info)
        free(info->path);
}