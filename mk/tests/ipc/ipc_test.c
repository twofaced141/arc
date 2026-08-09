#include "test.h"
#include "ipc.h"
#include "port.h"
#include "string.h"
#include "debug.h"

void ipc_boot_tests(void) {
    test_group("ipc");
    int ok;

    ok = 1;
    ok = ok && test_check_int(IPC_ID_INVALID, 0x0000);
    ok = ok && test_check_int(IPC_FAULT_REQ, 0x8001);
    ok = ok && test_check_int(IPC_FAULT_RESP, 0x8002);
    ok = ok && test_check_int(SC_PORT_CREATE, 17);
    ok = ok && test_check_int(SC_PORT_SEND, 19);
    ok = ok && test_check_int(SC_PORT_RECV, 20);
    ok = ok && test_check_int(SC_PORT_CALL, 21);
    ok = ok && test_check_int(SC_PORT_REPLY, 22);
    ok = ok && test_check_int(SC_PORT_POLL, 24);
    ok = ok && test_check_int(SC_BSD_BASE, 1024);
    test_result("ipc_constants", ok);

    ok = test_check_int(sizeof(ipc_pager_fault_req_t), 32);
    ok = ok && test_check_int(sizeof(ipc_pager_fault_req_t) <= 64, 1);
    ok = ok && test_check_int(sizeof(ipc_pager_fault_resp_t) <= 64, 1);
    ok = ok && test_check_int(sizeof(ipc_pager_fault_resp_t), 8);
    test_result("ipc_struct_sizes", ok);

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

    ok = test_check_int(memcmp(&req, raw, sizeof(req)), 0);
    test_result("ipc_fault_req_layout", ok);

    {
        ipc_pager_fault_req_t *r = (ipc_pager_fault_req_t *)raw;
        ok = 1;
        ok = ok && test_check_u64(r->object_id,    0xDEADBEEFCAFEBABEull);
        ok = ok && test_check_u64(r->fault_offset, 0x0000000000001000ull);
        ok = ok && test_check_int(r->prot,         0x03);
        ok = ok && test_check_int(r->fault_tid,    42);
        ok = ok && test_check_u64(r->reply_handle, 0x0000000100000007ull);
        test_result("ipc_fault_req_fields", ok);
    }

    {
        ipc_pager_fault_resp_t resp;
        resp.phys_addr = 0x0000000000A99000ull;
        uint8_t resp_raw[64];
        memset(resp_raw, 0xBB, sizeof(resp_raw));
        *(uint64_t *)(resp_raw + 0) = 0x0000000000A99000ull;
        ok = test_check_int(memcmp(&resp, resp_raw, sizeof(resp)), 0);
        test_result("ipc_fault_resp_layout", ok);
    }

    ok = 1;
    ok = ok && test_check_int(CAP_PORT,   1);
    ok = ok && test_check_int(CAP_MEMORY, 2);
    ok = ok && test_check_int(CAP_THREAD, 3);
    ok = ok && test_check_int(CAP_SEND,   1);
    ok = ok && test_check_int(CAP_RECV,   2);
    ok = ok && test_check_int(CAP_REPLY,  4);
    ok = ok && test_check_int(CAP_READ,   8);
    ok = ok && test_check_int(CAP_WRITE,  16);
    ok = ok && test_check_int(CAP_EXEC,   32);
    test_result("ipc_cap_constants", ok);
}
