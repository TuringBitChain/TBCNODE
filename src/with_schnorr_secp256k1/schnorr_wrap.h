#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 引用/释放 schnorr verify 上下文（可选；也可依赖内部懒初始化）
void tbc_schnorr_ctx_ref(void);
void tbc_schnorr_ctx_unref(void);

// 1 = x-only pubkey 可被解析；0 = 失败
int tbc_xonly_pubkey_parse(const uint8_t xonly_pk32[32]);

// 返回 1 = verify 成功；0 = 失败
int tbc_schnorr_verify_bip340(
    const uint8_t msg32[32],
    const uint8_t sig64[64],
    const uint8_t xonly_pk32[32]
);

#ifdef __cplusplus
}
#endif
