// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/metadata.h>
#include <ddk/protocol/platform-defs.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "platform-bus.h"
#include "platform-device.h"
#include "platform-proxy.h"

static zx_status_t platform_dev_map_mmio(void* ctx, uint32_t index, uint32_t cache_policy,
                                         void** vaddr, size_t* size, zx_paddr_t* out_paddr,
                                         zx_handle_t* out_handle) {
    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);

    if (index >= dev->mmios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }

    pbus_mmio_t& mmio = dev->mmios[index];
    zx_paddr_t vmo_base = ROUNDDOWN(mmio.base, PAGE_SIZE);
    size_t vmo_size = ROUNDUP(mmio.base + mmio.length - vmo_base, PAGE_SIZE);
    zx_handle_t vmo_handle;
    zx_status_t status = zx_vmo_create_physical(dev->bus->resource, vmo_base, vmo_size,
                                                &vmo_handle);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmo_create_physical failed %d\n", status);
        return status;
    }

    status = zx_vmo_set_cache_policy(vmo_handle, cache_policy);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmo_set_cache_policy failed %d\n", status);
        goto fail;
    }

    uintptr_t virt;
    status = zx_vmar_map(zx_vmar_root_self(), 0, vmo_handle, 0, vmo_size,
                         ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE | ZX_VM_FLAG_MAP_RANGE,
                         &virt);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmar_map failed %d\n", status);
        goto fail;
    }

    *size = mmio.length;
    *out_handle = vmo_handle;
    if (out_paddr) {
        *out_paddr = vmo_base;
    }
    *vaddr = (void *)(virt + (mmio.base - vmo_base));
    return ZX_OK;

fail:
    zx_handle_close(vmo_handle);
    return status;
}

static zx_status_t platform_dev_map_interrupt(void* ctx, uint32_t index,
                                              uint32_t flags, zx_handle_t* out_handle) {
    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);
    uint32_t flags_;
    if (index >= dev->irqs.size() || !out_handle) {
        return ZX_ERR_INVALID_ARGS;
    }
    pbus_irq_t& irq = dev->irqs[index];
    if (flags) {
        flags_ = flags;
    } else {
        flags_ = irq.mode;
    }
    zx_status_t status = zx_interrupt_create(dev->bus->resource, irq.irq, flags_, out_handle);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_interrupt: zx_interrupt_create failed %d\n", status);
        return status;
    }
    return status;
}

static zx_status_t platform_dev_get_bti(void* ctx, uint32_t index, zx_handle_t* out_handle) {
    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);
    iommu_protocol_t* iommu = &dev->bus->iommu;

    if (!iommu->ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }


printf("platform_dev_get_bti %u:%u:%u index %u size %zu\n", dev->vid, dev->pid, dev->did, index, dev->btis.size());


    if (index >= dev->btis.size() || !out_handle) {
        return ZX_ERR_INVALID_ARGS;
    }
    pbus_bti_t& bti = dev->btis[index];

    return iommu_get_bti(iommu, bti.iommu_index, bti.bti_id, out_handle);
}

static zx_status_t platform_dev_get_device_info(void* ctx, pdev_device_info_t* out_info) {
    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);

    memset(out_info, 0, sizeof(*out_info));
    out_info->vid = dev->vid;
    out_info->pid = dev->pid;
    out_info->did = dev->did;
    memcpy(&out_info->serial_port_info, &dev->serial_port_info, sizeof(out_info->serial_port_info));
    out_info->mmio_count = static_cast<uint32_t>(dev->mmios.size());
    out_info->irq_count = static_cast<uint32_t>(dev->irqs.size());
    out_info->gpio_count = static_cast<uint32_t>(dev->gpios.size());
    out_info->i2c_channel_count = static_cast<uint32_t>(dev->i2c_channels.size());
    out_info->clk_count = static_cast<uint32_t>(dev->clks.size());
    out_info->bti_count = static_cast<uint32_t>(dev->btis.size());
    out_info->metadata_count = static_cast<uint32_t>(dev->metadata.size());

    return ZX_OK;
}

static platform_device_protocol_ops_t platform_dev_proto_ops = {
    .map_mmio = platform_dev_map_mmio,
    .map_interrupt = platform_dev_map_interrupt,
    .get_bti = platform_dev_get_bti,
    .get_device_info = platform_dev_get_device_info,
};

