#ifndef APPLE_AGX_PA_H
#define APPLE_AGX_PA_H
/* Physical (UAT output) address width: J313/T8103 40 bits, J316s/T6000 42 bits
 * (Asahi hw/t600x.rs uat_oas = 42; DRAM starts at 0x10000000000). GPU virtual
 * address limits are unchanged and stay 40-bit. */
#if defined(APPLE_AGX_PLATFORM_J316S) || defined(HV_AGX_PLATFORM_J316S)
#define APPLE_AGX_PA_BITS 42u
#define APPLE_AGX_PA_LIMIT (1ULL << 42)
#define APPLE_AGX_UAT_OA_MASK 0x3ffffffc000ULL
#else
#define APPLE_AGX_PA_BITS 40u
#define APPLE_AGX_PA_LIMIT (1ULL << 40)
#define APPLE_AGX_UAT_OA_MASK 0xffffffc000ULL
#endif
#endif
