#include "schnorr_wrap.h"

/*
* IMPORTANT:
*  - 工程内同时存在旧版 src/secp256k1 与新版 third_party/secp256k1_upstream。
*  - 为避免最终链接时出现同名符号(secp256k1_*)重复定义，新版 upstream 会在构建后通过 objcopy
*    给所有符号加前缀：secp256k1_up_。
*  - 因此在包含 upstream 头文件前，将我们用到的 API 名称宏替换为加前缀后的符号名。
*/
#define secp256k1_context_create      secp256k1_up_context_create
#define secp256k1_context_destroy     secp256k1_up_context_destroy
#define secp256k1_context_randomize   secp256k1_up_context_randomize

#define secp256k1_xonly_pubkey_parse  secp256k1_up_xonly_pubkey_parse
#define secp256k1_schnorrsig_verify   secp256k1_up_schnorrsig_verify
// ...把你在 wrapper 中实际用到的 API 都映射一下

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

static secp256k1_context* g_ctx = NULL;
static int g_ctx_refcnt = 0;

static secp256k1_context* get_ctx(void) {
    if (!g_ctx) {
        g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    }
    return g_ctx;
}

void tbc_schnorr_ctx_ref(void) {
    if (g_ctx_refcnt == 0) {
        (void)get_ctx();
    }
    g_ctx_refcnt++;
}

void tbc_schnorr_ctx_unref(void) {
    if (g_ctx_refcnt <= 0) return;
    g_ctx_refcnt--;
    if (g_ctx_refcnt == 0 && g_ctx) {
        secp256k1_context_destroy(g_ctx);
        g_ctx = NULL;
    }
}

int tbc_xonly_pubkey_parse(const uint8_t xonly_pk32[32]) {
    secp256k1_context* ctx = get_ctx();
    if (!ctx) return 0;

    secp256k1_xonly_pubkey pk;
    return secp256k1_xonly_pubkey_parse(ctx, &pk, xonly_pk32) ? 1 : 0;
}

int tbc_schnorr_verify_bip340(
    const uint8_t msg32[32],
    const uint8_t sig64[64],
    const uint8_t xonly_pk32[32]
) {
    secp256k1_context* ctx = get_ctx();
    if (!ctx) { return 0; }

    secp256k1_xonly_pubkey pk;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pk, xonly_pk32)) { return 0; }
    
    return secp256k1_schnorrsig_verify(ctx, sig64, msg32, 32, &pk);
}