static zx_status_t pdev_rpc_get_mmio(platform_dev_t* dev, uint32_t index, zx_off_t* out_offset,
                                     size_t *out_length, zx_paddr_t* out_paddr, zx_handle_t* out_handle,
                                     uint32_t* out_handle_count) {
    if (index >= dev->mmios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }

    pbus_mmio_t& mmio = dev->mmios[index];
    zx_paddr_t vmo_base = ROUNDDOWN(mmio.base, PAGE_SIZE);
    size_t vmo_size = ROUNDUP(mmio.base + mmio.length - vmo_base, PAGE_SIZE);
    zx_status_t status = zx_vmo_create_physical(dev->bus->resource, vmo_base, vmo_size,
                                                out_handle);
    if (status != ZX_OK) {
        zxlogf(ERROR, "pdev_rpc_get_mmio: zx_vmo_create_physical failed %d\n", status);
        return status;
    }
    *out_offset = mmio.base - vmo_base;
    *out_length = mmio.length;
    *out_paddr = vmo_base;
    *out_handle_count = 1;
    return ZX_OK;
}

static zx_status_t pdev_rpc_get_interrupt(platform_dev_t* dev, uint32_t index,
                                          uint32_t flags,
                                          zx_handle_t* out_handle, uint32_t* out_handle_count) {

    zx_status_t status = platform_dev_map_interrupt(dev, index, flags, out_handle);
    if (status == ZX_OK) {
        *out_handle_count = 1;
    }
    return status;
}

static zx_status_t pdev_rpc_get_bti(platform_dev_t* dev, uint32_t index, zx_handle_t* out_handle,
                                    uint32_t* out_handle_count) {

    zx_status_t status = platform_dev_get_bti(dev, index, out_handle);
    if (status == ZX_OK) {
        *out_handle_count = 1;
    }
    return status;
}

static zx_status_t pdev_rpc_ums_set_mode(platform_dev_t* dev, usb_mode_t mode) {
    platform_bus_t* bus = dev->bus;
    if (!bus->ums.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return usb_mode_switch_set_mode(&bus->ums, mode);
}

static zx_status_t pdev_rpc_gpio_config(platform_dev_t* dev, uint32_t index,
                                        uint32_t flags) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;

    return gpio_config(&bus->gpio, index, flags);
}

static zx_status_t pdev_rpc_gpio_set_alt_function(platform_dev_t* dev, uint32_t index,
                                                  uint64_t function) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;

    return gpio_set_alt_function(&bus->gpio, index, function);
}

static zx_status_t pdev_rpc_gpio_read(platform_dev_t* dev, uint32_t index, uint8_t* out_value) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;

    return gpio_read(&bus->gpio, index, out_value);
}

static zx_status_t pdev_rpc_gpio_write(platform_dev_t* dev, uint32_t index, uint8_t value) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;

    return gpio_write(&bus->gpio, index, value);
}

static zx_status_t pdev_rpc_get_gpio_interrupt(platform_dev_t* dev, uint32_t index,
                                               uint32_t flags,
                                               zx_handle_t* out_handle,
                                               uint32_t* out_handle_count) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }

    index = dev->gpios[index].gpio;
    zx_status_t status = gpio_get_interrupt(&bus->gpio, index, flags, out_handle);
    if (status == ZX_OK) {
        *out_handle_count = 1;
    }
    return status;
}

static zx_status_t pdev_rpc_release_gpio_interrupt(platform_dev_t* dev, uint32_t index) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;
    return gpio_release_interrupt(&bus->gpio, index);
}

