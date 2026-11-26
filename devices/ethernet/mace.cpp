/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/** @file Media Access Controller for Ethernet (MACE) emulation. */

#include <devices/common/dbdma.h>
#include <devices/deviceregistry.h>
#include <frontend/runtime.h>
#include <devices/ethernet/mace.h>
#include <loguru.hpp>
#include <machines/machinebase.h>
#include <machines/machineproperties.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

using namespace MaceEnet;

int MaceController::device_postinit()
{
    this->enabled = GET_BIN_PROP("net_enabled");
    this->int_ctrl = dynamic_cast<InterruptCtrl*>(
        gMachineObj->get_comp_by_type(HWCompType::INT_CTRL));
    if (this->int_ctrl) {
        this->irq_id = this->int_ctrl->register_dev_int(IntSrc::ETHERNET);
    }
    return 0;
}

uint8_t MaceController::read(uint8_t reg_offset)
{
    if (!this->enabled)
        return 0xFF;
    this->ensure_network();
    if (reg_offset >= 8 && reg_offset <= 18) {
        LOG_F(INFO, "MACE read reg %d", reg_offset);
    }

    switch (reg_offset) {
    case MaceReg::Rcv_FIFO:
        if (!this->rx_fifo.empty()) {
            uint8_t v = this->rx_fifo.front();
            this->rx_fifo.pop_front();
            return v;
        }
        return 0;
    case MaceReg::Rcv_Frame_Ctrl:
        return this->rcv_fc;
    case MaceReg::Xmit_Frame_Stat:
        return this->xmt_fs;
    case MaceReg::Xmit_Retry_Cnt:
        return this->xmt_retry;
    case MaceReg::Rcv_Frame_Stat:
        return this->rcv_fs;
    case MaceReg::FIFO_Frame_Cnt:
        return this->fifo_fc;
    case MaceReg::Interrupt: {
        uint8_t ret_val = this->int_stat;
        this->int_stat = 0;
        if (this->int_ctrl)
            this->int_ctrl->ack_int(this->irq_id, 0);
        LOG_F(9, "%s: all interrupt flags cleared", this->name.c_str());
        return ret_val;
    }
    case MaceReg::Interrupt_Mask:
        return this->int_mask;
    case MaceReg::Poll:
        this->poll_reg = 0;
        if (this->mac_cfg & 0x02) this->poll_reg |= 0x40; // TDTREQ
        if (this->mac_cfg & 0x01) this->poll_reg |= 0x20; // RDTREQ
        if (this->xmt_fs & 0x80) this->poll_reg |= 0x80;  // XMTSV
        return this->poll_reg;
    case MaceReg::BIU_Config_Ctrl:
        return this->biu_ctrl;
    case MaceReg::FIFO_Config:
        return this->fifo_ctrl;
    case MaceReg::MAC_Config_Ctrl:
        return this->mac_cc;
    case MaceReg::PLS_Config_Ctrl:
        return this->pls_cc;
    case MaceReg::PHY_Config_Ctrl:
        return this->phy_cc;
    case MaceReg::Chip_ID_Lo:
        return this->chip_id & 0xFFU;
    case MaceReg::Chip_ID_Hi:
        return (this->chip_id >> 8) & 0xFFU;
    case MaceReg::Int_Addr_Config:
        return this->addr_cfg;
    case MaceReg::Missed_Pkt_Cnt:
        return this->missed_pkts;
    default:
        LOG_F(INFO, "%s: reading from register %d", this->name.c_str(), reg_offset);
    }

    return 0;
}

void MaceController::write(uint8_t reg_offset, uint8_t value)
{
    if (!this->enabled)
        return;
    this->ensure_network();
    if (reg_offset <= 29) {
        LOG_F(INFO, "MACE write reg %d = 0x%02X", reg_offset, value);
    }

    switch(reg_offset) {
    case MaceReg::Rcv_Frame_Ctrl:
        this->rcv_fc = value;
        break;
    case MaceReg::Xmit_Frame_Ctrl:
        this->send_tx_frame();
        break;
    case MaceReg::Interrupt_Mask:
        this->int_mask = value;
        break;
    case MaceReg::BIU_Config_Ctrl:
        if (value & BIU_SWRST) {
            LOG_F(INFO, "%s: soft reset asserted", this->name.c_str());
            value &= ~BIU_SWRST; // acknowledge soft reset
            this->mac_cfg = 0;
            this->mac_cc = 0;
            this->pls_cc = 0;
            this->phy_cc = 0;
            this->fifo_ctrl = 0;
            this->rcv_fc = 1;
            this->int_stat = 0;
            this->xmt_fs = 0;
            this->rcv_fs = 0;
            this->tx_frame.clear();
            this->rx_queue.clear();
        }
        this->biu_ctrl = value;
        break;
    case MaceReg::FIFO_Config:
        this->fifo_ctrl = value;
        break;
    case MaceReg::MAC_Config_Ctrl:
        this->mac_cfg = value;
        break;
    case MaceReg::PLS_Config_Ctrl:
        this->pls_cc = value & 0x0F;
        break;
    case MaceReg::PHY_Config_Ctrl:
        this->phy_cc = value & ~0x80; // link is always up in user-mode networking
        break;
    case MaceReg::User_Test:
        this->user_test = value;
        if ((value & 0x06) && (this->mac_cfg & 0x01)) {
            TimerManager::get_instance()->add_oneshot_timer(
                this->loopback_timer, MSECS_TO_NSECS(150),
                [this](uint64_t, uint64_t) {
                    if (!(this->user_test & 0x06) || !(this->mac_cfg & 0x01))
                        return;
                    std::vector<uint8_t> pkt(60, 0);
                    for (int i = 0; i < 6; i++) {
                        uint8_t mac = (this->phys_addr >> (i * 8)) & 0xFF;
                        pkt[i] = mac;
                        pkt[6 + i] = mac;
                    }
                    pkt[12] = 0x08;
                    pkt[13] = 0x00;
                    this->rx_fifo.insert(this->rx_fifo.end(), pkt.begin(), pkt.end());
                    this->int_stat |= 0x02; // RCVINT
                    this->raise_interrupt();
                });
        }
        break;
    case MaceReg::Int_Addr_Config:
        if ((value & IAC_LOGADDR) && (value & IAC_PHYADDR))
            value &= ~IAC_PHYADDR;
        if (value & (IAC_LOGADDR | IAC_PHYADDR))
            this->addr_ptr = 0;
        // ADDRCHG is self-clearing on the real part.
        this->addr_cfg = value & ~IAC_ADDRCHG;
        break;
    case MaceReg::Log_Addr_Flt:
        if (this->addr_cfg & IAC_LOGADDR) {
            uint64_t mask = ~(0xFFULL << (this->addr_ptr * 8));
            this->log_addr = (this->log_addr & mask) | ((uint64_t)value << (this->addr_ptr * 8));
            if (++this->addr_ptr >= 8) {
                this->addr_cfg &= ~IAC_LOGADDR;
                this->addr_ptr = 0;
            }
        }
        break;
    case MaceReg::Phys_Addr:
        if (this->addr_cfg & IAC_PHYADDR) {
            uint64_t mask = ~(0xFFULL << (this->addr_ptr * 8));
            this->phys_addr = (this->phys_addr & mask) | ((uint64_t)value << (this->addr_ptr * 8));
            if (++this->addr_ptr >= 6) {
                this->addr_cfg &= ~IAC_PHYADDR;
                this->addr_ptr = 0;
            }
        }
        break;
    default:
        LOG_F(INFO, "%s: writing 0x%X to register %d", this->name.c_str(),
              value, reg_offset);
    }
}

