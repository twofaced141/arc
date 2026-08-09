#include <stdint.h>
#include <stddef.h>
#include "ipc.h"
#include "debug.h"
#include "string.h"

/* ================================================================
 * ipc_test.c — Boot-time self-test for the IPC ABI
 *
 * Verifies that the typed protocol structures match the ad-hoc
 * binary layout used by the pager protocol.
 *
 * Called from main.c during boot (currently amd64).
 * ================================================================ */

void ipc_abi_test(void) {
    if (IPC_FAULT_REQ != 0x8001) return;
    if (IPC_FAULT_RESP != 0x8002) return;
    if (sizeof(ipc_pager_fault_req_t) > 64) return;
    if (sizeof(ipc_pager_fault_resp_t) > 64) return;
    if (sizeof(ipc_pager_fault_req_t) != 32) return;

    ipc_pager_fault_req_t req;
    memset(&req, 0, sizeof(req));
    req.object_id    = 0xDEADBEEFCAFEBABEull;
    req.fault_offset = 0x0000000000001000ull;
    req.prot         = 0x03;
    req.fault_tid    = 42;
    req.reply_handle = 0x0000000100000007ull;

    uint8_t raw[64];
    memset(raw, 0xAA, sizeof(raw));
    *(uint64_t *)(raw + 0)  = 0xDEADBEEFCAFEBABEull;
    *(uint64_t *)(raw + 8)  = 0x0000000000001000ull;
    *(uint32_t *)(raw + 16) = 0x03;
    *(uint32_t *)(raw + 20) = 42;
    *(uint64_t *)(raw + 24) = 0x0000000100000007ull;

    if (memcmp(&req, raw, sizeof(req)) != 0) return;

    ipc_pager_fault_req_t *r = (ipc_pager_fault_req_t *)raw;
    if (r->object_id    != 0xDEADBEEFCAFEBABEull ||
        r->fault_offset != 0x0000000000001000ull ||
        r->prot         != 0x03                  ||
        r->fault_tid    != 42                    ||
        r->reply_handle != 0x0000000100000007ull) return;

    ipc_pager_fault_resp_t resp;
    resp.phys_addr = 0x0000000000A99000ull;
    uint8_t resp_raw[64];
    memset(resp_raw, 0xBB, sizeof(resp_raw));
    *(uint64_t *)(resp_raw + 0) = 0x0000000000A99000ull;
    if (memcmp(&resp, resp_raw, sizeof(resp)) != 0) return;

    if (SC_PORT_CREATE  != 17 || SC_PORT_SEND != 19 ||
        SC_PORT_RECV    != 20 || SC_PORT_CALL != 21 ||
        SC_PORT_REPLY   != 22 || SC_PORT_POLL != 24 ||
        SC_BSD_BASE     != 1024) return;
}
