/* 以纯 C 编译并链接动态库：确认 iis.h 是合法 C 头文件、导出符号齐全、跨模块分配释放正常。 */
#include "iis/iis.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t px[16 * 8];
    iis_image_info info = {16, 8, 1, 8};
    iis_buffer enc = {0}, dec = {0};
    uint8_t d1[32], d2[32];
    int i;

    for (i = 0; i < (int)sizeof px; ++i) px[i] = (uint8_t)(i * 7);
    if (iis_abi_version() != IIS_ABI_VERSION) return 1;
    if (iis_jpegls_encode(px, sizeof px, &info, &enc) != IIS_OK) return 2;
    if (iis_jpegls_decode(enc.data, enc.size, &info, &dec) != IIS_OK) return 3;
    iis_sha256(px, sizeof px, d1);
    iis_sha256(dec.data, dec.size, d2);
    if (dec.size != sizeof px || memcmp(d1, d2, 32) != 0) return 4;
    iis_buffer_free(&enc);
    iis_buffer_free(&dec);
    if (iis_crc32c(0, "123456789", 9) != 0xE3069283u) return 5;
    {
        /* 文件与线程池接口：只检查 C 下可编译、可链接、参数校验正常 */
        iis_file_options fo;
        iis_file_result fr;
        iis_pool* pool = NULL;
        memset(&fo, 0, sizeof fo);
        if (iis_compress_file("", &fo, &fr) != IIS_E_INVALID_ARG) return 6;
        if (iis_pool_create(1, &fo, NULL, NULL, &pool) != IIS_OK) return 7;
        iis_pool_wait(pool);
        if (iis_pool_pending(pool) != 0) return 8;
        iis_pool_destroy(pool);
    }
    printf("c_abi_smoke ok: %s\n", iis_build_info());
    return 0;
}
