#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>

#define VIRTIO_PCI_VENDOR     0x1AF4
#define VIRTIO_PCI_DEVICE_NET 0x1000

#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_SIZE      0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_DEVICE_STATUS   0x12
#define VIRTIO_PCI_ISR             0x13
#define VIRTIO_PCI_DEVICE_CFG      0x14

#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

#define VIRTIO_F_NOTIFY_ON_EMPTY   (1 << 24)
#define VIRTIO_F_ANY_LAYOUT        (1 << 27)
#define VIRTIO_F_RING_INDIRECT_DESC (1 << 28)
#define VIRTIO_F_RING_EVENT_IDX    (1 << 29)
#define VIRTIO_NET_F_MAC           (1 << 5)
#define VIRTIO_NET_F_STATUS        (1 << 16)

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

#endif
