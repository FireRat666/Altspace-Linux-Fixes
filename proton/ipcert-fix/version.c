/*
 * version.dll shim for Unity games under Wine/Proton.
 *
 * Proton (up to at least cachyos-11.0-20261015) has a crypt32 that ignores
 * IP-address names in certificates. So HTTPS to a bare IP that has an IP
 * certificate (e.g. Altspace's https://158.69.117.174/join, a Let's Encrypt IP
 * cert) fails with CERT_E_CN_NO_MATCH, which Unity reports as
 * "Curl error 60 ... Common Name(CN) does not match ... UnityTls error code: 7".
 * Wine fixed this upstream on 2026-10-19 (commit 563aa5f6fe).
 *
 * This DLL sits next to the game exe. It forwards every version.dll function
 * to the real one, and wraps UnityPlayer.dll's call to
 * CertVerifyCertificateChainPolicy: if Wine says "name doesn't match", the
 * server name is an IP, and the certificate lists that exact IP, the error is
 * cleared. Every other result (bad chain, expired, wrong IP) is left alone.
 *
 * Built without any Windows SDK; see build.sh.
 */

typedef unsigned char BYTE;
typedef unsigned short WCHAR;
typedef unsigned int DWORD;
typedef int BOOL;
typedef long LONG;
typedef unsigned long long SIZE_T;
typedef void *HMODULE;

#define WINAPI __stdcall
#define DLL_PROCESS_ATTACH 1
#define PAGE_READWRITE 4
#define CERT_E_CN_NO_MATCH 0x800B010Fu
#define CERT_CHAIN_POLICY_SSL ((const char *)4)

__declspec(dllimport) HMODULE WINAPI GetModuleHandleW(const WCHAR *);
__declspec(dllimport) HMODULE WINAPI LoadLibraryW(const WCHAR *);
__declspec(dllimport) void *WINAPI GetProcAddress(HMODULE, const char *);
__declspec(dllimport) BOOL WINAPI VirtualProtect(void *, SIZE_T, DWORD, DWORD *);
__declspec(dllimport) DWORD WINAPI GetSystemDirectoryW(WCHAR *, DWORD);
__declspec(dllimport) void WINAPI OutputDebugStringA(const char *);

/* ---- forwarding to the real version.dll ---- */

static const char *const names[16] = {
    "GetFileVersionInfoA", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA",
    "VerInstallFileW", "VerLanguageNameA", "VerLanguageNameW",
    "VerQueryValueA", "VerQueryValueW"};
void *real_fn[16];

