/* Vtable slots of the hooked methods, taken from the Windows SDK headers.
 *
 * In C, the SDK declares each COM interface as a struct of function pointers
 * (IDXGIOutput1Vtbl, ...) listed in vtable order, including all inherited
 * methods. A member's offset divided by the pointer size is its vtable slot.
 * This file is C (not C++) so those declarations are the ones we get. */
#include <stddef.h>
#include <dxgi1_6.h>

const size_t kSlot_IDXGIOutput1_DuplicateOutput =
    offsetof(IDXGIOutput1Vtbl, DuplicateOutput) / sizeof(void*);

const size_t kSlot_IDXGIOutput5_DuplicateOutput1 =
    offsetof(IDXGIOutput5Vtbl, DuplicateOutput1) / sizeof(void*);
