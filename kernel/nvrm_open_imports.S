/* Extra host imports required by the OPEN-kernel-module 595.99.02 cores
 * (kernel-open/nvidia/nv-kernel.o_binary + nvidia-modeset) that the closed
 * cores did not reference.  Split into three groups:
 *
 *  1. Retpoline thunk   - MUST be a real indirect jump (a 0-return stub would
 *                         break every indirect call the blob makes through it).
 *  2. nvlink locks      - no NVLink on a single discrete GPU; give a real (but
 *                         trivial) lock so a non-NULL handle is returned.
 *  3. libspdm + tegra   - SPDM attestation runs only in Confidential Computing
 *                         mode (os_cc_enabled = NV_FALSE here) and DCE is Tegra
 *                         SoC only; neither is reached on a non-CC discrete
 *                         GeForce boot, so these only need to satisfy the link.
 *                         Left weak so a real implementation can override later.
 */
.section .text

/* ---- 1. retpoline thunk: jump to the address held in %rbp ---------------- */
.globl __x86_indirect_thunk_rbp
.type  __x86_indirect_thunk_rbp,@function
__x86_indirect_thunk_rbp:
    jmp *%rbp
.size __x86_indirect_thunk_rbp, .-__x86_indirect_thunk_rbp

/* ---- 2. nvlink locks: return a valid non-NULL handle, no-op the rest ----- */
.globl nvlink_allocLock
.type  nvlink_allocLock,@function
nvlink_allocLock:
    lea nvrm_nvlink_dummy_lock(%rip), %rax
    ret
.size nvlink_allocLock, .-nvlink_allocLock

.macro NVRM_STUB0 name
.globl \name
.type  \name,@function
\name:
    xor %eax, %eax
    ret
.size \name, .-\name
.endm

NVRM_STUB0 nvlink_acquireLock
NVRM_STUB0 nvlink_releaseLock
NVRM_STUB0 nvlink_freeLock

/* ---- 3. Tegra DCE (SoC only) --------------------------------------------- */
NVRM_STUB0 nv_tegra_dce_client_ipc_send_recv
NVRM_STUB0 nv_tegra_dce_register_ipc_client
NVRM_STUB0 nv_tegra_dce_unregister_ipc_client
NVRM_STUB0 nv_tegra_get_rm_interface_type

/* ---- 3. libspdm (Confidential Computing only; weak so it can be replaced) - */
.macro SPDM_WEAK name
.weak \name
.type \name,@function
\name:
    xor %eax, %eax
    ret
.size \name, .-\name
.endm

SPDM_WEAK libspdm_aead_aes_gcm_decrypt
SPDM_WEAK libspdm_aead_aes_gcm_decrypt_prealloc
SPDM_WEAK libspdm_aead_aes_gcm_encrypt
SPDM_WEAK libspdm_aead_aes_gcm_encrypt_prealloc
SPDM_WEAK libspdm_aead_free
SPDM_WEAK libspdm_aead_gcm_prealloc
SPDM_WEAK libspdm_asn1_get_tag
SPDM_WEAK libspdm_check_crypto_backend
SPDM_WEAK libspdm_decode_base64
SPDM_WEAK libspdm_ec_compute_key
SPDM_WEAK libspdm_ec_free
SPDM_WEAK libspdm_ec_generate_key
SPDM_WEAK libspdm_ec_get_public_key_from_x509
SPDM_WEAK libspdm_ec_new_by_nid
SPDM_WEAK libspdm_ecdsa_sign
SPDM_WEAK libspdm_ecdsa_verify
SPDM_WEAK libspdm_encode_base64
SPDM_WEAK libspdm_hkdf_sha256_expand
SPDM_WEAK libspdm_hkdf_sha256_extract
SPDM_WEAK libspdm_hkdf_sha384_expand
SPDM_WEAK libspdm_hkdf_sha384_extract
SPDM_WEAK libspdm_hmac_sha256_all
SPDM_WEAK libspdm_hmac_sha256_final
SPDM_WEAK libspdm_hmac_sha256_free
SPDM_WEAK libspdm_hmac_sha256_new
SPDM_WEAK libspdm_hmac_sha256_set_key
SPDM_WEAK libspdm_hmac_sha256_update
SPDM_WEAK libspdm_hmac_sha384_all
SPDM_WEAK libspdm_random_bytes
SPDM_WEAK libspdm_rsa_free
SPDM_WEAK libspdm_rsa_get_public_key_from_x509
SPDM_WEAK libspdm_rsa_new
SPDM_WEAK libspdm_rsa_pss_sign
SPDM_WEAK libspdm_rsa_pss_verify
SPDM_WEAK libspdm_rsa_set_key
SPDM_WEAK libspdm_sha256_duplicate
SPDM_WEAK libspdm_sha256_final
SPDM_WEAK libspdm_sha256_free
SPDM_WEAK libspdm_sha256_hash_all
SPDM_WEAK libspdm_sha256_init
SPDM_WEAK libspdm_sha256_new
SPDM_WEAK libspdm_sha256_update
SPDM_WEAK libspdm_sha384_duplicate
SPDM_WEAK libspdm_sha384_final
SPDM_WEAK libspdm_sha384_free
SPDM_WEAK libspdm_sha384_hash_all
SPDM_WEAK libspdm_sha384_init
SPDM_WEAK libspdm_sha384_new
SPDM_WEAK libspdm_sha384_update
SPDM_WEAK libspdm_x509_compare_date_time
SPDM_WEAK libspdm_x509_get_cert_from_cert_chain
SPDM_WEAK libspdm_x509_get_extended_basic_constraints
SPDM_WEAK libspdm_x509_get_extended_key_usage
SPDM_WEAK libspdm_x509_get_extension_data
SPDM_WEAK libspdm_x509_get_issuer_name
SPDM_WEAK libspdm_x509_get_key_usage
SPDM_WEAK libspdm_x509_get_serial_number
SPDM_WEAK libspdm_x509_get_signature_algorithm
SPDM_WEAK libspdm_x509_get_subject_name
SPDM_WEAK libspdm_x509_get_validity
SPDM_WEAK libspdm_x509_get_version
SPDM_WEAK libspdm_x509_set_date_time
SPDM_WEAK libspdm_x509_verify_cert
SPDM_WEAK libspdm_x509_verify_cert_chain

.section .data
nvrm_nvlink_dummy_lock:
    .quad 0

.section .note.GNU-stack,"",@progbits
