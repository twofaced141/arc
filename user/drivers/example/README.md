# example — minimal driver

A template driver for exploring the Arc userspace driver API.

## Functionality

Registers the "example" service and exits. It does not interact with any hardware; it serves purely as an ABI demonstration.

## Building

```make
make ARCH=amd64
```

## API

- `svc_register(name, data)` — register a service
- `driver_exit(status)` — exit the driver
- `puts(str)` — debug output