/* Each export is a jump through real_fn[i], so arguments pass through as-is. */
#define STUB(i, name) \
    __asm__(".globl " #name "\n" #name ":\n jmp *real_fn+" #i "*8(%rip)\n");
STUB(0, GetFileVersionInfoA)
STUB(1, GetFileVersionInfoExA)
STUB(2, GetFileVersionInfoExW)
STUB(3, GetFileVersionInfoSizeA)
STUB(4, GetFileVersionInfoSizeExA)
STUB(5, GetFileVersionInfoSizeExW)
STUB(6, GetFileVersionInfoSizeW)
STUB(7, GetFileVersionInfoW)
STUB(8, VerFindFileA)
STUB(9, VerFindFileW)
STUB(10, VerInstallFileA)
STUB(11, VerInstallFileW)
STUB(12, VerLanguageNameA)
STUB(13, VerLanguageNameW)
STUB(14, VerQueryValueA)
STUB(15, VerQueryValueW)

static void load_real_version(void)
{
    WCHAR path[300];
    const WCHAR tail[] = {'\\','v','e','r','s','i','o','n','.','d','l','l',0};
    DWORD n = GetSystemDirectoryW(path, 280), i;
    HMODULE real;
    if (!n || n >= 280) return;
    for (i = 0; tail[i]; i++) path[n + i] = tail[i];
    path[n + i] = 0;
    if (!(real = LoadLibraryW(path))) return;
    for (i = 0; i < 16; i++) real_fn[i] = GetProcAddress(real, names[i]);
}

/* ---- the certificate name fix ---- */

typedef BOOL (WINAPI *verify_fn)(const char *, const void *, const void *, void *);
static verify_fn real_verify;
static LONG (WINAPI *ipv4_parse)(const WCHAR *, BOOL, const WCHAR **, BYTE *);
static LONG (WINAPI *ipv6_parse)(const WCHAR *, const WCHAR **, BYTE *);

/* Read a DER length at p (end is the buffer end); returns 0 on bad input. */
static const BYTE *der_len(const BYTE *p, const BYTE *end, DWORD *len)
{
    DWORD n, i;
    if (p >= end) return 0;
    if (*p < 0x80) { *len = *p; return p + 1; }
    n = *p++ & 0x7f;
    if (n == 0 || n > 3 || p + n > end) return 0;
    for (*len = 0, i = 0; i < n; i++) *len = (*len << 8) | *p++;
    return p;
}

/* Does the certificate's subjectAltName list this IP (4 or 16 bytes)? */
static BOOL cert_has_ip(const BYTE *cert, DWORD size, const BYTE *ip, DWORD iplen)
{
    const BYTE *end = cert + size, *p, *q;
    DWORD len, i;
    for (p = cert; p + 5 <= end; p++)
    {
        /* OID 2.5.29.17 (subjectAltName) */
        if (p[0] != 0x06 || p[1] != 0x03 || p[2] != 0x55 || p[3] != 0x1d || p[4] != 0x11)
            continue;
        q = p + 5;
        if (q + 3 <= end && q[0] == 0x01 && q[1] == 0x01) q += 3;  /* critical flag */
        if (q >= end || *q++ != 0x04 || !(q = der_len(q, end, &len))) return 0;
        if (q >= end || *q++ != 0x30 || !(q = der_len(q, end, &len))) return 0;
        if (q + len > end) return 0;
        end = q + len;
        while (q < end)
        {
            BYTE tag = *q++;
            if (!(q = der_len(q, end, &len)) || q + len > end) return 0;
            if (tag == 0x87 && len == iplen)   /* [7] iPAddress */
            {
                for (i = 0; i < len && q[i] == ip[i]; i++) ;
                if (i == len) return 1;
            }
            q += len;
        }
        return 0;
    }
    return 0;
}

/* Layouts of the few x64 CryptoAPI struct fields we read. */
struct chain_ctx   { DWORD cbSize, trust[2], cChain; struct simple_chain **rgpChain; };
struct simple_chain{ DWORD cbSize, trust[2], cElement; struct chain_elem **rgpElement; };
struct chain_elem  { DWORD cbSize; struct cert_ctx *pCertContext; };
struct cert_ctx    { DWORD enc; BYTE *pbCertEncoded; DWORD cbCertEncoded; };
struct policy_para { DWORD cbSize, dwFlags; void *pvExtraPolicyPara; };
struct ssl_para    { DWORD cbSize, dwAuthType, fdwChecks; WCHAR *pwszServerName; };
struct policy_status { DWORD cbSize, dwError; LONG lChainIndex, lElementIndex; };

static BOOL WINAPI fixed_verify(const char *oid, const void *chain_v,
                                const void *para_v, void *status_v)
{
    const struct chain_ctx *chain = chain_v;
    const struct policy_para *para = para_v;
    struct policy_status *status = status_v;
    const struct ssl_para *ssl;
    const struct cert_ctx *leaf;
    const WCHAR *term;
    BYTE ip[16];
    DWORD iplen = 0;
    BOOL ret = real_verify(oid, chain_v, para_v, status_v);

    if (!ret || oid != CERT_CHAIN_POLICY_SSL || !status || status->dwError != CERT_E_CN_NO_MATCH)
        return ret;
    if (!para || para->cbSize < sizeof(*para) || !(ssl = para->pvExtraPolicyPara) ||
        ssl->cbSize < sizeof(*ssl) || !ssl->pwszServerName)
        return ret;
    if (!chain || !chain->cChain || !chain->rgpChain[0]->cElement)
        return ret;
    leaf = chain->rgpChain[0]->rgpElement[0]->pCertContext;

    if (ipv4_parse && !ipv4_parse(ssl->pwszServerName, 1, &term, ip) && !*term) iplen = 4;
    else if (ipv6_parse && !ipv6_parse(ssl->pwszServerName, &term, ip) && !*term) iplen = 16;
    if (!iplen) return ret;   /* a hostname: Wine already handles those */

    if (cert_has_ip(leaf->pbCertEncoded, leaf->cbCertEncoded, ip, iplen))
    {
        status->dwError = 0;
        status->lChainIndex = status->lElementIndex = -1;
        OutputDebugStringA("ipcert-fix: accepted IP certificate\n");
    }
    return ret;
}

/* Point UnityPlayer.dll's import of CertVerifyCertificateChainPolicy at ours. */
static void hook_unityplayer(void)
{
    const WCHAR up[] = {'U','n','i','t','y','P','l','a','y','e','r','.','d','l','l',0};
    const WCHAR c32[] = {'c','r','y','p','t','3','2','.','d','l','l',0};
    const WCHAR nt[] = {'n','t','d','l','l','.','d','l','l',0};
    BYTE *base = GetModuleHandleW(up), *imp;
    HMODULE crypt32 = LoadLibraryW(c32), ntdll = GetModuleHandleW(nt);
    DWORD imp_rva, old;

    if (!base || !crypt32) return;
    real_verify = (verify_fn)GetProcAddress(crypt32, "CertVerifyCertificateChainPolicy");
    ipv4_parse = GetProcAddress(ntdll, "RtlIpv4StringToAddressW");
    ipv6_parse = GetProcAddress(ntdll, "RtlIpv6StringToAddressW");
    if (!real_verify) return;

    /* PE header -> import directory */
    imp_rva = *(DWORD *)(base + *(DWORD *)(base + 0x3c) + 0x18 + 0x70 + 8);
    if (!imp_rva) return;
    for (imp = base + imp_rva; *(DWORD *)(imp + 12); imp += 20)
    {
        void **slot = (void **)(base + *(DWORD *)(imp + 16));   /* FirstThunk */
        for (; *slot; slot++)
        {
            if (*slot != (void *)real_verify) continue;
            if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
            {
                *slot = (void *)fixed_verify;
                VirtualProtect(slot, sizeof(*slot), old, &old);
                OutputDebugStringA("ipcert-fix: hooked UnityPlayer cert check\n");
            }
            return;
        }
    }
}

BOOL WINAPI DllMain(HMODULE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        load_real_version();
        hook_unityplayer();
    }
    return 1;
}
