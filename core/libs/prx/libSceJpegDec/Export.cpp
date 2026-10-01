#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int32_t APS5_VABI sceJpegDecCreate(const void* param, void* memory, uint32_t memorySize, void** handle) {
 (void)param;
 (void)memory;
 (void)memorySize;
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecDelete(void* handle) {
 (void)handle;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecQueryMemorySize(const void* param) {
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecParseHeader(const void* param, void* imageInfo) {
 (void)param;
 (void)imageInfo;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecDecode(void* handle, const void* param, void* imageInfo) {
 (void)handle;
 (void)param;
 (void)imageInfo;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecQueryMemorySizeWithInputControl(const void* param) {
 (void)param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecParseHeaderWithInputControl(const void* param, void* imageInfo) {
 (void)param;
 (void)imageInfo;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int32_t APS5_VABI sceJpegDecDecodeWithInputControl(void* handle, const void* param, void* imageInfo) {
 (void)handle;
 (void)param;
 (void)imageInfo;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