void MaceController::ensure_network()
{
    if (this->net_started)
        return;
    this->net_started = true;
    this->net_backend = std::make_unique<SlirpBackend>();
    this->net_backend->init([this](const uint8_t* frame, int len) {
        this->receive_frame(frame, len);
    });
}

int MaceController::xfer_to(DmaChannel* ch_obj, uint8_t* buf, int len)
{
    this->ensure_network();
    if (len > 0) {
        this->tx_frame.insert(this->tx_frame.end(), buf, buf + len);
        this->send_tx_frame();
    }
    return len;
}

int MaceController::xfer_from(DmaChannel* ch_obj, uint8_t* buf, int len)
{
    if (this->rx_queue.empty() || len <= 0) {
        return 0;
    }

    std::vector<uint8_t>& frame = this->rx_queue.front();
    int copy_len = std::min((int)frame.size(), len);
    std::memcpy(buf, frame.data(), copy_len);
    if (copy_len == (int)frame.size()) {
        this->rx_queue.pop_front();
    } else {
        frame.erase(frame.begin(), frame.begin() + copy_len);
    }
    return copy_len;
}

void MaceController::notify(DmaChannel* ch_obj, DmaMsg msg)
{
    if (msg == DMA_MSG_START || msg == DMA_MSG_DATA_AVAIL) {
        this->try_receive();
    }
}

void MaceController::send_tx_frame()
{
    if (this->tx_frame.size() >= 14 && this->net_backend && this->net_backend->is_ready()) {
        if (this->user_test & 0x06) { // internal/external loopback
            this->rx_fifo.insert(this->rx_fifo.end(), this->tx_frame.begin(), this->tx_frame.end());
            this->int_stat |= 0x02; // RCVINT
        } else {
            this->net_backend->input_frame(this->tx_frame.data(), (int)this->tx_frame.size());
        frontend_activity(DeviceActivity::Network, true);
        }
        this->xmt_fs = 0x80; // XMTSV: transmit status valid, no errors
        this->int_stat |= 0x01; // XMTINT
        this->raise_interrupt();
    }
    this->tx_frame.clear();
}

void MaceController::raise_interrupt()
{
    if (this->int_ctrl && (this->int_stat & ~this->int_mask)) {
        this->int_ctrl->ack_int(this->irq_id, 1);
    }
}

void MaceController::receive_frame(const uint8_t* frame, int len)
{
    if (!this->enabled || !frame || len <= 0) {
        return;
    }

    std::vector<uint8_t> pkt(16 + len, 0);
    pkt[0] = len & 0xFF;
    pkt[2] = (len >> 8) & 0x0F;
    std::memcpy(pkt.data() + 16, frame, len);
    this->rx_queue.push_back(std::move(pkt));
    frontend_activity(DeviceActivity::Network, false);
    this->int_stat |= 0x02; // RCVINT
    this->raise_interrupt();
    this->try_receive();
}

void MaceController::try_receive()
{
    DMAChannel* rx_dma = dynamic_cast<DMAChannel*>(this->channel_obj);
    while (!this->rx_queue.empty() && rx_dma) {
        std::vector<uint8_t>& frame = this->rx_queue.front();
        DmaPushResult res = rx_dma->push_data(
            reinterpret_cast<const char*>(frame.data()), (int)frame.size());
        if (res != DmaPushResult::PushedData) {
            break;
        }
        this->rx_queue.pop_front();
    }
}

static const PropMap Mace_properties = {
    {"net_enabled", new BinProperty(1)},
};

static const DeviceDescription Mace_Descriptor = {
    MaceController::create, {}, Mace_properties,
    HWCompType::MMIO_DEV | HWCompType::ETHER_MAC
};

REGISTER_DEVICE(Mace, Mace_Descriptor);