static zx_status_t pdev_rpc_set_gpio_polarity(platform_dev_t* dev,
                                            uint32_t index, uint32_t flags) {
    platform_bus_t* bus = dev->bus;
    if (!bus->gpio.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->gpios.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    index = dev->gpios[index].gpio;
    return gpio_set_polarity(&bus->gpio, index, flags);
}

static zx_status_t pdev_rpc_canvas_config(platform_dev_t* dev, zx_handle_t vmo, size_t offset,
                                          canvas_info_t* info, uint8_t* canvas_idx) {
    platform_bus_t* bus = dev->bus;
    if (!bus->canvas.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return canvas_config(&bus->canvas, vmo, offset, info, canvas_idx);
}

static zx_status_t pdev_rpc_canvas_free(platform_dev_t* dev, uint8_t canvas_idx) {
    platform_bus_t* bus = dev->bus;
    if (!bus->canvas.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return canvas_free(&bus->canvas, canvas_idx);
}

static zx_status_t pdev_rpc_scpi_get_sensor(platform_dev_t* dev,
                                            char* name,
                                            uint32_t *sensor_id) {
    platform_bus_t* bus = dev->bus;
    if (!bus->scpi.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return scpi_get_sensor(&bus->scpi, name, sensor_id);
}

static zx_status_t pdev_rpc_scpi_get_sensor_value(platform_dev_t* dev,
                                            uint32_t sensor_id,
                                            uint32_t* sensor_value) {
    platform_bus_t* bus = dev->bus;
    if (!bus->scpi.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return scpi_get_sensor_value(&bus->scpi, sensor_id, sensor_value);
}

static zx_status_t pdev_rpc_scpi_get_dvfs_info(platform_dev_t* dev,
                                               uint8_t power_domain,
                                               scpi_opp_t* opps) {
    platform_bus_t* bus = dev->bus;
    if (!bus->scpi.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return scpi_get_dvfs_info(&bus->scpi, power_domain, opps);
}

static zx_status_t pdev_rpc_scpi_get_dvfs_idx(platform_dev_t* dev,
                                              uint8_t power_domain,
                                              uint16_t* idx) {
    platform_bus_t* bus = dev->bus;
    if (!bus->scpi.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return scpi_get_dvfs_idx(&bus->scpi, power_domain, idx);
}

static zx_status_t pdev_rpc_scpi_set_dvfs_idx(platform_dev_t* dev,
                                              uint8_t power_domain,
                                              uint16_t idx) {
    platform_bus_t* bus = dev->bus;
    if (!bus->scpi.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    return scpi_set_dvfs_idx(&bus->scpi, power_domain, idx);
}

static zx_status_t pdev_rpc_i2c_transact(platform_dev_t* dev, pdev_req_t* req, uint8_t* data,
                                        zx_handle_t channel) {
    platform_bus_t* bus = dev->bus;
    if (!bus->i2c.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    uint32_t index = req->index;
    if (index >= dev->i2c_channels.size()) {
        return ZX_ERR_INVALID_ARGS;
    }
    pbus_i2c_channel_t* pdev_channel = &dev->i2c_channels[index];

    return platform_i2c_transact(dev->bus, req, pdev_channel, data, channel);
}

static zx_status_t pdev_rpc_clk_enable(platform_dev_t* dev, uint32_t index) {
    platform_bus_t* bus = dev->bus;
    if (!bus->clk.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->clks.size()) {
        return ZX_ERR_INVALID_ARGS;
    }

    index = dev->clks[index].clk;

    return clk_enable(&bus->clk, index);
}

static zx_status_t pdev_rpc_clk_disable(platform_dev_t* dev, uint32_t index) {
    platform_bus_t* bus = dev->bus;
    if (!bus->clk.ops) {
        return ZX_ERR_NOT_SUPPORTED;
    }
    if (index >= dev->clks.size()) {
        return ZX_ERR_INVALID_ARGS;
    }

    index = dev->clks[index].clk;

    return clk_disable(&bus->clk, index);
}

static zx_status_t platform_dev_rxrpc(void* ctx, zx_handle_t channel) {
    if (channel == ZX_HANDLE_INVALID) {
        // proxy device has connected
        return ZX_OK;
    }

    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);
    struct {
        pdev_req_t req;
        uint8_t data[PDEV_I2C_MAX_TRANSFER_SIZE];
    } req_data;
    pdev_req_t* req = &req_data.req;
    pdev_resp_t resp;
    uint32_t len = sizeof(req_data);
    zx_handle_t in_handle;
    uint32_t in_handle_count = 1;

    zx_status_t status = zx_channel_read(channel, 0, &req_data, &in_handle, len, in_handle_count,
                                        &len, &in_handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_read failed %d\n", status);
        return status;
    }

    resp.txid = req->txid;
    zx_handle_t handle = ZX_HANDLE_INVALID;
    uint32_t handle_count = 0;

    switch (req->op) {
    case PDEV_GET_MMIO:
        resp.status = pdev_rpc_get_mmio(dev, req->index, &resp.mmio.offset, &resp.mmio.length,
                                        &resp.mmio.paddr, &handle, &handle_count);
        break;
    case PDEV_GET_INTERRUPT:
        resp.status = pdev_rpc_get_interrupt(dev, req->index, req->flags, &handle, &handle_count);
        break;
    case PDEV_GET_BTI:
        resp.status = pdev_rpc_get_bti(dev, req->index, &handle, &handle_count);
        break;
    case PDEV_GET_DEVICE_INFO:
         resp.status = platform_dev_get_device_info(dev, &resp.info);
        break;
    case PDEV_UMS_SET_MODE:
        resp.status = pdev_rpc_ums_set_mode(dev, req->usb_mode);
        break;
    case PDEV_GPIO_CONFIG:
        resp.status = pdev_rpc_gpio_config(dev, req->index, req->gpio_flags);
        break;
    case PDEV_GPIO_SET_ALT_FUNCTION:
        resp.status = pdev_rpc_gpio_set_alt_function(dev, req->index, req->gpio_alt_function);
        break;
    case PDEV_GPIO_READ:
        resp.status = pdev_rpc_gpio_read(dev, req->index, &resp.gpio_value);
        break;
    case PDEV_GPIO_WRITE:
        resp.status = pdev_rpc_gpio_write(dev, req->index, req->gpio_value);
        break;
    case PDEV_GPIO_GET_INTERRUPT:
        resp.status = pdev_rpc_get_gpio_interrupt(dev, req->index,
                                                req->flags, &handle, &handle_count);
        break;
    case PDEV_GPIO_RELEASE_INTERRUPT:
        resp.status = pdev_rpc_release_gpio_interrupt(dev, req->index);
        break;
    case PDEV_GPIO_SET_POLARITY:
        resp.status = pdev_rpc_set_gpio_polarity(dev, req->index, req->flags);
        break;
    case PDEV_SCPI_GET_SENSOR:
        resp.status = pdev_rpc_scpi_get_sensor(dev, req->scpi_name, &resp.scpi_sensor_id);
        break;
    case PDEV_SCPI_GET_SENSOR_VALUE:
        resp.status = pdev_rpc_scpi_get_sensor_value(dev, req->scpi_sensor_id,
                                                     &resp.scpi_sensor_value);
        break;
    case PDEV_SCPI_GET_DVFS_INFO:
        resp.status = pdev_rpc_scpi_get_dvfs_info(dev, req->scpi_power_domain,
                                                  &resp.scpi_opps);
        break;
    case PDEV_SCPI_GET_DVFS_IDX:
        resp.status = pdev_rpc_scpi_get_dvfs_idx(dev, req->scpi_power_domain,
                                                 &resp.scpi_dvfs_idx);
        break;
    case PDEV_SCPI_SET_DVFS_IDX:
        resp.status = pdev_rpc_scpi_set_dvfs_idx(dev, req->scpi_power_domain,
                                                 static_cast<uint16_t>(req->index));
        break;
    case PDEV_I2C_GET_MAX_TRANSFER:
        resp.status = i2c_impl_get_max_transfer_size(&dev->bus->i2c, req->index,
                                                     &resp.i2c_max_transfer);
        break;
    case PDEV_I2C_TRANSACT:
        resp.status = pdev_rpc_i2c_transact(dev, req, req_data.data, channel);
        if (resp.status == ZX_OK) {
            // If platform_i2c_transact succeeds, we return immmediately instead of calling
            // zx_channel_write below. Instead we will respond in platform_i2c_complete().
            return ZX_OK;
        }
        break;
    case PDEV_CLK_ENABLE:
        resp.status = pdev_rpc_clk_enable(dev, req->index);
        break;
    case PDEV_CLK_DISABLE:
        resp.status = pdev_rpc_clk_disable(dev, req->index);
        break;
    case PDEV_CANVAS_CONFIG:
        resp.status = pdev_rpc_canvas_config(dev, in_handle,
                                             req->canvas.offset, &req->canvas.info,
                                             &resp.canvas_idx);
        break;
    case PDEV_CANCAS_FREE:
        resp.status = pdev_rpc_canvas_free(dev, req->canvas_idx);
        break;
    default:
        zxlogf(ERROR, "platform_dev_rxrpc: unknown op %u\n", req->op);
        return ZX_ERR_INTERNAL;
    }

    // set op to match request so zx_channel_write will return our response
    status = zx_channel_write(channel, 0, &resp, sizeof(resp),
                              (handle_count == 1 ? &handle : nullptr), handle_count);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_rxrpc: zx_channel_write failed %d\n", status);
    }
    return status;
}

static zx_status_t platform_dev_get_protocol(void* ctx, uint32_t proto_id, void* protocol) {
    platform_dev_t* dev = static_cast<platform_dev_t*>(ctx);

    if (proto_id == ZX_PROTOCOL_PLATFORM_DEV) {
        platform_device_protocol_t* proto = static_cast<platform_device_protocol_t*>(protocol);
        proto->ops = &platform_dev_proto_ops;
        proto->ctx = dev;
        return ZX_OK;
    } else {
        return platform_bus_get_protocol(dev->bus, proto_id, protocol);
    }
}

void platform_dev_free(platform_dev_t* dev) {
    dev->mmios.reset();
    dev->irqs.reset();
    dev->gpios.reset();
    dev->i2c_channels.reset();
    dev->clks.reset();
    dev->btis.reset();
    dev->metadata.reset();
    free(dev);
}

static zx_protocol_device_t platform_dev_proto = {
    .version = DEVICE_OPS_VERSION,
    .get_protocol = platform_dev_get_protocol,
    .open = nullptr,
    .open_at = nullptr,
    .close = nullptr,
    .unbind = nullptr,
    // Note that we do not have a release callback here because we
    // need to support re-adding platform devices when they are reenabled.
    .release = nullptr,
    .read = nullptr,
    .write = nullptr,
    .get_size = nullptr,
    .ioctl = nullptr,
    .suspend = nullptr,
    .resume = nullptr,
    .rxrpc = platform_dev_rxrpc,
    .message = nullptr,
};

zx_status_t platform_device_add(platform_bus_t* bus, const pbus_dev_t* pdev, uint32_t flags) {
    zx_status_t status = ZX_OK;

    if (flags & ~(PDEV_ADD_DISABLED | PDEV_ADD_PBUS_DEVHOST)) {
        return ZX_ERR_INVALID_ARGS;
    }

    platform_dev_t dev = {};

    dev.bus = bus;
    dev.flags = flags;
    if (!pdev->name) {
        return ZX_ERR_INVALID_ARGS;
    }
    strlcpy(dev.name, pdev->name, sizeof(dev.name));
    dev.vid = pdev->vid;
    dev.pid = pdev->pid;
    dev.did = pdev->did;
    memcpy(&dev.serial_port_info, &pdev->serial_port_info, sizeof(dev.serial_port_info));

printf("platform_device_add %u:%u:%u bti_count: %u\n", dev.vid, dev.pid, dev.did, pdev->bti_count);

    fbl::AllocChecker ac;

    if (pdev->mmio_count) {
        dev.mmios.reserve(pdev->mmio_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->mmio_count; i++) {
            dev.mmios.push_back(pdev->mmios[i]);
        }
    }
    if (pdev->irq_count) {
        dev.irqs.reserve(pdev->irq_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->irq_count; i++) {
            dev.irqs.push_back(pdev->irqs[i]);
        }
    }
    if (pdev->gpio_count) {
        dev.gpios.reserve(pdev->gpio_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->gpio_count; i++) {
            dev.gpios.push_back(pdev->gpios[i]);
        }
    }
    if (pdev->i2c_channel_count) {
        dev.i2c_channels.reserve(pdev->i2c_channel_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->i2c_channel_count; i++) {
            dev.i2c_channels.push_back(pdev->i2c_channels[i]);
        }
    }
    if (pdev->clk_count) {
        dev.clks.reserve(pdev->clk_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->clk_count; i++) {
            dev.clks.push_back(pdev->clks[i]);
        }
    }
    if (pdev->bti_count) {
        dev.btis.reserve(pdev->bti_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->bti_count; i++) {
            dev.btis.push_back(pdev->btis[i]);
        }
    }
    if (pdev->metadata_count) {
        dev.metadata.reserve(pdev->metadata_count, &ac);
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < pdev->metadata_count; i++) {
            dev.metadata.push_back(pdev->metadata[i]);
        }
    }

    bus->devices.push_back(fbl::move(dev), &ac);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

     platform_dev_t* devptr = &bus->devices[bus->devices.size() - 1];
printf("MOVED %u:%u:%u bti_count: %zu\n", devptr->vid, devptr->pid, devptr->did, devptr->btis.size());

    if ((flags & PDEV_ADD_DISABLED) == 0) {
        status = platform_device_enable(devptr, true);
    }

    return status;
}

static zx_status_t platform_device_add_metadata(platform_dev_t* dev, uint32_t index) {
    uint32_t type = dev->metadata[index].type;
    uint32_t extra = dev->metadata[index].extra;
    platform_bus_t* bus = dev->bus;
    uint8_t* metadata = bus->metadata;
    zx_off_t offset = 0;

    while (offset < bus->metadata_size) {
        zbi_header_t* header = (zbi_header_t*)metadata;
        size_t length = ZBI_ALIGN(sizeof(zbi_header_t) + header->length);

        if (header->type == type && header->extra == extra) {
            return device_add_metadata(dev->zxdev, type, header + 1, length - sizeof(zbi_header_t));
        }
        metadata += length;
        offset += length;
    }
    return ZX_ERR_NOT_FOUND;
}

zx_status_t platform_device_enable(platform_dev_t* dev, bool enable) {
    zx_status_t status = ZX_OK;

    if (enable && !dev->enabled) {
        zx_device_prop_t props[] = {
            {BIND_PLATFORM_DEV_VID, 0, dev->vid},
            {BIND_PLATFORM_DEV_PID, 0, dev->pid},
            {BIND_PLATFORM_DEV_DID, 0, dev->did},
        };

        char namestr[ZX_DEVICE_NAME_MAX];
        if (dev->vid == PDEV_VID_GENERIC && dev->pid == PDEV_PID_GENERIC &&
            dev->did == PDEV_DID_KPCI) {
            strlcpy(namestr, "pci", sizeof(namestr));
        } else {

            snprintf(namestr, sizeof(namestr), "%02x:%02x:%01x", dev->vid, dev->pid, dev->did);
        }
        char argstr[64];
        snprintf(argstr, sizeof(argstr), "pdev:%s,", namestr);
        bool new_devhost = !(dev->flags & PDEV_ADD_PBUS_DEVHOST);
        device_add_args_t args = {};
        args.version = DEVICE_ADD_ARGS_VERSION;
        args.name = namestr;
        args.ctx = dev;
        args.ops = &platform_dev_proto;
        args.proto_id = ZX_PROTOCOL_PLATFORM_DEV;
        args.props = props;
        args.prop_count = countof(props);
        args.proxy_args = (new_devhost ? argstr : nullptr);
        args.flags = (new_devhost ? DEVICE_ADD_MUST_ISOLATE : 0) |
                     (dev->metadata.size() ? DEVICE_ADD_INVISIBLE : 0);

        // add PCI root at top level
        zx_device_t* parent = dev->bus->zxdev;
        if (dev->did == PDEV_DID_KPCI) {
            parent = device_get_parent(parent);
        }

        if (dev->metadata.size()) {
            // keep device invisible until we add its metadata
            args.flags |= DEVICE_ADD_INVISIBLE;
        }
        status = device_add(parent, &args, &dev->zxdev);
        if (status != ZX_OK) {
            return status;
        }

        if (dev->metadata.size()) {
            for (uint32_t i = 0; i < dev->metadata.size(); i++) {
                pbus_metadata_t* pbm = &dev->metadata[i];
                if (pbm->data && pbm->len) {
                    device_add_metadata(dev->zxdev, pbm->type, pbm->data, pbm->len);
                } else {
                    platform_device_add_metadata(dev, i);
                }
            }
            device_make_visible(dev->zxdev);
        }
     } else if (!enable && dev->enabled) {
        device_remove(dev->zxdev);
        dev->zxdev = nullptr;
    }

    if (status == ZX_OK) {
        dev->enabled = enable;
    }

    return status;
}