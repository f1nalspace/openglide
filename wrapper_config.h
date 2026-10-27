#ifndef WRAPPER_CONFIG_H
#define WRAPPER_CONFIG_H

#include "sdk2_3dfx.h"

#define WRAPPER_FLAG_WINDOWED               (0x1)
#define WRAPPER_FLAG_MIPMAPS                (0x2)
#define WRAPPER_FLAG_MSAA_MASK              (0xC)
#define WRAPPER_FLAG_ANNOTATE               (0x10)
#define WRAPPER_FLAG_FRAMEBUFFER_SRGB       (0x20)
#define WRAPPER_FLAG_VSYNCOFF               (0x40)
#define WRAPPER_FLAG_QEMU                   (0x80)

/* The severity setConfigMessages hands over with every message. */
#define WRAPPER_MESSAGE_INFO                (0)
#define WRAPPER_MESSAGE_ERROR               (1)

#ifdef __cplusplus
extern "C" {
#endif

FX_ENTRY void FX_CALL setConfig(FxU32 flags, void *magic);
FX_ENTRY void FX_CALL setConfigRes(int res, void *swap12);
FX_ENTRY void FX_CALL setConfigOffset(int x, int y);
FX_ENTRY void FX_CALL setConfigWindow(int width, int height, int offset_x);
FX_ENTRY void FX_CALL setConfigWindowOffset(int width, int height, int offset_x, int offset_y);
FX_ENTRY void FX_CALL setConfigPresentHook(void (*present_hook)(void));
FX_ENTRY void FX_CALL setConfigMessages(void (*messages)(int severity, const char *text));

#ifdef __cplusplus
}
#endif

#endif /* WRAPPER_CONFIG_H */
