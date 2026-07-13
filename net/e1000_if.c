#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/init.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include "e1000.h"
#include "debug.h"
#include "vmm.h"

static struct netif e1000_netif;
static int netif_initialized;

static err_t e1000if_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    debug_printf("e1000if_output: pbuf len=%u tot_len=%u\r\n", p->len, p->tot_len);
    struct pbuf *q;
    uint8_t buf[2048];
    uint16_t off = 0;

    for (q = p; q != NULL; q = q->next) {
        uint16_t copy = q->len;
        if (off + copy > sizeof(buf)) copy = sizeof(buf) - off;
        for (uint16_t i = 0; i < copy; i++)
            buf[off + i] = ((uint8_t *)q->payload)[i];
        off += copy;
    }

    e1000_send(buf, off);
    return ERR_OK;
}

static err_t e1000if_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = '0';
    netif->output = etharp_output;
    netif->linkoutput = e1000if_output;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    e1000_get_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void e1000if_input(void *pkt, uint16_t len) {
    if (!netif_initialized) {
        kfree(pkt);
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) { kfree(pkt); return; }

    struct pbuf *q;
    uint16_t off = 0;
    for (q = p; q != NULL; q = q->next) {
        uint16_t copy = q->len;
        if (off + copy > len) copy = len - off;
        for (uint16_t i = 0; i < copy; i++)
            ((uint8_t *)q->payload)[i] = ((uint8_t *)pkt)[off + i];
        off += copy;
    }

    kfree(pkt);

    struct eth_hdr *eth = (struct eth_hdr *)p->payload;
    if (ntohs(eth->type) == ETHTYPE_IP || ntohs(eth->type) == ETHTYPE_ARP) {
        if (e1000_netif.input(p, &e1000_netif) != ERR_OK)
            pbuf_free(p);
    } else {
        pbuf_free(p);
    }
}

void net_init(void) {
    lwip_init();

    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&gw, 10, 0, 2, 2);
    IP4_ADDR(&ip, 0, 0, 0, 0);
    IP4_ADDR(&mask, 0, 0, 0, 0);

    netif_add(&e1000_netif, &ip, &mask, &gw, NULL, e1000if_init, ethernet_input);
    netif_set_default(&e1000_netif);
    netif_set_up(&e1000_netif);

    netif_initialized = 1;

    debug_printf("net: waiting for DHCP...\r\n");
    dhcp_start(&e1000_netif);
}

static int dhcp_done;

/* ── HTTP test: TCP GET example.com ── */
static struct tcp_pcb *http_pcb;

static void http_tcp_err(void *arg, err_t err) {
    (void)arg;
    debug_printf("http: TCP error=%d\r\n", err);
    http_pcb = NULL;
}

static err_t http_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK) { debug_printf("http: recv err=%d\r\n", err); return ERR_OK; }
    if (!p) {
        debug_print("http: connection closed\r\n");
        http_pcb = NULL;
        return ERR_OK;
    }

    uint16_t len = p->tot_len;
    uint8_t *data = (uint8_t *)kmalloc(len + 1);
    if (data) {
        uint16_t off = 0;
        struct pbuf *q;
        for (q = p; q; q = q->next) {
            for (uint16_t i = 0; i < q->len && off < len; i++)
                data[off++] = ((uint8_t *)q->payload)[i];
        }
        data[len] = 0;
        debug_printf("http: got %u bytes:\r\n", len);
        uint16_t show = len < 512 ? len : 512;
        for (uint16_t i = 0; i < show; i++)
            debug_putchar(data[i]);
        debug_printf("\r\n");
        kfree(data);
    }

    tcp_recved(pcb, len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t http_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        debug_printf("http: connect err=%d\r\n", err);
        http_pcb = NULL;
        return ERR_OK;
    }
    debug_print("http: connected, sending GET\r\n");

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    uint16_t req_len = 0;
    while (req[req_len]) req_len++;

    err_t werr = tcp_write(pcb, req, req_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK)
        debug_printf("http: tcp_write err=%d\r\n", werr);
    tcp_output(pcb);
    return ERR_OK;
}

static void http_test_run(void) {
    ip_addr_t ip;
    IP4_ADDR(&ip, 74, 125, 205, 101);

    http_pcb = tcp_new();
    if (!http_pcb) { debug_print("http: tcp_new failed\r\n"); return; }
    tcp_bind(http_pcb, IP4_ADDR_ANY, 0);
    tcp_arg(http_pcb, NULL);
    tcp_recv(http_pcb, http_tcp_recv);
    tcp_err(http_pcb, http_tcp_err);

    err_t err = tcp_connect(http_pcb, &ip, 80, http_tcp_connected);
    if (err != ERR_OK) {
        debug_printf("http: tcp_connect err=%d\r\n", err);
        tcp_close(http_pcb);
        http_pcb = NULL;
    } else {
        debug_print("http: connecting to 74.125.205.101:80...\r\n");
    }
}

void net_poll(void) {
    if (!netif_initialized) return;

    static int npc;
    if (++npc <= 3) debug_printf("net_poll\r\n");

    sys_check_timeouts();
    e1000_poll();

    if (!dhcp_done && dhcp_supplied_address(&e1000_netif)) {
        dhcp_done = 1;
        debug_printf("net: DHCP OK, IP=%d.%d.%d.%d\r\n",
            ip4_addr1(&e1000_netif.ip_addr), ip4_addr2(&e1000_netif.ip_addr),
            ip4_addr3(&e1000_netif.ip_addr), ip4_addr4(&e1000_netif.ip_addr));
    }
}
